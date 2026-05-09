// arbiter/src/notification_bus.cpp

#include "notification_bus.h"

#include <vector>

namespace arbiter {

int64_t NotificationBus::subscribe(int64_t tenant_id, Callback cb) {
    if (!cb) return 0;
    int64_t id = next_id_.fetch_add(1);
    std::lock_guard<std::mutex> lk(mu_);
    subs_[id] = Sub{tenant_id, std::move(cb)};
    return id;
}

void NotificationBus::unsubscribe(int64_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    subs_.erase(id);
}

void NotificationBus::publish(const Notification& n) {
    // Snapshot matching callbacks under the lock, then fire outside it.
    // Lets a callback re-enter publish() (or unsubscribe()) without
    // deadlocking, and keeps the lock window short.
    std::vector<Callback> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets.reserve(subs_.size());
        for (auto& [id, s] : subs_) {
            if (s.tenant_id == n.tenant_id) targets.push_back(s.cb);
        }
    }
    for (auto& cb : targets) {
        try { cb(n); } catch (...) { /* publishers can't catch all */ }
    }
}

const char* notification_kind_str(Notification::Kind k) {
    switch (k) {
        case Notification::Kind::RunStarted:   return "run.started";
        case Notification::Kind::RunCompleted: return "run.completed";
        case Notification::Kind::RunFailed:    return "run.failed";
    }
    return "unknown";
}

} // namespace arbiter
