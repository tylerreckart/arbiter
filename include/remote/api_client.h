#pragma once
// HTTP client for a remote `arbiter --api` instance.  Used by the TUI
// `--connect` mode.  Trusted operator URL — SSRF guard is off so LAN /
// loopback home servers work.  Never logs or persists the bearer token.

#include "a2a/http.h"
#include "a2a/sse_reader.h"
#include "json.h"
#include "remote/connect_config.h"
#include "repl/conversation_store.h"
#include "repl/prompt_attachments.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace arbiter {

struct RemoteAgentInfo {
    std::string id;
    std::string name;
    std::string role;
    std::string model;
};

struct RemoteMessage {
    int64_t     id = 0;
    std::string role;
    std::string content;
    int         input_tokens = 0;
    int         output_tokens = 0;
    int64_t     created_at = 0;
    std::string request_id;
};

struct RemoteBootstrapResult {
    bool ok = false;
    std::string error;           // human-readable; empty on success
    std::string tenant_name;     // when known from probe / request_received
    int64_t     tenant_id = 0;
    std::vector<ConversationEntry> conversations;
    std::string active_conversation_id;  // newest or newly created
    std::vector<RemoteAgentInfo> agents;
};

class RemoteApiClient {
public:
    explicit RemoteApiClient(RemoteConnectConfig cfg);

    [[nodiscard]] const RemoteConnectConfig& config() const { return cfg_; }

    // GET /v1/health — fail-fast reachability.
    [[nodiscard]] a2a::HttpResponse health(long timeout_secs = 10) const;

    // Authenticated probe: GET /v1/conversations (and optionally agents).
    // Fails on transport error, unreachable, or 401/403.
    [[nodiscard]] RemoteBootstrapResult bootstrap() const;

    [[nodiscard]] std::vector<ConversationEntry>
    list_conversations(int limit = 50) const;

    // POST /v1/conversations — returns new id or empty + error out-param.
    [[nodiscard]] std::string create_conversation(std::string* error_out,
                                                   const std::string& title = {},
                                                   const std::string& agent_id = "index") const;

    [[nodiscard]] bool delete_conversation(const std::string& id,
                                            std::string* error_out) const;

    [[nodiscard]] bool patch_conversation_title(const std::string& id,
                                                 const std::string& title,
                                                 std::string* error_out) const;

    [[nodiscard]] std::vector<RemoteMessage>
    list_messages(const std::string& conversation_id,
                  std::string* error_out,
                  int limit = 200) const;

    [[nodiscard]] std::vector<RemoteAgentInfo>
    list_agents(std::string* error_out) const;

    // POST /v1/conversations/:id/messages — stream SSE events.
    // `on_event(name, data_json)` fires for each event.  Sets cancel to
    // abort the curl stream; also call cancel_request() with the id from
    // request_received for server-side cancel.
    // When `attachments` is non-empty, `message` is sent as a vision parts
    // array (same shape as POST /v1/orchestrate) instead of a bare string.
    a2a::HttpResponse post_message_stream(
        const std::string& conversation_id,
        const std::string& message,
        a2a::SseReader::EventCallback on_event,
        std::atomic<bool>& cancel,
        long timeout_secs = 600) const {
        return post_message_stream(conversation_id, message, {},
                                   std::move(on_event), cancel, timeout_secs);
    }
    a2a::HttpResponse post_message_stream(
        const std::string& conversation_id,
        const std::string& message,
        std::vector<PromptAttachment> attachments,
        a2a::SseReader::EventCallback on_event,
        std::atomic<bool>& cancel,
        long timeout_secs = 600) const;

    // POST /v1/requests/:id/cancel
    a2a::HttpResponse cancel_request(const std::string& request_id) const;

private:
    [[nodiscard]] std::vector<a2a::HttpHeader> auth_headers() const;
    [[nodiscard]] std::string url(std::string_view path) const;
    static ConversationEntry conversation_from_json(const JsonValue& v);

    RemoteConnectConfig cfg_;
};

} // namespace arbiter
