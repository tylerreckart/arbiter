// src/a2a/types.cpp — Serialize + parse for the A2A v1.0 wire surface.
//
// Every parse function throws std::runtime_error with a stable
// "a2a parse: <where>: <why>" prefix on missing-or-malformed required
// fields.  Optional fields are silently dropped if the type is wrong —
// we don't want to fail a streaming SSE event because some upstream
// stuck a number in an extension field.
//
// The serializers use the json.h helper constructors (jobj, jstr, ...)
// to keep the wire shape declarative.  Field ordering in the emitted
// JSON is unspecified by the parser (std::unordered_map underneath),
// so tests that compare wire output do so semantically, not byte-for-byte.

#include "a2a/types.h"

#include <stdexcept>
#include <string>

namespace arbiter::a2a {

// ---------------------------------------------------------------------------
// Local parse helpers.  Wrapped in an anonymous namespace so they don't
// pollute the public surface — agent code outside this file shouldn't be
// rolling its own field extraction; if it needs more, add a helper here.
// ---------------------------------------------------------------------------
namespace {

[[noreturn]] void throw_parse(const std::string& where, const std::string& why) {
    throw std::runtime_error("a2a parse: " + where + ": " + why);
}

const JsonObject& require_object(const JsonValue& v, const std::string& where) {
    if (!v.is_object()) throw_parse(where, "expected JSON object");
    return v.as_object();
}

std::string require_string_field(const JsonValue& v, const std::string& field) {
    auto p = v.get(field);
    if (!p)              throw_parse(field, "required field missing");
    if (!p->is_string()) throw_parse(field, "expected string");
    return p->as_string();
}

std::optional<std::string> opt_string(const JsonValue& v, const std::string& field) {
    auto p = v.get(field);
    if (!p || !p->is_string()) return std::nullopt;
    return p->as_string();
}

std::optional<bool> opt_bool(const JsonValue& v, const std::string& field) {
    auto p = v.get(field);
    if (!p || !p->is_bool()) return std::nullopt;
    return p->as_bool();
}

std::vector<std::string> opt_string_array(const JsonValue& v, const std::string& field) {
    std::vector<std::string> out;
    auto p = v.get(field);
    if (!p || !p->is_array()) return out;
    for (auto& item : p->as_array()) {
        if (item && item->is_string()) out.push_back(item->as_string());
    }
    return out;
}

std::shared_ptr<JsonValue> opt_passthrough(const JsonValue& v, const std::string& field) {
    auto p = v.get(field);
    return p ? p : nullptr;
}

void put_if(JsonObject& m, const std::string& key, const std::optional<std::string>& v) {
    if (v) m[key] = jstr(*v);
}

void put_if(JsonObject& m, const std::string& key, const std::optional<bool>& v) {
    if (v) m[key] = jbool(*v);
}

void put_if(JsonObject& m, const std::string& key, const std::shared_ptr<JsonValue>& v) {
    if (v) m[key] = v;
}

std::shared_ptr<JsonValue> string_array(const std::vector<std::string>& xs) {
    auto a = jarr();
    auto& arr = a->as_array_mut();
    arr.reserve(xs.size());
    for (auto& s : xs) arr.push_back(jstr(s));
    return a;
}

} // namespace

// ---------------------------------------------------------------------------
// TaskState <-> string.  Wire form is the lowercase hyphenated name
// from the spec; our enum identifiers use underscores because hyphens
// aren't valid in C++ identifiers.
// ---------------------------------------------------------------------------
std::string task_state_to_string(TaskState s) {
    switch (s) {
        case TaskState::submitted:       return "submitted";
        case TaskState::working:         return "working";
        case TaskState::input_required:  return "input-required";
        case TaskState::auth_required:   return "auth-required";
        case TaskState::completed:       return "completed";
        case TaskState::canceled:        return "canceled";
        case TaskState::failed:          return "failed";
        case TaskState::rejected:        return "rejected";
        case TaskState::unknown:         return "unknown";
    }
    return "unknown";
}

TaskState task_state_from_string(const std::string& s) {
    if (s == "submitted")      return TaskState::submitted;
    if (s == "working")        return TaskState::working;
    if (s == "input-required") return TaskState::input_required;
    if (s == "auth-required")  return TaskState::auth_required;
    if (s == "completed")      return TaskState::completed;
    if (s == "canceled")       return TaskState::canceled;
    if (s == "failed")         return TaskState::failed;
    if (s == "rejected")       return TaskState::rejected;
    return TaskState::unknown;
}

bool task_state_is_terminal(TaskState s) {
    return s == TaskState::completed
        || s == TaskState::canceled
        || s == TaskState::failed
        || s == TaskState::rejected;
}

// ---------------------------------------------------------------------------
// Part
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const Part& p) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["kind"] = jstr(p.kind);

    if (p.kind == "text") {
        m["text"] = jstr(p.text);
    } else if (p.kind == "file") {
        // FileWithBytes vs FileWithUri — exactly one is set.  When
        // both are populated (caller bug) we prefer URI: it's idempotent
        // and doesn't bloat the SSE frame with base64.
        auto file = jobj();
        auto& fm = file->as_object_mut();
        if (p.file_uri) {
            fm["uri"] = jstr(*p.file_uri);
        } else if (p.file_bytes) {
            fm["bytes"] = jstr(*p.file_bytes);
        }
        put_if(fm, "name",      p.file_name);
        put_if(fm, "mimeType",  p.file_mime_type);
        m["file"] = file;
    } else if (p.kind == "data") {
        m["data"] = p.data ? p.data : jobj();
    }

    put_if(m, "metadata", p.metadata);
    return o;
}

