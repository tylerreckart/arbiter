#pragma once
// index/include/api_client.h — Multi-provider LLM API client over raw TLS / TCP.
// Routes requests by model-string prefix: bare "claude-*" → Anthropic Messages
// API, "openai/<model>" → OpenAI Chat Completions, "ollama/<model>" → Ollama
// (OpenAI-compatible /v1/chat/completions), "gemini/<model>" → Google Gemini
// generateContent.  Adding a new provider is a prefix + one row in the provider
// table in api_client.cpp.

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
    std::string error_type;          // "advisor_halt" when gate halted
    std::string halt_reason;         // populated only when error_type=="advisor_halt"
    std::string raw_body;            // full response for debug
    std::string stop_reason;
    bool had_tool_calls = false;
};

using StreamCallback = std::function<void(const std::string& chunk)>;

// Provider descriptor.  Selected per-request by model-string prefix; each
// provider owns its host/port/path, whether TLS is required, which request
// and response formats to use, and whether an API key is needed.
struct Provider {
    enum Format { FORMAT_ANTHROPIC, FORMAT_OPENAI_CHAT, FORMAT_GEMINI };

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

// True when the model likely needs the "weak-executor" prompt profile — a
// leaner, tool-vocabulary-first system prompt with few-shot examples of
// tool emission.  Today that's local Ollama models; small local models
// (qwen-7b, llama3-8b, etc.) don't reliably invoke tools from abstract
// instructions the way frontier cloud models do.  Cloud providers
// (Anthropic, OpenAI) are treated as tool-fluent.
bool is_weak_executor(const std::string& model);

// Strip any provider prefix from a model string (e.g. "ollama/llama3:8b"
// → "llama3:8b").  What the actual API expects as the model name.
std::string strip_model_prefix(const std::string& model);

class ApiClient {
public:
    // Keys keyed by provider name ("anthropic", "openai", …).  Missing
    // entries are fine — a request routed to a provider without a key
    // fails with a clear per-request error rather than refusing to
    // construct.  Values are zeroed on destruction.
    explicit ApiClient(std::map<std::string, std::string> api_keys);
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
    // Each provider's API key is stored XOR-masked at rest so a passive
    // memory scan / core dump / swap read doesn't surface the raw token.
    // The mask is a same-length random buffer generated at construction;
    // callers obtain a short-lived plaintext copy via unmask_api_key(name)
    // and are expected to cleanse that copy as soon as the wire request
    // has been flushed.  A missing provider entry means "no key for that
    // provider" — the wire path rejects requests to it with a clear error
    // rather than failing construction.
    struct MaskedKey {
        std::vector<unsigned char> masked;
        std::vector<unsigned char> mask;
    };
    std::map<std::string, MaskedKey> api_keys_;
    std::string unmask_api_key(const std::string& provider) const;
    SSL_CTX* ssl_ctx_ = nullptr;

    std::mutex conn_mutex_;
    std::map<std::string, Conn> conns_;   // keyed by provider name

    std::atomic<int>  total_in_{0};
    std::atomic<int>  total_out_{0};
    std::atomic<bool> cancelled_{false};

    // Connection lifecycle per provider.
    bool ensure_connection(const Provider& p, Conn& c);
    void close_connection(Conn& c);

    // Wire I/O — `c` is assumed connected.  Gemini puts the model id in the
    // URL path (`/v1beta/models/<model>:generateContent`), so the path is
    // computed per-request in complete()/stream() and passed in here rather
    // than read off the static Provider record.
    void send_request(const Provider& p, Conn& c,
                      const std::string& path,
                      const std::string& body, bool streaming);
    std::string read_response(Conn& c);
    ApiResponse read_streaming_response(Conn& c, StreamCallback cb,
                                         Provider::Format fmt);

    // Format-specific helpers.  The openai builder needs the Provider to
    // branch on name (OpenAI proper vs Ollama) — they share a wire format
    // but differ on a handful of fields (max_completion_tokens,
    // stream_options, reasoning-model temperature handling).
    static std::string build_body_anthropic(const ApiRequest& req, bool streaming);
    static std::string build_body_openai   (const Provider& prov,
                                            const ApiRequest& req, bool streaming);
    static std::string build_body_gemini   (const ApiRequest& req);
    static ApiResponse parse_body_anthropic(const std::string& body);
    static ApiResponse parse_body_openai   (const std::string& body);
    static ApiResponse parse_body_gemini   (const std::string& body);
};

} // namespace index_ai
