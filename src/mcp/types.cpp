// src/mcp/types.cpp — JSON-RPC 2.0 framing for MCP

#include "mcp/types.h"

#include <sstream>
#include <stdexcept>

namespace index_ai::mcp {

namespace {

std::shared_ptr<JsonValue> envelope(const std::string& method) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["jsonrpc"] = jstr("2.0");
    m["method"]  = jstr(method);
    return o;
}

} // namespace

std::string serialize_request(const Request& r) {
    auto o = envelope(r.method);
    auto& m = o->as_object_mut();
    m["id"] = jnum(static_cast<double>(r.id));
    if (r.params) m["params"] = r.params;
    return json_serialize(*o);
}

std::string serialize_notification(const Notification& n) {
    auto o = envelope(n.method);
    auto& m = o->as_object_mut();
    if (n.params) m["params"] = n.params;
    return json_serialize(*o);
}

Response parse_response(const std::string& line) {
    std::shared_ptr<JsonValue> v;
    try { v = json_parse(line); }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("malformed JSON: ") + e.what());
    }
    if (!v || !v->is_object())
        throw std::runtime_error("response is not a JSON object");

    Response r;
    if (auto j = v->get("jsonrpc"); !j || !j->is_string() || j->as_string() != "2.0")
        throw std::runtime_error("missing or wrong 'jsonrpc' field");

    if (auto j = v->get("id"); j && j->is_number()) {
        r.id = static_cast<int64_t>(j->as_number());
    } else {
        // Notifications never have an id; if we see a response without an
        // id it's an inbound notification from the server (tool-side
        // signalling like notifications/cancelled).  We surface those by
        // leaving id = 0 and letting the caller drop them — the matcher
        // never expects id 0 since our request counter starts at 1.
    }

    auto res_v = v->get("result");
    auto err_v = v->get("error");
    if (res_v && err_v)
        throw std::runtime_error("response has both 'result' and 'error'");
    if (res_v) {
        r.result = res_v;
    } else if (err_v) {
        if (!err_v->is_object())
            throw std::runtime_error("'error' is not a JSON object");
        RpcError e;
        if (auto c = err_v->get("code"); c && c->is_number())
            e.code = static_cast<int>(c->as_number());
        if (auto m = err_v->get("message"); m && m->is_string())
            e.message = m->as_string();
        if (auto d = err_v->get("data")) e.data = d;
        r.error = std::move(e);
    }
    // Empty result + empty error is allowed (e.g. notifications/initialized
    // ack from some servers); leaves result null and error empty.
    return r;
}

std::vector<ToolDescriptor> parse_tools_list(const Response& r) {
    if (r.error) {
        throw std::runtime_error("tools/list returned error: " + r.error->message);
    }
    if (!r.result || !r.result->is_object())
        throw std::runtime_error("tools/list result is not an object");
    auto tools = r.result->get("tools");
    if (!tools || !tools->is_array())
        throw std::runtime_error("tools/list result missing 'tools' array");

    std::vector<ToolDescriptor> out;
    for (auto& item : tools->as_array()) {
        if (!item || !item->is_object()) continue;
        ToolDescriptor t;
        if (auto n = item->get("name"); n && n->is_string()) t.name = n->as_string();
        if (auto d = item->get("description"); d && d->is_string()) t.description = d->as_string();
        if (auto s = item->get("inputSchema")) t.input_schema = s;
        // Skip nameless entries — every spec-compliant MCP server names
        // its tools, so an unnamed item is corruption we don't surface.
        if (!t.name.empty()) out.push_back(std::move(t));
    }
    return out;
}

ToolResult parse_tool_result(const Response& r) {
    if (r.error) {
        // Transport-level error (tool name unknown, server crashed mid-call).
        // Fold it into is_error=true with the message as text content so the
        // agent's tool-result block stays uniform regardless of where the
        // error originated.
        ToolResult tr;
        tr.is_error = true;
        ContentItem c;
        c.type = "text";
        c.text = "MCP error " + std::to_string(r.error->code) + ": " + r.error->message;
        tr.content.push_back(std::move(c));
        return tr;
    }
    if (!r.result || !r.result->is_object())
        throw std::runtime_error("tools/call result is not an object");

    ToolResult tr;
    if (auto e = r.result->get("isError"); e && e->is_bool()) tr.is_error = e->as_bool();

    auto content = r.result->get("content");
    if (content && content->is_array()) {
        for (auto& item : content->as_array()) {
            if (!item || !item->is_object()) continue;
            ContentItem c;
            if (auto t = item->get("type"); t && t->is_string()) c.type = t->as_string();
            if (auto t = item->get("text"); t && t->is_string()) c.text = t->as_string();
            if (auto m = item->get("mimeType"); m && m->is_string()) c.mime_type = m->as_string();
            tr.content.push_back(std::move(c));
        }
    }
    return tr;
}

std::string render_tool_result(const ToolResult& r) {
    std::ostringstream out;
    if (r.is_error) out << "[tool reported isError=true]\n";
    bool wrote_anything = false;
    for (auto& c : r.content) {
        if (c.type == "text") {
            out << c.text;
            if (!c.text.empty() && c.text.back() != '\n') out << "\n";
            wrote_anything = true;
        } else {
            out << "[non-text content: " << c.type;
            if (!c.mime_type.empty()) out << " (" << c.mime_type << ")";
            out << " — agent surfaces only text]\n";
            wrote_anything = true;
        }
    }
    if (!wrote_anything && !r.is_error) {
        out << "(empty tool result)\n";
    }
    return out.str();
}

} // namespace index_ai::mcp
