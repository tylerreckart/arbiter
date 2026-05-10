#pragma once
// arbiter/include/request_event_bus.h
//
// In-process pub/sub keyed by request_id.  The SSE writer publishes one
// payload per persisted event; subscribers (the resubscribe handler that
// answers GET /v1/requests/:id/events for a still-running request, or
// the A2A tasks/resubscribe handler) wake and stream the new event onto
// their own connection.
//
// Lifecycle: subscribers register a callback, get back a non-zero
// subscription id, and MUST call unsubscribe(id) when their connection
// closes.  The bus does no persistence — durable replay comes from
// `request_events` rows on TenantStore.  A subscriber that misses an
// event due to disconnection re-syncs by reconnecting with the last
// seen seq.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace arbiter {

struct RequestEventEnvelope {
    std::string request_id;
    int64_t     seq          = 0;
    std::string event_kind;
    std::string payload_json;
    int64_t     created_at_ms = 0;
    bool        terminal     = false;     // true on the run's final `done` event
};

class RequestEventBus {
public:
    using Callback = std::function<void(const RequestEventEnvelope&)>;

    int64_t subscribe(const std::string& request_id, Callback cb);
    void    unsubscribe(int64_t id);

    void publish(const RequestEventEnvelope& env);

private:
    struct Sub {
        std::string request_id;
        Callback    cb;
    };
    mutable std::mutex          mu_;
    std::map<int64_t, Sub>      subs_;
    std::atomic<int64_t>        next_id_{1};
};

} // namespace arbiter
