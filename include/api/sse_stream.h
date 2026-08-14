#pragma once
// arbiter/include/api/sse_stream.h
//
// Thread-safe SSE response framer with optional durable event persistence.

#include "json.h"
#include "request_event_bus.h"
#include "tenant_store.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace arbiter {

class SseStream {
public:
    explicit SseStream(int fd);
    ~SseStream();

    void write_headers();

    void set_persistence(TenantStore* ts, RequestEventBus* bus,
                         int64_t tenant_id, std::string request_id);

    void emit(const std::string& event, std::shared_ptr<JsonValue> data);
    void close();

private:
    void start_heartbeat_locked();
    void stop_heartbeat();
    void persist_event_locked(const std::string& kind,
                              const std::string& payload_json);
    void try_coalesce_text_locked(const std::shared_ptr<JsonValue>& data,
                                  const std::string& payload_json);
    void flush_pending_text_locked();

    int        fd_;
    std::mutex mu_;
    bool       closed_ = false;

    TenantStore*       ts_ = nullptr;
    RequestEventBus*   bus_ = nullptr;
    int64_t            persist_tenant_id_ = 0;
    std::string        persist_request_id_;
    int64_t            seq_ = 0;

    std::shared_ptr<JsonValue> pending_text_first_;
    std::string                pending_text_key_;
    std::string                pending_text_concat_;
    size_t                     pending_text_size_ = 0;
    static constexpr size_t    kCoalesceThreshold = 2048;

    std::thread                       hb_thread_;
    std::condition_variable           hb_cv_;
    bool                              hb_stop_ = false;
    std::chrono::steady_clock::time_point last_write_{};
    static constexpr int              kHeartbeatSeconds      = 30;
    static constexpr int              kHeartbeatTickSeconds  = 5;
};

} // namespace arbiter
