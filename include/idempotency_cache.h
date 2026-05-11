#pragma once
// arbiter/include/idempotency_cache.h
//
// In-memory cache for the `Idempotency-Key` HTTP header.  Servers as
// the dedup layer in front of POST /v1/orchestrate, /messages, and
// /agents/:id/chat: a client that retries a write with the same key
// gets back the same SSE stream / event log as the original execution
// rather than triggering a second one.
//
// Scope is per-tenant (so two tenants can independently use the
// string "abc" as a key without collision) and per-process (a server
// restart loses the table; a retry after restart will re-execute).
// Durable dedup is a Phase-3 follow-up that requires the recovery
// sweep to actually resume orphans.
//
// TTL: 24h.  Expired entries are evicted lazily on `get`/`put` and
// on a periodic background sweep driven by `prune_expired`.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace arbiter {

class IdempotencyCache {
public:
    struct Entry {
        std::string                          request_id;
        std::chrono::steady_clock::time_point created_at;
    };

    explicit IdempotencyCache(
        std::chrono::nanoseconds ttl =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::hours(24)))
        : ttl_(ttl) {}

    // Look up a (tenant_id, key) pair.  Returns the entry if present
    // and unexpired.  Expired entries are evicted as a side effect.
    std::optional<Entry> get(int64_t tenant_id, const std::string& key);

    // Reserve a (tenant_id, key) → request_id mapping.  Returns true on
    // first insertion; false if a different entry already claims the
    // slot (the caller should fall back to `get()` to retrieve the
    // canonical request_id and stream the replay).  Idempotent for
    // the same (tenant_id, key, request_id) triple.
    bool put(int64_t tenant_id, const std::string& key,
             const std::string& request_id);

    // Drop entries older than the TTL.  Cheap O(N) sweep; intended for
    // a periodic background tick.  Safe to call concurrently with
    // get/put.
    void prune_expired();

    // Visible only for tests.
    size_t size() const;

private:
    static std::string make_key(int64_t tenant_id, const std::string& k) {
        return std::to_string(tenant_id) + ":" + k;
    }

    mutable std::mutex                       mu_;
    std::unordered_map<std::string, Entry>   table_;
    std::chrono::nanoseconds                 ttl_;
};

} // namespace arbiter