Part part_from_json(const JsonValue& v) {
    require_object(v, "part");
    Part p;
    p.kind = require_string_field(v, "kind");

    if (p.kind == "text") {
        p.text = require_string_field(v, "text");
    } else if (p.kind == "file") {
        auto file = v.get("file");
        if (!file || !file->is_object()) throw_parse("part.file", "expected object");
        p.file_uri       = opt_string(*file, "uri");
        p.file_bytes     = opt_string(*file, "bytes");
        p.file_name      = opt_string(*file, "name");
        p.file_mime_type = opt_string(*file, "mimeType");
        if (!p.file_uri && !p.file_bytes) {
            throw_parse("part.file", "must have one of uri | bytes");
        }
    } else if (p.kind == "data") {
        p.data = v.get("data");
        if (!p.data) throw_parse("part.data", "required field missing");
    } else {
        throw_parse("part.kind", "unknown kind: " + p.kind);
    }

    p.metadata = opt_passthrough(v, "metadata");
    return p;
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const Message& msg) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["kind"]      = jstr("message");
    m["role"]      = jstr(msg.role);
    m["messageId"] = jstr(msg.message_id);

    auto parts = jarr();
    auto& pa = parts->as_array_mut();
    pa.reserve(msg.parts.size());
    for (auto& p : msg.parts) pa.push_back(to_json(p));
    m["parts"] = parts;

    put_if(m, "taskId",            msg.task_id);
    put_if(m, "contextId",         msg.context_id);
    put_if(m, "metadata",          msg.metadata);
    put_if(m, "extensions",        msg.extensions);
    put_if(m, "referenceTaskIds",  msg.reference_task_ids);
    return o;
}

Message message_from_json(const JsonValue& v) {
    require_object(v, "message");
    Message m;
    m.role               = require_string_field(v, "role");
    m.message_id         = require_string_field(v, "messageId");
    m.task_id            = opt_string(v, "taskId");
    m.context_id         = opt_string(v, "contextId");
    m.metadata           = opt_passthrough(v, "metadata");
    m.extensions         = opt_passthrough(v, "extensions");
    m.reference_task_ids = opt_passthrough(v, "referenceTaskIds");

    auto parts = v.get("parts");
    if (!parts || !parts->is_array()) {
        throw_parse("message.parts", "required array missing");
    }
    for (auto& item : parts->as_array()) {
        if (item) m.parts.push_back(part_from_json(*item));
    }
    return m;
}

// ---------------------------------------------------------------------------
// Artifact
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const Artifact& a) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["artifactId"] = jstr(a.artifact_id);

    auto parts = jarr();
    auto& pa = parts->as_array_mut();
    pa.reserve(a.parts.size());
    for (auto& p : a.parts) pa.push_back(to_json(p));
    m["parts"] = parts;

    put_if(m, "name",        a.name);
    put_if(m, "description", a.description);
    put_if(m, "metadata",    a.metadata);
    put_if(m, "extensions",  a.extensions);
    return o;
}

