// src/mcp/client.cpp — Single-server MCP session

#include "mcp/client.h"

#include <stdexcept>

namespace arbiter::mcp {

namespace {

constexpr const char* kProtocolVersion = "2025-06-18";

std::shared_ptr<JsonValue> make_initialize_params() {
    auto params = jobj();
    auto& m = params->as_object_mut();
    m["protocolVersion"] = jstr(kProtocolVersion);
    m["capabilities"] = jobj();
    auto info = jobj();
    auto& im = info->as_object_mut();
    im["name"]    = jstr("arbiter");
    im["version"] = jstr("0.4.3");
    m["clientInfo"] = info;
    return params;
}

} // namespace

Client::Client(ClientConfig cfg) : cfg_(std::move(cfg)) {
    proc_ = std::make_unique<Subprocess>(cfg_.argv, cfg_.env_extra);

    // Initialize handshake.  Errors at this stage abort the whole
    // session — caller catches and surfaces to the agent.
    Response init_resp;
    try {
        init_resp = rpc("initialize", make_initialize_params(), cfg_.init_timeout);
    } catch (...) {
        proc_.reset();   // kill subprocess before propagating
        throw;
    }
    if (init_resp.error) {
        proc_.reset();
        throw std::runtime_error("MCP initialize error " +
            std::to_string(init_resp.error->code) + ": " + init_resp.error->message);
    }

    // Spec: client must send notifications/initialized after a successful
    // initialize, before issuing other requests.  Servers that don't
    // observe this just hang on tools/list.
    Notification ready;
    ready.method = "notifications/initialized";
    ready.params = jobj();
    if (!proc_->send_line(serialize_notification(ready))) {
        proc_.reset();
        throw std::runtime_error("MCP server closed pipe during handshake");
    }
}

Client::~Client() = default;

Response Client::rpc(const std::string& method,
                      std::shared_ptr<JsonValue> params,
                      std::chrono::milliseconds timeout) {
    if (!proc_) throw std::runtime_error("MCP client not initialised");
    Request req;
    req.id     = next_id_++;
    req.method = method;
    req.params = params;

    if (!proc_->send_line(serialize_request(req)))
        throw std::runtime_error("MCP server pipe closed during '" + method + "' send");

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            throw std::runtime_error("MCP '" + method + "' timed out");

        auto line = proc_->recv_line(remaining);
        if (!line)
            throw std::runtime_error("MCP server stopped responding during '" + method + "'");
        if (line->empty()) continue;   // tolerate blank-line keepalives

        Response resp;
        try { resp = parse_response(*line); }
        catch (const std::exception& e) {
            (void)e;
            continue;
        }
        // Notifications carry no id; let them pass.
        if (resp.id == req.id) return resp;
    }
}

const std::vector<ToolDescriptor>& Client::tools() {
    if (tools_loaded_) return tools_cache_;
    auto resp = rpc("tools/list", jobj(), cfg_.call_timeout);
    tools_cache_ = parse_tools_list(resp);
    tools_loaded_ = true;
    return tools_cache_;
}

ToolResult Client::call_tool(const std::string& name,
                              std::shared_ptr<JsonValue> arguments) {
    auto params = jobj();
    auto& m = params->as_object_mut();
    m["name"]      = jstr(name);
    m["arguments"] = (arguments && arguments->is_object()) ? arguments : jobj();
    auto resp = rpc("tools/call", params, cfg_.call_timeout);
    return parse_tool_result(resp);
}

} // namespace arbiter::mcp
