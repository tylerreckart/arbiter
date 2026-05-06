#pragma once
// include/a2a/client.h — Client for talking to remote A2A agents.
//
// One Client per configured remote.  Methods are synchronous from the
// caller's perspective — the streaming variant blocks until the upstream
// terminates (or `cancel` is set) and fires `on_event` inline.
//
// Auth model: bearer-only for v1.  The token is resolved at construction
// time (typically from an environment variable named in the registry
// config) so a token rotation requires reloading the registry.

#include "a2a/http.h"
#include "a2a/types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace index_ai::a2a {

// A2A server-sent event handed to the streaming consumer.  The caller
// dispatches on `kind` ("task" | "message" | "status-update" |
// "artifact-update") and parses `result` into the matching struct via
// the appropriate *_from_json helper.  Carrying the raw JsonValue lets
// us defer parsing to the consumer that actually needs the typed form
// — most consumers only care about a subset of events.
struct StreamEvent {
    std::string                kind;
    std::shared_ptr<JsonValue> result;
};

class Client {
public:
    Client(std::string name,
           std::string endpoint_url,
           std::string bearer_token,
           std::chrono::seconds rpc_timeout    = std::chrono::seconds(60),
           std::chrono::seconds stream_timeout = std::chrono::seconds(600));

    const std::string& name()         const { return name_; }
    const std::string& endpoint_url() const { return endpoint_url_; }

    // Lazily fetch and cache the agent card.  Discovery URL defaults
    // to `<endpoint>/agent-card.json` per the v1.0 path convention
    // arbiter's server uses.  Throws on transport or parse failure
    // — callers should wrap in try/catch and surface as ERR.
    const AgentCard& card();

    // Force a re-fetch of the card.  Useful when the registry says a
    // remote bumped its version; clears the cached copy then re-fetches.
    const AgentCard& refresh_card();

    // Synchronous JSON-RPC message/send.  Returns the resulting Task
    // on success; on failure populates `err_out` with a human-readable
    // string and returns std::nullopt.
    std::optional<Task> send_message(const Message& msg,
                                      std::string& err_out);

    // Streaming JSON-RPC message/stream.  Each event fires `on_event`
    // inline on the calling thread.  Caller can flip `cancel` from
    // another thread to abort cleanly.  Returns the final HttpResponse
    // — its error field is non-empty on transport failure or cancel.
    HttpResponse stream_message(const Message& msg,
                                  std::function<void(const StreamEvent&)> on_event,
                                  std::atomic<bool>& cancel);

    // Synchronous JSON-RPC tasks/get.  Returns the Task or nullopt on
    // error; populates `err_out`.
    std::optional<Task> get_task(const std::string& task_id,
                                   std::string& err_out);

    // Synchronous JSON-RPC tasks/cancel.  Returns the (now-canceled)
    // Task on success or nullopt on error.  Servers may legitimately
    // return ERR_TASK_NOT_CANCELABLE for terminal tasks; we surface
    // that as a non-null nullopt with a populated err_out.
    std::optional<Task> cancel_task(const std::string& task_id,
                                      std::string& err_out);

private:
    std::vector<HttpHeader> auth_headers() const;
    std::shared_ptr<JsonValue> rpc(const std::string& method,
                                    std::shared_ptr<JsonValue> params,
                                    std::string& err_out);

    std::string                  name_;
    std::string                  endpoint_url_;
    std::string                  bearer_token_;
    std::chrono::seconds         rpc_timeout_;
    std::chrono::seconds         stream_timeout_;

    mutable std::mutex           card_mu_;
    std::optional<AgentCard>     cached_card_;

    // Monotonic id counter for JSON-RPC requests.  The server doesn't
    // require unique ids per session — every method gets one — but
    // libcurl reuses the easy handle so a uniformly-incrementing id
    // helps when correlating server logs across calls.
    std::atomic<int64_t>         rpc_id_counter_{1};
};

} // namespace index_ai::a2a
