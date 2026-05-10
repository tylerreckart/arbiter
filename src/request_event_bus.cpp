// arbiter/src/request_event_bus.cpp

#include "request_event_bus.h"

#include <vector>

namespace arbiter {

int64_t RequestEventBus::subscribe(const std::string& request_id, Callback cb) {
    if (!cb || request_id.empty()) return 0;
    int64_t id = next_id_.fetch_add(1);
    std::lock_guard<std::mutex> lk(mu_);
    subs_[id] = Sub{request_id, std::move(cb)};
    return id;
}

void RequestEventBus::unsubscribe(int64_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    subs_.erase(id);
}

void RequestEventBus::publish(const RequestEventEnvelope& env) {
    // Snapshot matching callbacks under the lock, fan out outside it
    // so a callback that re-enters publish() (or unsubscribe()) can't
    // deadlock and the lock window stays microsecond-scale.
    std::vector<Callback> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets.reserve(subs_.size());
        for (auto& [id, s] : subs_) {
            if (s.request_id == env.request_id) targets.push_back(s.cb);
        }
    }
    for (auto& cb : targets) {
        try { cb(env); } catch (...) { /* publishers can't catch all */ }
    }
}

} // namespace arbiter
