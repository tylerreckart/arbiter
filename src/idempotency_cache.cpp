// arbiter/src/idempotency_cache.cpp

#include "idempotency_cache.h"

namespace arbiter {

std::optional<IdempotencyCache::Entry>
IdempotencyCache::get(int64_t tenant_id, const std::string& key) {
    if (key.empty()) return std::nullopt;
    const std::string k = make_key(tenant_id, key);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(k);
    if (it == table_.end()) return std::nullopt;
    if (now - it->second.created_at >= ttl_) {
        table_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

bool IdempotencyCache::put(int64_t tenant_id, const std::string& key,
                            const std::string& request_id) {
    if (key.empty() || request_id.empty()) return false;
    const std::string k = make_key(tenant_id, key);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(mu_);
    auto it = table_.find(k);
    if (it != table_.end()) {
        if (now - it->second.created_at >= ttl_) {
            it->second = {request_id, now};
            return true;
        }
        // Same request_id → idempotent insert (no change).  Different
        // request_id → race; caller falls back to get() to retrieve
        // the canonical id.
        return it->second.request_id == request_id;
    }
    table_.emplace(k, Entry{request_id, now});
    return true;
}

void IdempotencyCache::prune_expired() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = table_.begin(); it != table_.end(); ) {
        if (now - it->second.created_at >= ttl_) {
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
