#pragma once
// Consume arbiter-native SSE events from POST /v1/conversations/:id/messages
// (same catalog as /v1/orchestrate) and feed the existing StreamRenderer /
// OutputQueue path used by the local TUI.
//
// Note: do not `#include "api_client.h"` here — from this directory that
// name resolves to remote/api_client.h (the HTTP client), not the LLM
// ApiResponse type in include/api_client.h.

#include "commands.h"
#include "json.h"
#include "repl/queues.h"
#include "stream_renderer.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace arbiter {

struct RemoteTurnHooks {
    // Called when request_received yields a request_id (for Esc → cancel).
    std::function<void(const std::string& request_id)> on_request_id;
    // Tenant name from request_received when present.
    std::function<void(const std::string& tenant_name, int64_t tenant_id)> on_tenant;
    // Tool completions (SSE tool_call ≈ Finished).
    std::function<void(const ToolActivityEvent&)> on_tool;
    // Sub-agent full responses.
    std::function<void(const std::string& agent, const std::string& content)> on_sub_agent;
    // Escalation / advisor halt.
    std::function<void(const std::string& agent, const std::string& reason)> on_escalation;
    // Pre-dispatch intent classify/route (SSE `intent`).
    std::function<void(const std::string& kind,
                       const std::string& source,
                       const std::string& target_agent,
                       bool applied)> on_intent;
    // Advisor activity (SSE `advisor`); halt/budget are suppressed by the
    // session hook in favor of on_escalation.
    std::function<void(const std::string& kind,
                       const std::string& agent,
                       const std::string& detail)> on_advisor;
};

struct RemoteTurnResult {
    bool ok = false;
    std::string content;
    std::string error;
    std::string request_id;
    int input_tokens = 0;
    int output_tokens = 0;
    bool cancelled = false;
};

// Stateful SSE → renderer bridge for one turn.  Feed every (event, data)
// from the stream; call finish() after the HTTP call returns.
class RemoteSseTurnConsumer {
public:
    RemoteSseTurnConsumer(StreamRenderer& renderer,
                          OutputQueue& queue,
                          RemoteTurnHooks hooks = {});

    void on_event(const std::string& event_name, const std::string& data);
    RemoteTurnResult finish(bool transport_cancelled);

    [[nodiscard]] const std::string& request_id() const { return request_id_; }

private:
    void handle_text(const JsonValue& payload);
    void handle_tool_call(const JsonValue& payload);
    void handle_file(const JsonValue& payload);
    void handle_done(const JsonValue& payload);
    void handle_error(const JsonValue& payload);
    void handle_request_received(const JsonValue& payload);
    void handle_sub_agent(const JsonValue& payload);
    void handle_escalation(const JsonValue& payload);
    void handle_intent(const JsonValue& payload);
    void handle_advisor(const JsonValue& payload);

    StreamRenderer& renderer_;
    OutputQueue&    queue_;
    RemoteTurnHooks hooks_;
    std::mutex      mu_;
    std::string     request_id_;
    std::string     content_;
    std::string     error_;
    bool            done_seen_ = false;
    bool            ok_ = false;
    int             input_tokens_ = 0;
    int             output_tokens_ = 0;
    bool            flushed_ = false;
};

} // namespace arbiter
