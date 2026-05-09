#pragma once
// arbiter/include/notification_bus.h
//
// In-process pub/sub keyed by tenant_id.  The Scheduler publishes a
// Notification when a TaskRun reaches a terminal state; subscribers
// (typically the long-lived /v1/notifications/stream SSE handler) wake
// and emit the event to their clients.
//
// Lifecycle: subscribers register a callback and get back a non-zero
// subscription id; they MUST call unsubscribe(id) before their callback
// goes out of scope (the SSE handler does this on connection close).
// The bus does no persistence — durable replay comes from the DB
// (`GET /v1/runs?since=<epoch>`).  A subscriber that misses an event due
// to disconnection re-syncs by polling the runs endpoint with its last
// seen `started_at`.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace arbiter {

struct Notification {
    enum class Kind { RunStarted, RunCompleted, RunFailed };
    Kind        kind        = Kind::RunCompleted;
    int64_t     tenant_id   = 0;
    int64_t     task_id     = 0;
    int64_t     run_id      = 0;
    std::string status;             // matches TaskRun.status
    std::string agent_id;
    int64_t     started_at  = 0;
    int64_t     completed_at = 0;
    std::string result_summary;     // truncated by publisher
    std::string error_message;
};

class NotificationBus {
public:
    using Callback = std::function<void(const Notification&)>;

    // Returns a positive subscription id.  `cb` fires for every publish
    // whose Notification.tenant_id matches `tenant_id`.  `cb` is invoked
    // on the publisher's thread, so subscribers should hand the event
    // to a local queue and return quickly.
    int64_t subscribe(int64_t tenant_id, Callback cb);

    // Idempotent — unknown ids are no-ops.
    void unsubscribe(int64_t id);

    // Fan out to every subscriber whose tenant_id matches.
    void publish(const Notification& n);

private:
    struct Sub {
        int64_t  tenant_id = 0;
        Callback cb;
    };
    mutable std::mutex          mu_;
    std::map<int64_t, Sub>      subs_;
    std::atomic<int64_t>        next_id_{1};
};

const char* notification_kind_str(Notification::Kind k);

} // namespace arbiter
