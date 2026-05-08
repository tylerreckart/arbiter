#pragma once
// include/a2a/types.h — Wire types for the Agent2Agent (A2A) protocol v1.0.
//
// A2A is JSON-RPC 2.0 over HTTPS with SSE for streaming.  Two surfaces ride
// the same shapes:
//   • server: arbiter exposes its agents at POST /v1/a2a/agents/:id and
//     emits status/artifact update events on a Server-Sent Events stream.
//   • client: arbiter delegates to remote A2A agents listed in
//     ~/.arbiter/a2a_agents.json; the master agent picks them by name and
//     their streamed updates are reprojected into arbiter's own SSE log.
//
// We model the v1.0 subset arbiter actually needs:
//   • AgentCard (with Skill, Capabilities, security blob)
//   • Message + Part (text / file / data)
//   • Task + TaskStatus + Artifact
//   • TaskStatusUpdateEvent + TaskArtifactUpdateEvent
//   • JSON-RPC 2.0 envelope (RpcRequest, RpcResponse, RpcError)
//
// Things we deliberately leave as opaque JsonValue blobs (round-tripped
// verbatim, never interpreted): securitySchemes, security, signatures,
// extensions, metadata.  Spec drift on those bits is loud and frequent
// and arbiter has no policy decisions that depend on their structure.
//
// Parse functions throw std::runtime_error on malformed *required* fields.
// Missing optionals stay empty.  Serializers never throw.

#include "json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arbiter::a2a {

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 error codes
// ---------------------------------------------------------------------------
// Standard JSON-RPC reserves -32768..-32000.  A2A defines its own range
// in -32099..-32001 below for protocol-specific failures.  We use the
// constants instead of magic numbers at every call site.
constexpr int RPC_PARSE_ERROR        = -32700;
constexpr int RPC_INVALID_REQUEST    = -32600;
constexpr int RPC_METHOD_NOT_FOUND   = -32601;
constexpr int RPC_INVALID_PARAMS     = -32602;
constexpr int RPC_INTERNAL_ERROR     = -32603;

// A2A-specific codes (Section 9.5 of the v1.0 spec).
constexpr int ERR_TASK_NOT_FOUND          = -32001;
constexpr int ERR_TASK_NOT_CANCELABLE     = -32002;
constexpr int ERR_PUSH_NOT_SUPPORTED      = -32003;
constexpr int ERR_UNSUPPORTED_OPERATION   = -32004;
constexpr int ERR_CONTENT_TYPE_INVALID    = -32005;
constexpr int ERR_INVALID_AGENT_RESPONSE  = -32006;
constexpr int ERR_VERSION_NOT_SUPPORTED   = -32007;

// ---------------------------------------------------------------------------
// Task lifecycle states.  Wire form is the lowercase hyphenated string
// (e.g. "input-required"); the unknown enumerator is what we surface when
// a server returns a state we don't recognise — callers should treat it
// as terminal-unknown rather than crashing.
// ---------------------------------------------------------------------------
enum class TaskState {
    submitted,
    working,
    input_required,
    auth_required,
    completed,
    canceled,
    failed,
    rejected,
    unknown,
};

std::string  task_state_to_string(TaskState s);
TaskState    task_state_from_string(const std::string& s);
bool         task_state_is_terminal(TaskState s);   // completed|canceled|failed|rejected

// ---------------------------------------------------------------------------
// Part.  Each Part is exactly one of text / file / data; the kind field
// is the discriminator.  File parts carry either inline bytes (base64 in
// JSON) or a URL — never both.  We keep both as optionals on one struct
// rather than a variant because the wire encoding is the same shape and
// constructing from JSON benefits from in-place population.
// ---------------------------------------------------------------------------
struct Part {
    std::string kind;                          // "text" | "file" | "data"

    // kind == "text"
    std::string text;

    // kind == "file"
    std::optional<std::string> file_bytes;     // base64-encoded
    std::optional<std::string> file_uri;
    std::optional<std::string> file_name;
    std::optional<std::string> file_mime_type;

    // kind == "data"
    std::shared_ptr<JsonValue> data;

    // common
    std::shared_ptr<JsonValue> metadata;       // opaque, optional
};