Artifact artifact_from_json(const JsonValue& v) {
    require_object(v, "artifact");
    Artifact a;
    a.artifact_id = require_string_field(v, "artifactId");
    a.name        = opt_string(v, "name");
    a.description = opt_string(v, "description");
    a.metadata    = opt_passthrough(v, "metadata");
    a.extensions  = opt_passthrough(v, "extensions");

    auto parts = v.get("parts");
    if (!parts || !parts->is_array()) {
        throw_parse("artifact.parts", "required array missing");
    }
    for (auto& item : parts->as_array()) {
        if (item) a.parts.push_back(part_from_json(*item));
    }
    return a;
}

// ---------------------------------------------------------------------------
// TaskStatus
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const TaskStatus& s) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["state"] = jstr(task_state_to_string(s.state));
    if (s.message)  m["message"]   = to_json(*s.message);
    put_if(m, "timestamp", s.timestamp);
    return o;
}

TaskStatus task_status_from_json(const JsonValue& v) {
    require_object(v, "status");
    TaskStatus s;
    s.state = task_state_from_string(require_string_field(v, "state"));
    if (auto p = v.get("message"); p && p->is_object()) {
        s.message = message_from_json(*p);
    }
    s.timestamp = opt_string(v, "timestamp");
    return s;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const Task& t) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["kind"]      = jstr("task");
    m["id"]        = jstr(t.id);
    m["contextId"] = jstr(t.context_id);
    m["status"]    = to_json(t.status);

    if (!t.artifacts.empty()) {
        auto a = jarr();
        auto& aa = a->as_array_mut();
        aa.reserve(t.artifacts.size());
        for (auto& art : t.artifacts) aa.push_back(to_json(art));
        m["artifacts"] = a;
    }
    if (!t.history.empty()) {
        auto h = jarr();
        auto& ha = h->as_array_mut();
        ha.reserve(t.history.size());
        for (auto& msg : t.history) ha.push_back(to_json(msg));
        m["history"] = h;
    }
    put_if(m, "metadata", t.metadata);
    return o;
}

Task task_from_json(const JsonValue& v) {
    require_object(v, "task");
    Task t;
    t.id         = require_string_field(v, "id");
    t.context_id = require_string_field(v, "contextId");

    auto status = v.get("status");
    if (!status) throw_parse("task.status", "required field missing");
    t.status = task_status_from_json(*status);

    if (auto a = v.get("artifacts"); a && a->is_array()) {
        for (auto& item : a->as_array()) {
            if (item) t.artifacts.push_back(artifact_from_json(*item));
        }
    }
    if (auto h = v.get("history"); h && h->is_array()) {
        for (auto& item : h->as_array()) {
            if (item) t.history.push_back(message_from_json(*item));
        }
    }
    t.metadata = opt_passthrough(v, "metadata");
    return t;
}

// ---------------------------------------------------------------------------
// Streaming events
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const TaskStatusUpdateEvent& e) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["kind"]      = jstr("status-update");
    m["taskId"]    = jstr(e.task_id);
    m["contextId"] = jstr(e.context_id);
    m["status"]    = to_json(e.status);
    m["final"]     = jbool(e.final);
    put_if(m, "metadata", e.metadata);
    return o;
}

TaskStatusUpdateEvent task_status_update_from_json(const JsonValue& v) {
    require_object(v, "status-update");
    TaskStatusUpdateEvent e;
    e.task_id    = require_string_field(v, "taskId");
    e.context_id = require_string_field(v, "contextId");

    auto status = v.get("status");
    if (!status) throw_parse("status-update.status", "required field missing");
    e.status = task_status_from_json(*status);

    if (auto f = opt_bool(v, "final")) e.final = *f;
    e.metadata = opt_passthrough(v, "metadata");
    return e;
}

std::shared_ptr<JsonValue> to_json(const TaskArtifactUpdateEvent& e) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["kind"]      = jstr("artifact-update");
    m["taskId"]    = jstr(e.task_id);
    m["contextId"] = jstr(e.context_id);
    m["artifact"]  = to_json(e.artifact);
    put_if(m, "append",    e.append);
    put_if(m, "lastChunk", e.last_chunk);
    put_if(m, "metadata",  e.metadata);
    return o;
}

