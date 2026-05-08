#pragma once
// include/a2a/event_translator.h — Translate arbiter orchestrator events
// into A2A streaming wire frames.
//
// A2A's message/stream is a JSON-RPC method that returns SSE.  Every
// chunk is a JSON-RPC response wrapping exactly one of:
//   • Task
//   • Message
//   • TaskStatusUpdateEvent
//   • TaskArtifactUpdateEvent
// The stream stays open until a TaskStatusUpdateEvent with final=true
// arrives.  Each frame is emitted as:
//
//   event: message
//   data: {"jsonrpc":"2.0","id":<rpc_id>,"result":{...}}
//
// A2aStreamWriter is the abstraction.  It takes a `Sink` callback that
// pushes already-serialised SSE frames downstream — api_server.cpp
// passes a closure over its TU-local SseStream so this header doesn't
// need to know about HTTP.  All writer methods are thread-safe via the
// sink's own synchronisation; the writer itself holds no mutable state
// beyond what it needs to thread task/context ids through every frame.
//
// Lifetime model: one writer per inbound message/stream call.  Cleanup
// is on the caller — they construct a writer, fire callbacks at it
// during the orchestrator's send_streaming, and destroy it before
// closing the sink.

#include "a2a/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace arbiter::a2a {

// SSE event sink.  Receives (event_name, payload).  api_server.cpp's
// adapter writes them to the SseStream as `event: <event_name>\ndata:
// <payload>\n\n`; the SseStream serialises the JsonValue itself, so we
// hand off the structured form rather than a pre-serialised string —
// avoids one round trip through json_serialize/json_parse on every
// frame.  event_name is always "message" for A2A frames.
using EventSink = std::function<void(const std::string& event_name,
                                      std::shared_ptr<JsonValue> payload)>;

class A2aStreamWriter {
public:
    A2aStreamWriter(EventSink sink,
                    std::shared_ptr<JsonValue> rpc_id,
                    std::string task_id,
                    std::string context_id,
                    std::string agent_id);

    const std::string& task_id()    const { return task_id_; }
    const std::string& context_id() const { return context_id_; }
    const std::string& agent_id()   const { return agent_id_; }

    // Emit a TaskStatusUpdateEvent.  `final=true` is the terminal
    // signal — callers must emit exactly one final status update per
    // stream and emit nothing afterwards.
    void emit_status(TaskState state, bool final,
                     std::optional<Message> msg = std::nullopt);

    // Append a text chunk to the streaming-text artifact.  The first
    // call lazily allocates the artifact id; subsequent calls reuse
    // it with append=true so a conformant client can concatenate the
    // chunks into one rendered message.  `last_chunk=true` signals
    // that no further text deltas will follow.
    void emit_text_chunk(const std::string& delta, bool last_chunk = false);

    // Emit a tool-call observation as a data-part artifact.  Carries
    // {tool, ok} under x-arbiter.tool_call so spec-aware clients can
    // surface it without arbiter-specific code paths.
    void emit_tool_call(const std::string& tool_name, bool ok);

    // Emit a captured file as a one-shot file-part artifact.  Bytes
    // are inlined; clients that prefer a URL-based reference can use
    // the persistent /v1/artifacts endpoints instead (separate path).
    void emit_file(const std::string& path,
                    const std::string& content,
                    const std::string& mime_type = "text/plain");

    // Emit a sub-agent's full text response as a data-part artifact.
    // arbiter folds depth>0 turns into a single artifact rather than
    // streaming their chunks — the user's primary stream is the
    // master agent's text.
    void emit_sub_agent(const std::string& sub_agent_id,
                         int depth,
                         const std::string& content);

    // Emit a free-form metadata event under x-arbiter.<kind>.  Used for
    // advisor / escalation / token_usage telemetry that doesn't have a
    // first-class A2A representation but is useful to downstream
    // diagnostic tooling.
    void emit_metadata(const std::string& kind,
                        std::shared_ptr<JsonValue> payload);

private:
    void write_jsonrpc_result(std::shared_ptr<JsonValue> result);

    EventSink                  sink_;
    std::shared_ptr<JsonValue> rpc_id_;
    std::string                task_id_;
    std::string                context_id_;
    std::string                agent_id_;

    // Lazily-allocated id for the running text artifact so concurrent
    // emit_text_chunk calls land on the same artifact.  Atomic because
    // orchestrator callbacks may fire from worker threads even though
    // arbiter's API server pins them to the request thread today;
    // belt-and-braces against future fan-out.
    std::atomic<bool> text_artifact_open_{false};
    std::string       text_artifact_id_;

    // Counter for sub-agent artifacts so each delegated turn lands on
    // its own artifact id (no append=true confusion across siblings).
    std::atomic<int>  sub_agent_counter_{0};
    std::atomic<int>  tool_call_counter_{0};
    std::atomic<int>  file_counter_{0};
    std::atomic<int>  metadata_counter_{0};
};

} // namespace arbiter::a2a