// ---------------------------------------------------------------------------
// Message.  Always carries kind="message" on the wire; we omit the field
// from the struct since it's a constant and rebuild it on serialize.
// referenceTaskIds + extensions are passed through opaquely.
// ---------------------------------------------------------------------------
struct Message {
    std::string                role;            // "user" | "agent"
    std::vector<Part>          parts;
    std::string                message_id;      // required
    std::optional<std::string> task_id;
    std::optional<std::string> context_id;
    std::shared_ptr<JsonValue> metadata;
    std::shared_ptr<JsonValue> extensions;      // raw array; never inspected
    std::shared_ptr<JsonValue> reference_task_ids; // raw array
};

// ---------------------------------------------------------------------------
// Artifact.  Used both as a streaming surface (TaskArtifactUpdateEvent)
// and a terminal payload on Task.artifacts.  The artifactId is required
// per spec — we generate UUIDs on the server side.
// ---------------------------------------------------------------------------
struct Artifact {
    std::string                artifact_id;     // required
    std::vector<Part>          parts;           // at least one in practice
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::shared_ptr<JsonValue> metadata;
    std::shared_ptr<JsonValue> extensions;
};

// ---------------------------------------------------------------------------
// TaskStatus.  state is required; message and timestamp are optional.
// We keep the timestamp as an opaque ISO-8601 string — the spec doesn't
// require monotonic ordering and we never compute against it.
// ---------------------------------------------------------------------------
struct TaskStatus {
    TaskState                  state = TaskState::submitted;
    std::optional<Message>     message;         // optional human-readable update
    std::optional<std::string> timestamp;       // ISO-8601, optional
};

// ---------------------------------------------------------------------------
// Task.  Returned synchronously from message/send and via tasks/get.
// history is the persisted message log (user + agent turns).  The
// metadata blob is used to thread arbiter-specific extension data
// (token usage, model, etc.) under x-arbiter.* namespaces.
// ---------------------------------------------------------------------------
struct Task {
    std::string                id;
    std::string                context_id;
    TaskStatus                 status;
    std::vector<Artifact>      artifacts;
    std::vector<Message>       history;
    std::shared_ptr<JsonValue> metadata;
};

// ---------------------------------------------------------------------------
// Streaming event types.  The two events ride alongside Message and Task
// in the SSE response stream of message/stream and tasks/resubscribe.
// final=true on a TaskStatusUpdateEvent indicates the stream is closing.
// ---------------------------------------------------------------------------
struct TaskStatusUpdateEvent {
    std::string                task_id;
    std::string                context_id;
    TaskStatus                 status;
    bool                       final = false;
    std::shared_ptr<JsonValue> metadata;
};

struct TaskArtifactUpdateEvent {
    std::string                task_id;
    std::string                context_id;
    Artifact                   artifact;
    std::optional<bool>        append;        // true => append parts to prior artifact with same id
    std::optional<bool>        last_chunk;    // true => no further chunks for this artifact
    std::shared_ptr<JsonValue> metadata;
};

// ---------------------------------------------------------------------------
// AgentCard subset.  We model only the fields we read or emit; security
// schemes and signatures are passed through as opaque JsonValue blobs
// because (a) v1.0 has five scheme variants we don't enforce locally and
// (b) signature verification is out of scope for v1.
// ---------------------------------------------------------------------------
struct Skill {
    std::string                id;
    std::string                name;
    std::string                description;
    std::vector<std::string>   tags;
    std::vector<std::string>   examples;
    std::vector<std::string>   input_modes;
    std::vector<std::string>   output_modes;
};

struct AgentCardCapabilities {
    bool streaming                    = false;
    bool push_notifications           = false;
    bool state_transition_history     = false;
    std::shared_ptr<JsonValue> extensions;     // opaque array
};

struct AgentCard {
    // Required.
    std::string                protocol_version = "1.0";
    std::string                name;
    std::string                description;
    std::string                url;
    std::string                version;            // agent's own version, not protocol
    std::vector<std::string>   default_input_modes;
    std::vector<std::string>   default_output_modes;
    std::vector<Skill>         skills;
    AgentCardCapabilities      capabilities;