TaskArtifactUpdateEvent task_artifact_update_from_json(const JsonValue& v) {
    require_object(v, "artifact-update");
    TaskArtifactUpdateEvent e;
    e.task_id    = require_string_field(v, "taskId");
    e.context_id = require_string_field(v, "contextId");

    auto art = v.get("artifact");
    if (!art) throw_parse("artifact-update.artifact", "required field missing");
    e.artifact = artifact_from_json(*art);

    e.append     = opt_bool(v, "append");
    e.last_chunk = opt_bool(v, "lastChunk");
    e.metadata   = opt_passthrough(v, "metadata");
    return e;
}

// ---------------------------------------------------------------------------
// AgentCard + nested
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const Skill& s) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]          = jstr(s.id);
    m["name"]        = jstr(s.name);
    m["description"] = jstr(s.description);
    m["tags"]        = string_array(s.tags);
    if (!s.examples.empty())     m["examples"]    = string_array(s.examples);
    if (!s.input_modes.empty())  m["inputModes"]  = string_array(s.input_modes);
    if (!s.output_modes.empty()) m["outputModes"] = string_array(s.output_modes);
    return o;
}

Skill skill_from_json(const JsonValue& v) {
    require_object(v, "skill");
    Skill s;
    s.id           = require_string_field(v, "id");
    s.name         = require_string_field(v, "name");
    s.description  = require_string_field(v, "description");
    s.tags         = opt_string_array(v, "tags");
    s.examples     = opt_string_array(v, "examples");
    s.input_modes  = opt_string_array(v, "inputModes");
    s.output_modes = opt_string_array(v, "outputModes");
    return s;
}

std::shared_ptr<JsonValue> to_json(const AgentCardCapabilities& c) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["streaming"]               = jbool(c.streaming);
    m["pushNotifications"]       = jbool(c.push_notifications);
    m["stateTransitionHistory"]  = jbool(c.state_transition_history);
    put_if(m, "extensions", c.extensions);
    return o;
}

AgentCardCapabilities capabilities_from_json(const JsonValue& v) {
    AgentCardCapabilities c;
    if (!v.is_object()) return c;        // capabilities is optional in some flows
    if (auto b = opt_bool(v, "streaming"))              c.streaming = *b;
    if (auto b = opt_bool(v, "pushNotifications"))      c.push_notifications = *b;
    if (auto b = opt_bool(v, "stateTransitionHistory")) c.state_transition_history = *b;
    c.extensions = opt_passthrough(v, "extensions");
    return c;
}

std::shared_ptr<JsonValue> to_json(const AgentCard& c) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["protocolVersion"]    = jstr(c.protocol_version);
    m["name"]               = jstr(c.name);
    m["description"]        = jstr(c.description);
    m["url"]                = jstr(c.url);
    m["version"]            = jstr(c.version);
    m["defaultInputModes"]  = string_array(c.default_input_modes);
    m["defaultOutputModes"] = string_array(c.default_output_modes);
    m["capabilities"]       = to_json(c.capabilities);

    auto skills = jarr();
    auto& sa = skills->as_array_mut();
    sa.reserve(c.skills.size());
    for (auto& s : c.skills) sa.push_back(to_json(s));
    m["skills"] = skills;

    put_if(m, "preferredTransport",  c.preferred_transport);
    put_if(m, "documentationUrl",    c.documentation_url);
    put_if(m, "iconUrl",             c.icon_url);
    put_if(m, "provider",            c.provider);
    put_if(m, "additionalInterfaces",c.additional_interfaces);
    put_if(m, "securitySchemes",     c.security_schemes);
    put_if(m, "security",            c.security);
    put_if(m, "signatures",          c.signatures);
    put_if(m, "supportsAuthenticatedExtendedCard", c.supports_authenticated_extended_card);
    return o;
}

