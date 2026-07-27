#pragma once
// arbiter/include/idempotency_cache.h
//
// Dedup layer for the `Idempotency-Key` HTTP header in front of
// POST /v1/orchestrate, /messages, and /agents/:id/chat: a client that
// retries a write with the same key gets back the same SSE stream /
// event log as the original execution rather than triggering a second
// one.
//
// Scope is per-tenant (so two tenants can independently use the
// string "abc" as a key without collision).  With a TenantStore bound
// via bind_store(), mappings also survive process restart — SQLite is
// the source of truth and the in-process table is an L1 cache.  Without
// a store the cache is process-local (tests / CLI helpers).
//
// TTL: 24h.  Expired entries are evicted lazily on `get`/`put`, and a
// full sweep runs amortized inside `put` (every kPruneEvery inserts) so
// unique keys — the common case, since clients mint a fresh key per
// request — can't grow the table unboundedly.  With a store bound the
// amortized sweep also prunes SQLite.  `prune_expired` remains
// callable for an explicit sweep (tests, API-server startup).

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace arbiter {

class TenantStore;

class IdempotencyCache {
public:
    struct Entry {
        std::string                          request_id;
        std::chrono::steady_clock::time_point created_at;
        // Wall-clock stamp from SQLite (epoch seconds).  Non-zero when
        // the entry was loaded from or written through the durable
        // store — L1 expiry then follows wall TTL so rehydration
        // cannot extend the documented 24h window.
        int64_t                              wall_created_at = 0;
    };

    explicit IdempotencyCache(
        std::chrono::nanoseconds ttl =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::hours(24)))
        : ttl_(ttl) {
        // Wall-clock TTL for the durable table; clamp to at least 1s so
        // a sub-second test TTL still expires store rows.
        ttl_seconds_ = std::chrono::duration_cast<std::chrono::seconds>(ttl).count();
        if (ttl_seconds_ < 1) ttl_seconds_ = 1;
    }

    // Bind (or unbind with nullptr) the durable TenantStore backend.
    // After bind, get/put write-through to SQLite so mappings survive
    // process restart.  Safe to call once during ApiServer construction.
    void bind_store(TenantStore* store);

    // Look up a (tenant_id, key) pair.  Returns the entry if present
    // and unexpired.  Expired entries are evicted as a side effect.
    // With a store bound, an L1 miss falls through to SQLite and
    // rehydrates the in-process table on hit.
    std::optional<Entry> get(int64_t tenant_id, const std::string& key);

    // Reserve a (tenant_id, key) → request_id mapping.  Returns true on
    // first insertion; false if a different entry already claims the
    // slot (the caller should fall back to `get()` to retrieve the
    // canonical request_id and stream the replay).  Idempotent for
    // the same (tenant_id, key, request_id) triple.  With a store
    // bound, SQLite is authoritative — a post-restart put that
    // collides with a durable row loses even when L1 is empty.
    bool put(int64_t tenant_id, const std::string& key,
             const std::string& request_id);

    // Drop entries older than the TTL.  Cheap O(N) sweep over L1; also
    // prunes the durable table when a store is bound.  Runs amortized
    // from put() and may be called explicitly.  Safe to call
    // concurrently with get/put.
    void prune_expired();

    // Visible only for tests.
    size_t size() const;

private:
    static std::string make_key(int64_t tenant_id, const std::string& k) {
        return std::to_string(tenant_id) + ":" + k;
    }

    static int64_t wall_now_seconds();

    bool l1_expired_locked(const Entry& e,
                           std::chrono::steady_clock::time_point now,
                           int64_t wall_now) const;

    void prune_expired_locked(std::chrono::steady_clock::time_point now,
                              int64_t wall_now);
    void prune_store(TenantStore* store) const;
    void remember_locked(const std::string& k, const std::string& request_id,
                         std::chrono::steady_clock::time_point now,
                         int64_t wall_created_at);

    static constexpr int kPruneEvery = 512;   // puts between amortized sweeps

    mutable std::mutex                       mu_;
    std::unordered_map<std::string, Entry>   table_;
    std::chrono::nanoseconds                 ttl_;
    int64_t                                  ttl_seconds_ = 86400;
    int                                      puts_since_prune_ = 0;
    TenantStore*                             store_ = nullptr;
};

} // namespace arbiter
