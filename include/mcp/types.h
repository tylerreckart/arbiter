#pragma once
// include/mcp/types.h — Wire types for the Model Context Protocol
//
// MCP rides JSON-RPC 2.0 over newline-delimited JSON on stdio.  Each
// message is one line of UTF-8 JSON terminated by '\n'; servers MUST
// NOT emit anything else on stdout (logs go to stderr).
//
// We model the subset arbiter actually uses:
//   • Request   — id-bearing call, expects a Response
//   • Response  — paired with a Request id; either `result` or `error`
//   • Notification — no id, no response (e.g. notifications/initialized)
//
// The richer MCP surface (resources, prompts, sampling) is intentionally
// out of scope for this first pass — we only need tools/list + tools/call
// to drive playwright and friends.  Adding more methods is purely about
// teaching mcp::Client new method names; the framing here is final.

#include "json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arbiter::mcp {

// JSON-RPC 2.0 error codes.  -32700..-32600 are reserved by the spec; the
// MCP-level errors live in the same envelope but use different codes
// chosen by the server (we don't interpret the numeric value, just
// surface message + code to the caller).
struct RpcError {
    int         code = 0;
    std::string message;
    std::shared_ptr<JsonValue> data;   // optional, server-defined
};

// Request: { "jsonrpc": "2.0", "id": <num|str>, "method": "...", "params": ... }
struct Request {
    int64_t                    id = 0;     // monotonic per-session counter
    std::string                method;
    std::shared_ptr<JsonValue> params;     // typically a JSON object; null = omitted
};

// Response: { "jsonrpc": "2.0", "id": <matches request>, "result": ... | "error": {...} }
struct Response {
    int64_t                    id = 0;
    std::shared_ptr<JsonValue> result;     // null on error path
    std::optional<RpcError>    error;      // populated on error path
};

// Notification: { "jsonrpc": "2.0", "method": "...", "params": ... }
// No id field, no response expected.
struct Notification {
    std::string                method;
    std::shared_ptr<JsonValue> params;
};

// One tool descriptor returned by tools/list.  We keep the input schema
// as a raw JSON blob — re-serialised into the system prompt verbatim so
// the agent sees the exact MCP-published spec without translation drift.
struct ToolDescriptor {
    std::string name;
    std::string description;
    std::shared_ptr<JsonValue> input_schema;   // the JSON Schema object as-is
};

// One content item from a tools/call response.  MCP defines text, image,
// and resource types; arbiter renders text inline and tags the rest as
// "[non-text content: <type>]" so the agent can adapt.
struct ContentItem {
    std::string type;       // "text" | "image" | "resource"
    std::string text;       // populated when type == "text"
    std::string mime_type;  // populated for image/resource
};

struct ToolResult {
    bool                     is_error = false;
    std::vector<ContentItem> content;
};

// Serialise a Request / Notification to its on-the-wire JSON line.  The
// '\n' terminator is *not* appended — the transport layer adds it after
// flushing, so callers can layer additional framing if needed.
std::string serialize_request(const Request& r);
std::string serialize_notification(const Notification& n);

// Parse one line of JSON into a Response.  Throws std::runtime_error on
// malformed framing (missing jsonrpc, both result+error, etc.).  Request
// ids we send are integers; inbound responses may echo the same value as
// a JSON number or as a decimal string (JSON-RPC 2.0 allows both).  String
// ids that parse as integers are stored as that integer so Client::rpc
// can match them.  Missing, null, or non-numeric string ids leave id=0
// (the notification / skip path).
Response parse_response(const std::string& line);

// Pull the tool array out of a tools/list response.  Throws if the
// shape doesn't match the spec.
std::vector<ToolDescriptor> parse_tools_list(const Response& r);

// Pull the content array + isError out of a tools/call response.  Throws
// if the shape doesn't match the spec.
ToolResult parse_tool_result(const Response& r);

// Render a ToolResult as the body of a [/mcp ...] tool-result block.
// Concatenates text content; flags non-text items inline.  Caller is
// responsible for the [/mcp call ...] header and [END MCP] footer.
std::string render_tool_result(const ToolResult& r);

} // namespace arbiter::mcp