AgentCard agent_card_from_json(const JsonValue& v) {
    require_object(v, "agentCard");
    AgentCard c;
    c.protocol_version     = v.get_string("protocolVersion", "1.0");
    c.name                 = require_string_field(v, "name");
    c.description          = require_string_field(v, "description");
    c.url                  = require_string_field(v, "url");
    c.version              = require_string_field(v, "version");
    c.default_input_modes  = opt_string_array(v, "defaultInputModes");
    c.default_output_modes = opt_string_array(v, "defaultOutputModes");

    if (auto cap = v.get("capabilities")) {
        c.capabilities = capabilities_from_json(*cap);
    }

    auto skills = v.get("skills");
    if (skills && skills->is_array()) {
        for (auto& item : skills->as_array()) {
            if (item) c.skills.push_back(skill_from_json(*item));
        }
    }

    c.preferred_transport      = opt_string(v, "preferredTransport");
    c.documentation_url        = opt_string(v, "documentationUrl");
    c.icon_url                 = opt_string(v, "iconUrl");
    c.provider                 = opt_passthrough(v, "provider");
    c.additional_interfaces    = opt_passthrough(v, "additionalInterfaces");
    c.security_schemes         = opt_passthrough(v, "securitySchemes");
    c.security                 = opt_passthrough(v, "security");
    c.signatures               = opt_passthrough(v, "signatures");
    c.supports_authenticated_extended_card =
        opt_bool(v, "supportsAuthenticatedExtendedCard");
    return c;
}

// ---------------------------------------------------------------------------
// JSON-RPC envelope
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const RpcError& e) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["code"]    = jnum(static_cast<double>(e.code));
    m["message"] = jstr(e.message);
    put_if(m, "data", e.data);
    return o;
}

std::shared_ptr<JsonValue> to_json(const RpcRequest& r) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["jsonrpc"] = jstr("2.0");
    m["method"]  = jstr(r.method);
    if (r.id) m["id"] = r.id;
    if (r.params) m["params"] = r.params;
    return o;
}

std::shared_ptr<JsonValue> to_json(const RpcResponse& r) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["jsonrpc"] = jstr("2.0");
    // Spec: response MUST have an id; null is allowed when the request
    // failed parse (we couldn't read the id off the request).
    m["id"] = r.id ? r.id : jnull();
    if (r.error) {
        m["error"] = to_json(*r.error);
    } else {
        // Result is included even when null — JSON-RPC distinguishes
        // "result: null" from a missing result; the latter is malformed
        // when error is also absent.
        m["result"] = r.result ? r.result : jnull();
    }
    return o;
}

RpcRequest rpc_request_from_json(const JsonValue& v) {
    require_object(v, "rpc.request");
    if (auto j = v.get("jsonrpc"); !j || !j->is_string() || j->as_string() != "2.0") {
        throw_parse("rpc.jsonrpc", "expected \"2.0\"");
    }
    RpcRequest r;
    r.method = require_string_field(v, "method");
    r.id     = v.get("id");          // may be null/string/number; pass through
    r.params = v.get("params");      // may be null
    return r;
}

RpcResponse rpc_response_from_json(const JsonValue& v) {
    require_object(v, "rpc.response");
    if (auto j = v.get("jsonrpc"); !j || !j->is_string() || j->as_string() != "2.0") {
        throw_parse("rpc.jsonrpc", "expected \"2.0\"");
    }
    RpcResponse r;
    r.id = v.get("id");

    auto res_v = v.get("result");
    auto err_v = v.get("error");
    if (res_v && err_v && !err_v->is_null()) {
        throw_parse("rpc.response", "has both result and error");
    }
    if (err_v && err_v->is_object()) {
        RpcError e;
        if (auto c = err_v->get("code"); c && c->is_number())
            e.code = static_cast<int>(c->as_number());
        if (auto m = err_v->get("message"); m && m->is_string())
            e.message = m->as_string();
        e.data = err_v->get("data");
        r.error = std::move(e);
    } else if (res_v) {
        r.result = res_v;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Convenience constructors
// ---------------------------------------------------------------------------
RpcResponse make_error_response(const std::shared_ptr<JsonValue>& request_id,
                                int code,
                                std::string message,
                                std::shared_ptr<JsonValue> data) {
    RpcResponse r;
    r.id = request_id;
    RpcError e;
    e.code = code;
    e.message = std::move(message);
    e.data = std::move(data);
    r.error = std::move(e);
    return r;
}

RpcResponse make_result_response(const std::shared_ptr<JsonValue>& request_id,
                                 std::shared_ptr<JsonValue> result) {
    RpcResponse r;
    r.id     = request_id;
    r.result = std::move(result);
    return r;
}

} // namespace arbiter::a2a