    // Optional / opaque pass-through.
    std::optional<std::string> preferred_transport;     // "JSONRPC" by default
    std::optional<std::string> documentation_url;
    std::optional<std::string> icon_url;
    std::shared_ptr<JsonValue> provider;                // {organization, url}
    std::shared_ptr<JsonValue> additional_interfaces;   // array
    std::shared_ptr<JsonValue> security_schemes;        // object map
    std::shared_ptr<JsonValue> security;                // array of object maps
    std::shared_ptr<JsonValue> signatures;              // array
    std::optional<bool>        supports_authenticated_extended_card;
};

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 envelope.  id may be string, number, or null in the spec;
// we accept all three on parse and round-trip what we received on
// response.  Keeping it as a JsonValue avoids lossy coercion.
// ---------------------------------------------------------------------------
struct RpcError {
    int                        code = 0;
    std::string                message;
    std::shared_ptr<JsonValue> data;          // optional, opaque
};

struct RpcRequest {
    std::shared_ptr<JsonValue> id;            // null/string/number
    std::string                method;
    std::shared_ptr<JsonValue> params;        // typically an object
};

struct RpcResponse {
    std::shared_ptr<JsonValue> id;
    std::shared_ptr<JsonValue> result;        // mutually exclusive with error
    std::optional<RpcError>    error;
};

// ---------------------------------------------------------------------------
// Serialize: every type below produces a JsonValue we can drop into a
// larger document or hand to json_serialize().  Pure functions, no I/O.
// ---------------------------------------------------------------------------
std::shared_ptr<JsonValue> to_json(const Part& p);
std::shared_ptr<JsonValue> to_json(const Message& m);
std::shared_ptr<JsonValue> to_json(const Artifact& a);
std::shared_ptr<JsonValue> to_json(const TaskStatus& s);
std::shared_ptr<JsonValue> to_json(const Task& t);
std::shared_ptr<JsonValue> to_json(const TaskStatusUpdateEvent& e);
std::shared_ptr<JsonValue> to_json(const TaskArtifactUpdateEvent& e);
std::shared_ptr<JsonValue> to_json(const Skill& s);
std::shared_ptr<JsonValue> to_json(const AgentCardCapabilities& c);
std::shared_ptr<JsonValue> to_json(const AgentCard& c);
std::shared_ptr<JsonValue> to_json(const RpcError& e);

// Build a JSON-RPC 2.0 response envelope.  If `error` is set on the
// response, `result` is omitted; otherwise `result` is included even when
// it's a JSON null (the spec distinguishes "result: null" from a missing
// result).  Always includes "jsonrpc": "2.0".
std::shared_ptr<JsonValue> to_json(const RpcResponse& r);

// Serialize a request envelope.  Used by the client (PR-5+).
std::shared_ptr<JsonValue> to_json(const RpcRequest& r);

// ---------------------------------------------------------------------------
// Parse: throw std::runtime_error("a2a parse: <field>: <reason>") on any
// missing-or-malformed required field.  Optional fields are silently
// dropped on type mismatch — clients shouldn't refuse a card just because
// an optional iconUrl came in as a number.
// ---------------------------------------------------------------------------
Part                       part_from_json(const JsonValue& v);
Message                    message_from_json(const JsonValue& v);
Artifact                   artifact_from_json(const JsonValue& v);
TaskStatus                 task_status_from_json(const JsonValue& v);
Task                       task_from_json(const JsonValue& v);
TaskStatusUpdateEvent      task_status_update_from_json(const JsonValue& v);
TaskArtifactUpdateEvent    task_artifact_update_from_json(const JsonValue& v);
Skill                      skill_from_json(const JsonValue& v);
AgentCardCapabilities      capabilities_from_json(const JsonValue& v);
AgentCard                  agent_card_from_json(const JsonValue& v);

RpcRequest                 rpc_request_from_json(const JsonValue& v);
RpcResponse                rpc_response_from_json(const JsonValue& v);

// ---------------------------------------------------------------------------
// Convenience: build a JSON-RPC error response from an in-flight request.
// Echoes the original id so the client can correlate.  If id is null
// (notification) the response is still well-formed but should normally
// be discarded by the transport layer.
// ---------------------------------------------------------------------------
RpcResponse make_error_response(const std::shared_ptr<JsonValue>& request_id,
                                int code,
                                std::string message,
                                std::shared_ptr<JsonValue> data = nullptr);

RpcResponse make_result_response(const std::shared_ptr<JsonValue>& request_id,
                                 std::shared_ptr<JsonValue> result);

} // namespace arbiter::a2a
