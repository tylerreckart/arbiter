#include "remote/sse_turn.h"

#include "render_policy.h"

#include <utility>

namespace arbiter {

RemoteSseTurnConsumer::RemoteSseTurnConsumer(StreamRenderer& renderer,
                                             OutputQueue& queue,
                                             RemoteTurnHooks hooks)
    : renderer_(renderer)
    , queue_(queue)
    , hooks_(std::move(hooks)) {}

void RemoteSseTurnConsumer::on_event(const std::string& event_name,
                                      const std::string& data) {
    std::shared_ptr<JsonValue> payload;
    try {
        if (!data.empty()) payload = json_parse(data);
    } catch (...) {
        payload = nullptr;
    }

    if (event_name == "request_received") {
        if (payload) handle_request_received(*payload);
        return;
    }
    if (event_name == "text") {
        if (payload) handle_text(*payload);
        return;
    }
    if (event_name == "tool_call") {
        if (payload) handle_tool_call(*payload);
        return;
    }
    if (event_name == "file") {
        if (payload) handle_file(*payload);
        return;
    }
    if (event_name == "sub_agent_response") {
        if (payload) handle_sub_agent(*payload);
        return;
    }
    if (event_name == "escalation") {
        if (payload) handle_escalation(*payload);
        return;
    }
    if (event_name == "error") {
        if (payload) handle_error(*payload);
        return;
    }
    if (event_name == "done") {
        if (payload) handle_done(*payload);
        return;
    }
    // stream_start / agent_start / stream_end / token_usage / advisor —
    // informational; local TUI mirrors these via orch callbacks.  Skip.
}

void RemoteSseTurnConsumer::handle_request_received(const JsonValue& payload) {
    std::string rid;
    std::string tenant;
    int64_t tid = 0;
    decltype(hooks_.on_request_id) id_cb;
    decltype(hooks_.on_tenant) tenant_cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        request_id_ = payload.get_string("request_id");
        rid = request_id_;
        tenant = payload.get_string("tenant");
        tid = static_cast<int64_t>(payload.get_number("tenant_id", 0));
        id_cb = hooks_.on_request_id;
        tenant_cb = hooks_.on_tenant;
    }
    if (id_cb && !rid.empty()) id_cb(rid);
    if (tenant_cb && (!tenant.empty() || tid != 0)) tenant_cb(tenant, tid);
}

void RemoteSseTurnConsumer::handle_text(const JsonValue& payload) {
    const std::string delta = payload.get_string("delta");
    if (delta.empty()) return;
    // Master-only text includes depth==0 or omitted; sub-agent text may
    // also arrive as "text" with agent set — feed all deltas into the
    // same renderer path (matches local StreamRenderer for the focused
    // pane). Sub-agent headers are handled via sub_agent_response.
    const int depth = static_cast<int>(payload.get_number("depth", 0));
    if (depth > 0) {
        // Sub-agent streaming text — still render (API emits for depth>0).
    }
    renderer_.feed(delta);
}

void RemoteSseTurnConsumer::handle_tool_call(const JsonValue& payload) {
    ToolActivityEvent ev;
    ev.phase = ToolActivityEvent::Phase::Finished;
    ev.label = payload.get_string("tool");
    if (ev.label.empty()) ev.label = payload.get_string("label");
    ev.ok = payload.get_bool("ok", true);
    ev.agent_id = payload.get_string("agent");
    ev.kind = ev.label;
    ev.id = "remote-" + ev.label;
    if (hooks_.on_tool) hooks_.on_tool(ev);
    else queue_.push_tool(ev);
}

void RemoteSseTurnConsumer::handle_file(const JsonValue& payload) {
    const std::string path = payload.get_string("path");
    const std::string content = payload.get_string("content");
    const int size = static_cast<int>(payload.get_number("size",
        static_cast<double>(content.size())));
    std::string line = "[file] " + (path.empty() ? "(unnamed)" : path) +
                       " (" + std::to_string(size) + " bytes)";
    queue_.push_prose({styled_activity_line(std::move(line), StyleId::System)});
    if (!content.empty()) {
        // Show a short preview; full content can be huge.
        std::string preview = content;
        if (preview.size() > 2000) {
            preview.resize(1997);
            preview += "...";
        }
        renderer_.feed(preview);
        if (preview.back() != '\n') renderer_.feed("\n");
    }
}

void RemoteSseTurnConsumer::handle_sub_agent(const JsonValue& payload) {
    const std::string agent = payload.get_string("agent");
    const std::string content = payload.get_string("content");
    if (hooks_.on_sub_agent) hooks_.on_sub_agent(agent, content);
    else if (!content.empty()) {
        queue_.push_prose({styled_interim_header(agent.empty() ? "agent" : agent)});
        StreamRenderer interim(kInterim, queue_);
        interim.feed(content);
        interim.flush();
    }
}

void RemoteSseTurnConsumer::handle_escalation(const JsonValue& payload) {
    const std::string agent = payload.get_string("agent");
    const std::string reason = payload.get_string("reason");
    if (hooks_.on_escalation) hooks_.on_escalation(agent, reason);
    else {
        std::string text = "[advisor halt: " +
            (agent.empty() ? "?" : agent) + "] " + reason;
        queue_.push_prose(
            {styled_activity_line(std::move(text), StyleId::Error)});
        queue_.end_message();
    }
}

void RemoteSseTurnConsumer::handle_error(const JsonValue& payload) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string msg = payload.get_string("message");
    if (!msg.empty()) {
        if (!error_.empty()) error_ += "; ";
        error_ += msg;
    }
}

void RemoteSseTurnConsumer::handle_done(const JsonValue& payload) {
    std::lock_guard<std::mutex> lk(mu_);
    done_seen_ = true;
    ok_ = payload.get_bool("ok", false);
    content_ = payload.get_string("content");
    if (!ok_) {
        const std::string err = payload.get_string("error");
        if (!err.empty()) error_ = err;
    }
    input_tokens_ = static_cast<int>(payload.get_number("input_tokens", 0));
    output_tokens_ = static_cast<int>(payload.get_number("output_tokens", 0));
    if (request_id_.empty())
        request_id_ = payload.get_string("request_id");
}

RemoteTurnResult RemoteSseTurnConsumer::finish(bool transport_cancelled) {
    if (!flushed_) {
        renderer_.flush();
        flushed_ = true;
    }
    RemoteTurnResult r;
    r.request_id = request_id_;
    r.input_tokens = input_tokens_;
    r.output_tokens = output_tokens_;
    r.cancelled = transport_cancelled;
    if (transport_cancelled && !done_seen_) {
        r.ok = false;
        r.error = "interrupted";
        r.content = content_;
        return r;
    }
    if (done_seen_) {
        r.ok = ok_;
        r.content = content_;
        r.error = error_;
        return r;
    }
    // Stream closed without done — treat as failure.
    r.ok = false;
    r.error = error_.empty() ? "stream ended without done event" : error_;
    r.content = content_;
    return r;
}

} // namespace arbiter
