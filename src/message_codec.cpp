#include "message_codec.h"

#include "json.h"

namespace arbiter {

namespace {

std::shared_ptr<JsonValue> tool_trace_to_json(
    const std::vector<ToolTraceEntry>& trace) {
    auto arr = jarr();
    for (const auto& t : trace) {
        auto obj = jobj();
        auto& m = obj->as_object_mut();
        m["id"] = jstr(t.id);
        m["label"] = jstr(t.label);
        m["kind"] = jstr(t.kind);
        m["detail"] = jstr(t.detail);
        m["ok"] = jbool(t.ok);
        m["result_preview"] = jstr(t.result_preview);
        arr->as_array_mut().push_back(obj);
    }
    return arr;
}

std::vector<ToolTraceEntry> tool_trace_from_json(const JsonValue* arr) {
    std::vector<ToolTraceEntry> out;
    if (!arr || !arr->is_array()) return out;
    for (auto& v : arr->as_array()) {
        if (!v || !v->is_object()) continue;
        ToolTraceEntry t;
        t.id = v->get_string("id");
        t.label = v->get_string("label");
        t.kind = v->get_string("kind");
        t.detail = v->get_string("detail");
        t.ok = v->get_bool("ok", true);
        t.result_preview = v->get_string("result_preview");
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace

std::string encode_messages_json(const std::vector<Message>& msgs) {
    auto arr = jarr();
    for (const auto& m : msgs) {
        auto obj = jobj();
        obj->as_object_mut()["role"]    = jstr(m.role);
        obj->as_object_mut()["content"] = jstr(m.content);
        if (!m.thinking.empty()) {
            obj->as_object_mut()["thinking"] = jstr(m.thinking);
        }
        if (!m.tool_trace.empty()) {
            obj->as_object_mut()["tool_trace"] = tool_trace_to_json(m.tool_trace);
        }
        arr->as_array_mut().push_back(obj);
    }
    return json_serialize(*arr);
}

std::vector<Message> decode_messages_json(const std::string& json) {
    std::vector<Message> out;
    if (json.empty()) return out;
    try {
        auto root = json_parse(json);
        if (!root || !root->is_array()) return out;
        for (auto& v : root->as_array()) {
            if (!v) continue;
            Message msg;
            msg.role = v->get_string("role");
            msg.content = v->get_string("content");
            msg.thinking = v->get_string("thinking");
            msg.tool_trace = tool_trace_from_json(v->get("tool_trace").get());
            out.push_back(std::move(msg));
        }
    } catch (...) {
        return {};
    }
    return out;
}

} // namespace arbiter
