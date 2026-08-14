// arbiter/src/api/sse_stream.cpp

#include "api/sse_stream.h"
#include "api/http_transport.h"

#include "json.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace arbiter {

SseStream::SseStream(int fd) : fd_(fd) {}
SseStream::~SseStream() { stop_heartbeat(); }
void SseStream::write_headers() {
    // Standard SSE headers.  X-Accel-Buffering: no tells nginx and
    // similar proxies to not buffer the response — without it, events
    // stall until the buffer fills.  Connection: close is fine for
    // our one-request-per-connection model.  CORS is rebuilt per
    // request so ARBITER_CORS_ORIGINS / Origin echoing stays correct.
    const std::string hdr =
        std::string("HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "X-Accel-Buffering: no\r\n") +
        current_cors_headers() +
        "Connection: close\r\n\r\n";
    std::lock_guard<std::mutex> lk(mu_);
    write_all(fd_, hdr);
    last_write_ = std::chrono::steady_clock::now();
    start_heartbeat_locked();
}
void SseStream::set_persistence(TenantStore* ts, RequestEventBus* bus,
                      int64_t tenant_id, std::string request_id) {
    std::lock_guard<std::mutex> lk(mu_);
    ts_ = ts;
    bus_ = bus;
    persist_tenant_id_ = tenant_id;
    persist_request_id_ = std::move(request_id);
}
void SseStream::emit(const std::string& event, std::shared_ptr<JsonValue> data) {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) return;
    std::string payload = data ? json_serialize(*data) : "{}";
    std::string frame = "event: " + event + "\ndata: " + payload + "\n\n";
    write_all(fd_, frame);
    last_write_ = std::chrono::steady_clock::now();

    if (!ts_) return;
    if (event == "text") {
        try_coalesce_text_locked(data, payload);
    } else {
        flush_pending_text_locked();
        persist_event_locked(event, payload);
    }
}
void SseStream::close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        flush_pending_text_locked();
        closed_ = true;
    }
    hb_cv_.notify_all(); // wake heartbeat thread; join deferred to destructor
}
void SseStream::start_heartbeat_locked() {
    if (hb_thread_.joinable()) return;
    hb_thread_ = std::thread([this]() {
        using clock = std::chrono::steady_clock;
        std::unique_lock<std::mutex> lk(mu_);
        while (!hb_stop_ && !closed_) {
            hb_cv_.wait_for(lk, std::chrono::seconds(kHeartbeatTickSeconds));
            if (hb_stop_ || closed_) break;
            auto now = clock::now();
            if (now - last_write_ < std::chrono::seconds(kHeartbeatSeconds))
                continue;
            static const std::string kHb = ": heartbeat\n\n";
            write_all(fd_, kHb);
            last_write_ = now;
        }
    });
}
void SseStream::stop_heartbeat() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        hb_stop_ = true;
    }
    hb_cv_.notify_all();
    if (hb_thread_.joinable()) hb_thread_.join();
}
void SseStream::persist_event_locked(const std::string& kind,
                            const std::string& payload_json) {
    if (!ts_) return;
    ++seq_;
    try {
        ts_->append_request_event(persist_tenant_id_,
                                   persist_request_id_,
                                   seq_, kind, payload_json);
    } catch (...) {
        return; // persistence is best-effort; don't kill the wire stream
    }
    if (bus_) {
        RequestEventEnvelope env;
        env.request_id    = persist_request_id_;
        env.seq           = seq_;
        env.event_kind    = kind;
        env.payload_json  = payload_json;
        env.terminal      = (kind == "done");
        try { bus_->publish(env); } catch (...) {}
    }
}
void SseStream::try_coalesce_text_locked(const std::shared_ptr<JsonValue>& data,
                                const std::string& payload_json) {
    if (!data || !data->is_object()) {
        // Unknown shape — persist as-is; coalescing only applies
        // to the canonical {agent, stream_id, delta} envelope.
        persist_event_locked("text", payload_json);
        return;
    }
    auto agent_v  = data->get("agent");
    auto stream_v = data->get("stream_id");
    auto delta_v  = data->get("delta");
    if (!agent_v  || !agent_v->is_string()  ||
        !stream_v || !stream_v->is_number() ||
        !delta_v  || !delta_v->is_string()) {
        persist_event_locked("text", payload_json);
        return;
    }
    std::string key = agent_v->as_string() + "|" +
                      std::to_string(static_cast<int64_t>(stream_v->as_number()));
    if (pending_text_size_ > 0 && key != pending_text_key_) {
        flush_pending_text_locked();
    }
    if (pending_text_size_ == 0) {
        pending_text_first_ = data;
        pending_text_key_   = key;
        pending_text_concat_.clear();
    }
    pending_text_concat_ += delta_v->as_string();
    pending_text_size_   += delta_v->as_string().size();
    if (pending_text_size_ >= kCoalesceThreshold) {
        flush_pending_text_locked();
    }
}
void SseStream::flush_pending_text_locked() {
    if (pending_text_size_ == 0) return;
    // Build a coalesced payload that reuses the first chunk's
    // identity (agent, stream_id) but concatenates every delta.
    // Replay-time clients see one bigger chunk in place of many
    // small ones; the assembled string is identical.
    auto coalesced = jobj();
    auto& m = coalesced->as_object_mut();
    if (auto a = pending_text_first_->get("agent")) m["agent"] = a;
    if (auto s = pending_text_first_->get("stream_id")) m["stream_id"] = s;
    if (auto d = pending_text_first_->get("depth")) m["depth"] = d;
    m["delta"] = jstr(pending_text_concat_);
    std::string payload = json_serialize(*coalesced);
    persist_event_locked("text", payload);
    pending_text_first_.reset();
    pending_text_key_.clear();
    pending_text_concat_.clear();
    pending_text_size_ = 0;
}

} // namespace arbiter
