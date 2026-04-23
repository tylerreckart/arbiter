#pragma once
// index/include/api_client.h — Multi-provider LLM API client over raw TLS / TCP.
// Routes requests by model-string prefix: "claude-*" → Anthropic Messages API,
// "ollama/<model>" → Ollama (OpenAI-compatible /v1/chat/completions).  Adding
// a new provider is a prefix + one row in the provider table in api_client.cpp.

#include "json.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <atomic>

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace index_ai {

struct Message {
    std::string role;    // "user" | "assistant"
    std::string content;
};

struct ApiRequest {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    int max_tokens = 1024;
    double temperature = 0.3;
    bool include_temperature = true;
};

struct ApiResponse {
    bool ok = false;
    std::string content;             // extracted text
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;       // prompt cache hits (Anthropic only)
    int cache_creation_tokens = 0;   // tokens written into cache (Anthropic only)
    std::string error;
    std::string error_type;
    std::string raw_body;            // full response for debug
    std::string stop_reason;
    bool had_tool_calls = false;
};

using StreamCallback = std::function<void(const std::string& chunk)>;

// Provider descriptor.  Selected per-request by model-string prefix; each
// provider owns its host/port/path, whether TLS is required, which request
// and response formats to use, and whether an API key is needed.
struct Provider {
    enum Format { FORMAT_ANTHROPIC, FORMAT_OPENAI_CHAT };

    std::string name;            // "anthropic", "ollama", …
    std::string prefix;          // match against req.model ("" = fallback)
    std::string host;
    int         port = 443;
    std::string path;
    bool        tls = true;
    bool        uses_api_key = true;
    Format      format = FORMAT_ANTHROPIC;
};

// Resolve a provider from a model string.  Matches the longest prefix first
// so more-specific entries (e.g. "claude-opus") can override a catch-all.
// Returns a reference into the static registry — never null.
const Provider& provider_for(const std::string& model);

// True when pricing tables in cost_tracker apply.  Local providers return
// false so CostTracker can skip them.
bool is_priced(const std::string& model);

// True when the model likely needs the "weak-executor" prompt profile — a
// leaner, tool-vocabulary-first system prompt with few-shot examples of
// tool emission.  Currently any non-Anthropic provider qualifies, since
// small local models (qwen-7b, llama3-8b, etc.) don't reliably invoke
// tools from abstract instructions the way Claude does.  If we ever add a
// local model that's tool-fluent, the rule can be tightened without
// changing callers.
bool is_weak_executor(const std::string& model);

// Strip any provider prefix from a model string (e.g. "ollama/llama3:8b"
// → "llama3:8b").  What the actual API expects as the model name.
std::string strip_model_prefix(const std::string& model);

class ApiClient {
public:
    explicit ApiClient(const std::string& anthropic_key);
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    ApiResponse complete(const ApiRequest& req);
    ApiResponse stream(const ApiRequest& req, StreamCallback cb);

    int total_input_tokens()  const { return total_in_.load(); }
    int total_output_tokens() const { return total_out_.load(); }
    void reset_stats() { total_in_ = 0; total_out_ = 0; }

    // Interrupt any in-progress streaming call.  Shuts down every open socket
    // so an in-flight SSL_read / read returns immediately.  Thread-safe.
    void cancel();

    // Connection slot — one per active provider.  Public so free-function
    // wire helpers (conn_send / conn_recv in api_client.cpp) can operate on
    // it without a friend declaration; treat as an implementation detail.
    struct Conn {
        SSL* ssl = nullptr;
        int  sock = -1;
        bool connected = false;
        bool tls = true;
        std::string last_error;
    };

private:
    // API key is stored XOR-masked at rest so a passive memory scan / core
    // dump doesn't surface the raw key.  The mask is a per-process random
    // buffer generated in the constructor; callers obtain a short-lived
    // plaintext copy via unmask_api_key() and are expected to cleanse that
    // copy as soon as the wire representation is flushed.
    std::vector<unsigned char> api_key_masked_;
    std::vector<unsigned char> api_key_mask_;
    std::string unmask_api_key() const;
    SSL_CTX* ssl_ctx_ = nullptr;

    std::mutex conn_mutex_;
    std::map<std::string, Conn> conns_;   // keyed by provider name

    std::atomic<int>  total_in_{0};
    std::atomic<int>  total_out_{0};
    std::atomic<bool> cancelled_{false};

    // Connection lifecycle per provider.
    bool ensure_connection(const Provider& p, Conn& c);
    void close_connection(Conn& c);

    // Wire I/O — `c` is assumed connected.
    void send_request(const Provider& p, Conn& c,
                      const std::string& body, bool streaming);
    std::string read_response(Conn& c);
    ApiResponse read_streaming_response(Conn& c, StreamCallback cb,
                                         Provider::Format fmt);

    // Format-specific helpers.
    static std::string build_body_anthropic(const ApiRequest& req, bool streaming);
    static std::string build_body_openai   (const ApiRequest& req, bool streaming);
    static ApiResponse parse_body_anthropic(const std::string& body);
    static ApiResponse parse_body_openai   (const std::string& body);
};

} // namespace index_ai
