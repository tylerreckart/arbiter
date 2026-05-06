// src/a2a/client.cpp — Per-remote A2A Client implementation.

#include "a2a/client.h"

#include "json.h"

#include <utility>

namespace index_ai::a2a {

namespace {

// Resolve the agent-card URL for a given endpoint.  arbiter's server
// publishes /v1/a2a/agents/<id>/agent-card.json — we append "agent-card.json"
// to the configured endpoint URL.  Other implementations might host
// the card elsewhere; operators with a non-arbiter remote should set
// the endpoint to the *card path's directory*, not the JSON-RPC URL.
std::string card_url_for(const std::string& endpoint_url) {
    std::string s = endpoint_url;
    if (s.empty()) return s;
    if (s.back() == '/') s.pop_back();
    return s + "/agent-card.json";
}

} // namespace

Client::Client(std::string name,
               std::string endpoint_url,
               std::string bearer_token,
               std::chrono::seconds rpc_timeout,
               std::chrono::seconds stream_timeout)
    : name_(std::move(name)),
      endpoint_url_(std::move(endpoint_url)),
      bearer_token_(std::move(bearer_token)),
      rpc_timeout_(rpc_timeout),
      stream_timeout_(stream_timeout) {}

std::vector<HttpHeader> Client::auth_headers() const {
    std::vector<HttpHeader> h;
    if (!bearer_token_.empty()) {
        h.push_back({"Authorization", "Bearer " + bearer_token_});
    }
    h.push_back({"A2A-Version", "1.0"});
    return h;
}

const AgentCard& Client::card() {
    std::lock_guard<std::mutex> lk(card_mu_);
    if (cached_card_) return *cached_card_;

    const std::string url = card_url_for(endpoint_url_);
    HttpResponse r = http_get(url, auth_headers(), /*timeout=*/15);
    if (!r.error.empty()) {
        throw std::runtime_error("a2a card fetch failed: " + r.error +
                                  " (url=" + url + ")");
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        throw std::runtime_error("a2a card fetch HTTP " +
                                  std::to_string(r.status_code) +
                                  " (url=" + url + ")");
    }
    auto v = json_parse(r.body);
    if (!v || !v->is_object()) {
        throw std::runtime_error("a2a card body is not a JSON object");
    }
    cached_card_ = agent_card_from_json(*v);
    return *cached_card_;
}

const AgentCard& Client::refresh_card() {
    {
        std::lock_guard<std::mutex> lk(card_mu_);
        cached_card_.reset();
    }
    return card();
}

std::shared_ptr<JsonValue> Client::rpc(const std::string& method,
                                        std::shared_ptr<JsonValue> params,
                                        std::string& err_out) {
    RpcRequest req;
    req.id     = jnum(static_cast<double>(rpc_id_counter_.fetch_add(1)));
    req.method = method;
    req.params = std::move(params);
    const std::string body = json_serialize(*to_json(req));

    HttpResponse r = rpc_call(endpoint_url_, auth_headers(), body,
                               rpc_timeout_.count());
    if (!r.error.empty()) {
        err_out = "transport: " + r.error;
        return nullptr;
    }
    if (r.status_code != 200) {
        err_out = "HTTP " + std::to_string(r.status_code) + ": " + r.body;
        return nullptr;
    }
    std::shared_ptr<JsonValue> v;
    try { v = json_parse(r.body); }
    catch (const std::exception& e) {
        err_out = std::string("malformed JSON-RPC response: ") + e.what();
        return nullptr;
    }
    if (!v || !v->is_object()) {
        err_out = "JSON-RPC response is not an object";
        return nullptr;
    }
    RpcResponse resp;
    try { resp = rpc_response_from_json(*v); }
    catch (const std::exception& e) {
        err_out = std::string("JSON-RPC parse: ") + e.what();
        return nullptr;
    }
    if (resp.error) {
        err_out = "JSON-RPC error " + std::to_string(resp.error->code) +
                  ": " + resp.error->message;
        return nullptr;
    }
    if (!resp.result) {
        err_out = "JSON-RPC response had no result and no error";
        return nullptr;
    }
    return resp.result;
}

std::optional<Task> Client::send_message(const Message& msg,
                                          std::string& err_out) {
    auto params = jobj();
    params->as_object_mut()["message"] = to_json(msg);
    auto result = rpc("message/send", params, err_out);
    if (!result) return std::nullopt;
    if (!result->is_object()) {
        err_out = "result is not an object";
        return std::nullopt;
    }
    // Spec allows either a Task or a Message back from message/send;
    // arbiter's server always returns Task.  Discriminate on the kind
    // field so a future server change to return a bare Message doesn't
    // break the client.
    const std::string kind = result->get_string("kind", "");
    if (kind == "message") {
        // Synthesise a one-message Task so the caller has a uniform
        // shape.  contextId is taken from the Message; state defaults
        // to completed since a bare Message implies no in-flight work.
        Message returned;
        try { returned = message_from_json(*result); }
        catch (const std::exception& e) {
            err_out = std::string("returned message parse: ") + e.what();
            return std::nullopt;
        }
        Task t;
        t.id         = returned.task_id.value_or("");
        t.context_id = returned.context_id.value_or("");
        t.status.state   = TaskState::completed;
        t.status.message = returned;
        t.history.push_back(*t.status.message);
        return t;
    }
    try { return task_from_json(*result); }
    catch (const std::exception& e) {
        err_out = std::string("returned task parse: ") + e.what();
        return std::nullopt;
    }
}

HttpResponse Client::stream_message(const Message& msg,
                                     std::function<void(const StreamEvent&)> on_event,
                                     std::atomic<bool>& cancel) {
    RpcRequest req;
    req.id     = jnum(static_cast<double>(rpc_id_counter_.fetch_add(1)));
    req.method = "message/stream";
    auto params = jobj();
    params->as_object_mut()["message"] = to_json(msg);
    req.params = params;

    const std::string body = json_serialize(*to_json(req));

    auto sse_cb = [&on_event](const std::string& /*event_name*/,
                                const std::string& data) {
        // Each SSE data line is a JSON-RPC response envelope.  Parse
        // and unwrap; ignore frames that fail to parse rather than
        // stopping the stream — A2A servers can interleave keepalive
        // comments that won't reach this callback (SseReader skips
        // them) but malformed data lines should be loud-but-survivable.
        std::shared_ptr<JsonValue> v;
        try { v = json_parse(data); }
        catch (...) { return; }
        if (!v || !v->is_object()) return;
        auto result = v->get("result");
        if (!result || !result->is_object()) return;

        StreamEvent ev;
        ev.kind   = result->get_string("kind", "");
        ev.result = result;
        on_event(ev);
    };

    return rpc_stream(endpoint_url_, auth_headers(), body,
                       sse_cb, cancel, stream_timeout_.count());
}

std::optional<Task> Client::get_task(const std::string& task_id,
                                       std::string& err_out) {
    auto params = jobj();
    params->as_object_mut()["id"] = jstr(task_id);
    auto result = rpc("tasks/get", params, err_out);
    if (!result) return std::nullopt;
    try { return task_from_json(*result); }
    catch (const std::exception& e) {
        err_out = std::string("task parse: ") + e.what();
        return std::nullopt;
    }
}

std::optional<Task> Client::cancel_task(const std::string& task_id,
                                          std::string& err_out) {
    auto params = jobj();
    params->as_object_mut()["id"] = jstr(task_id);
    auto result = rpc("tasks/cancel", params, err_out);
    if (!result) return std::nullopt;
    try { return task_from_json(*result); }
    catch (const std::exception& e) {
        err_out = std::string("task parse: ") + e.what();
        return std::nullopt;
    }
}

} // namespace index_ai::a2a
