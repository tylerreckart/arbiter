// arbiter/src/idempotency_cache.cpp

#include "idempotency_cache.h"

#include "tenant_store.h"

#include <chrono>

namespace arbiter {

int64_t IdempotencyCache::wall_now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void IdempotencyCache::bind_store(TenantStore* store) {
    std::lock_guard<std::mutex> lk(mu_);
    store_ = store;
}

bool IdempotencyCache::l1_expired_locked(
        const Entry& e,
        std::chrono::steady_clock::time_point now,
        int64_t wall_now) const {
    if (e.wall_created_at > 0) {
        return wall_now - e.wall_created_at >= ttl_seconds_;
    }
    return now - e.created_at >= ttl_;
}

void IdempotencyCache::remember_locked(
        const std::string& k, const std::string& request_id,
        std::chrono::steady_clock::time_point now,
        int64_t wall_created_at) {
    table_[k] = Entry{request_id, now, wall_created_at};
}

void IdempotencyCache::prune_store(TenantStore* store) const {
    if (!store) return;
    store->prune_idempotency_keys(wall_now_seconds() - ttl_seconds_);
}

std::optional<IdempotencyCache::Entry>
IdempotencyCache::get(int64_t tenant_id, const std::string& key) {
    if (key.empty()) return std::nullopt;
    const std::string k = make_key(tenant_id, key);
    auto now = std::chrono::steady_clock::now();
    const int64_t wall_now = wall_now_seconds();

    TenantStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        store = store_;
        auto it = table_.find(k);
        if (it != table_.end()) {
            if (l1_expired_locked(it->second, now, wall_now)) {
                table_.erase(it);
            } else {
                return it->second;
            }
        }
    }

    if (!store) return std::nullopt;

    auto durable = store->get_idempotency_key(tenant_id, key, ttl_seconds_);
    if (!durable) return std::nullopt;

    std::lock_guard<std::mutex> lk(mu_);
    remember_locked(k, durable->request_id, now, durable->created_at);
    return Entry{durable->request_id, now, durable->created_at};
}

bool IdempotencyCache::put(int64_t tenant_id, const std::string& key,
                            const std::string& request_id) {
    if (key.empty() || request_id.empty()) return false;
    const std::string k = make_key(tenant_id, key);
    auto now = std::chrono::steady_clock::now();
    const int64_t wall_now = wall_now_seconds();

    TenantStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        store = store_;
    }

    if (store) {
        // SQLite is authoritative across restarts.  A lost race against
        // an existing durable row must not leave L1 pointing at the
        // loser's request_id.  Keep store I/O outside mu_ so get()'s
        // fall-through path can't contend with a held lock.
        const bool ok = store->put_idempotency_key(
            tenant_id, key, request_id, ttl_seconds_);
        std::optional<TenantStore::IdempotencyMapping> durable;
        if (!ok) {
            durable = store->get_idempotency_key(
                tenant_id, key, ttl_seconds_);
        }
        bool do_store_prune = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (++puts_since_prune_ >= kPruneEvery) {
                puts_since_prune_ = 0;
                prune_expired_locked(now, wall_now);
                do_store_prune = true;
            }
            if (ok) {
                remember_locked(k, request_id, now, wall_now);
            } else if (durable) {
                remember_locked(k, durable->request_id, now,
                                durable->created_at);
            }
        }
        if (do_store_prune) prune_store(store);
        if (ok) return true;
        if (durable) return durable->request_id == request_id;
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    // Amortized sweep: well-behaved clients send a fresh key per request,
    // so expired entries are almost never revisited by get() and would
    // otherwise accumulate for the life of the process.  An O(N) sweep
    // every kPruneEvery inserts keeps the table bounded at roughly the
    // insert rate × TTL without a dedicated timer thread.
    if (++puts_since_prune_ >= kPruneEvery) {
        puts_since_prune_ = 0;
        prune_expired_locked(now, wall_now);
    }
    auto it = table_.find(k);
    if (it != table_.end()) {
        if (l1_expired_locked(it->second, now, wall_now)) {
            it->second = {request_id, now, 0};
            return true;
        }
        // Same request_id → idempotent insert (no change).  Different
        // request_id → race; caller falls back to get() to retrieve
        // the canonical id.
        return it->second.request_id == request_id;
    }
    table_.emplace(k, Entry{request_id, now, 0});
    return true;
}

void IdempotencyCache::prune_expired() {
    auto now = std::chrono::steady_clock::now();
    const int64_t wall_now = wall_now_seconds();
    TenantStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        store = store_;
        prune_expired_locked(now, wall_now);
    }
    prune_store(store);
}

void IdempotencyCache::prune_expired_locked(
        std::chrono::steady_clock::time_point now,
        int64_t wall_now) {
    for (auto it = table_.begin(); it != table_.end(); ) {
        if (l1_expired_locked(it->second, now, wall_now)) {
            it = table_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t IdempotencyCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return table_.size();
}

} // namespace arbiter
