#pragma once
// arbiter/include/api_client.h — Multi-provider LLM API client over raw TLS / TCP.
// Routes requests by model-string prefix: "ollama/<model>" → local Ollama
// (OpenAI-compatible /v1/chat/completions).  All hosted model ids route through
// OpenRouter's OpenAI-compatible chat endpoint, including canonical slugs like
// "openai/...", "anthropic/...", "google/...", and the explicit
// "openrouter/<slug>" prefix.

#include "json.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>
#include <atomic>

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace arbiter {

// Multipart content block.  Vision input requires interleaved text and
// image parts; this carries either, with image data either inlined as base64
// or referenced by URL.  The runtime never holds image bytes raw — they are
// always base64 by the time they reach this struct, both because the wire
// formats are base64 anyway and because keeping a single representation
// simplifies the body builders.
struct ContentPart {
    enum Kind { TEXT, IMAGE };
    Kind kind = TEXT;

    // TEXT
    std::string text;

    // IMAGE — exactly one of {image_data, image_url} is populated.
    // image_data is the raw base64 (no `data:` prefix); media_type is the
    // MIME type ("image/png", "image/jpeg", "image/webp", "image/gif").
    // image_url is a hosted URL the provider will fetch — passed through
    // unchanged on the OpenAI and Anthropic paths, expressed as fileData
    // on Gemini.
    std::string image_data;
    std::string media_type;
    std::string image_url;
};

// Compact finished-tool chrome for TUI transcript replay.  Not sent to
// model providers — UI/session persistence only.
struct ToolTraceEntry {
    std::string id;
    std::string label;
    std::string kind;
    std::string detail;
    bool        ok = true;
    std::string result_preview;
};

struct Message {
    std::string role;                // "user" | "assistant"

    // Two representations.  When `parts` is non-empty, it is authoritative
    // and `content` is ignored — body builders walk parts to emit each
    // provider's multipart content shape.  When `parts` is empty, `content`
    // is treated as a single text part (the legacy text-only path).  This
    // keeps every existing call site (which only writes `content`) working
    // unchanged while allowing new code to attach images.
    std::string content;
    std::vector<ContentPart> parts;

    // Finished tool rows that followed this assistant turn in the TUI.
    // Populated by the REPL after /cmd dispatch; ignored by API body builders.
    std::vector<ToolTraceEntry> tool_trace;

    // Provider reasoning/thinking accumulated for this assistant turn
    // (Anthropic thinking_delta, OpenAI reasoning_content, Gemini thought
    // parts).  Persisted for conversation-switch replay; never sent upstream.
    std::string thinking;
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
    // Accumulated provider reasoning/thinking text (when emitted).  Empty
    // for models that don't stream a separate reasoning channel.
    std::string reasoning;
    // Set when gate-mode advisor returned CONTINUE on the terminating turn.
    // Outer loops (LoopManager) use this to distinguish "task done" from
    // "send() returned ok but hit kMaxTurns before the gate could fire".
    bool gate_approved = false;
};

using StreamCallback = std::function<void(const std::string& chunk)>;
// Optional side-channel for provider reasoning/thinking deltas (Anthropic
// thinking_delta, OpenAI reasoning_content, Gemini thought parts).  Not mixed
// into StreamCallback text so the TUI can render a collapsible ThinkingSegment
// separately.
using ReasoningCallback = std::function<void(const std::string& delta)>;

// Provider descriptor.  Selected per-request by model-string prefix; each
// provider owns its host/port/path, whether TLS is required, which request
// and response formats to use, and whether an API key is needed.
struct Provider {
    enum Format { FORMAT_ANTHROPIC, FORMAT_OPENAI_CHAT, FORMAT_GEMINI };

    std::string name;            // "openrouter", "ollama", …
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
// (OpenRouter-hosted models) are treated as tool-fluent.
bool is_weak_executor(const std::string& model);

// Strip any provider prefix from a model string (e.g. "ollama/llama3:8b"
// → "llama3:8b").  What the actual API expects as the model name.
std::string strip_model_prefix(const std::string& model);

class Metrics;
class ProviderCircuitBreaker;

class ApiClient {
public:
    // Keys keyed by provider name ("openrouter").  Missing
    // entries are fine — a request routed to a provider without a key
    // fails with a clear per-request error rather than refusing to
    // construct.  Values are zeroed on destruction.
    explicit ApiClient(std::map<std::string, std::string> api_keys);
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    ApiResponse complete(const ApiRequest& req);
    ApiResponse stream(const ApiRequest& req, StreamCallback cb);

    // Optional reasoning/thinking stream sink.  Fired from the same thread
    // as StreamCallback when a provider emits a separate reasoning channel.
    void set_reasoning_callback(ReasoningCallback cb) { reasoning_cb_ = std::move(cb); }

    int total_input_tokens()  const { return total_in_.load(); }
    int total_output_tokens() const { return total_out_.load(); }
    void reset_stats() { total_in_ = 0; total_out_ = 0; }

    // Attach the process-wide circuit breaker.  When set, complete()
    // and stream() check `allow(provider)` before each upstream call
    // and record success/failure on the result.  Non-owning pointer;
    // null is fine and disables the breaker layer (legacy behaviour).
    void set_circuit_breaker(ProviderCircuitBreaker* cb) { breaker_ = cb; }
    ProviderCircuitBreaker* circuit_breaker() const { return breaker_; }

    // Attach the process-wide metrics registry.  When set, every
    // upstream call increments arbiter_provider_calls_total and the
    // appropriate retry / 5xx / 429 counters.  Non-owning; null is
    // fine.
    void set_metrics(Metrics* m) { metrics_ = m; }
    Metrics* metrics() const { return metrics_; }

    // Interrupt any in-progress streaming call.  Shuts down every open socket
    // so an in-flight SSL_read / read returns immediately.  Thread-safe.
    void cancel();

    // Pure helpers — request body builders.  Public so unit tests can verify
    // each provider's wire shape directly without spinning up a mock server.
    // The OpenAI-compatible builder branches on provider name (OpenRouter vs
    // Ollama) because they share a wire format but differ on a few fields.
    static std::string build_body_anthropic(const ApiRequest& req, bool streaming);
    static std::string build_body_openai   (const Provider& prov,
                                            const ApiRequest& req, bool streaming);
    static std::string build_body_gemini   (const ApiRequest& req);

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
    // Returns the unmasked key.  The caller must zero the returned string
    // before it goes out of scope; call sites wrap it in SensitiveString
    // (defined in api_client.cpp) for automatic zeroing on scope exit.
    std::string unmask_api_key(const std::string& provider) const;
    SSL_CTX* ssl_ctx_ = nullptr;

    // Per-provider connection pool.  The original design guarded a single
    // keep-alive Conn per provider with one global mutex (conn_mutex_) held
    // for the *entire* request — including the full streaming read — so only
    // one upstream call could be in flight across the whole process at a
    // time.  Every pane and sub-agent shares one ApiClient (Orchestrator has
    // exactly one), so they all serialized behind that lock even when talking
    // to different providers (issue #48).  Now each provider owns a small
    // pool: concurrent callers lease distinct Conns and do their socket I/O
    // with no shared lock held, so different panes — and different providers
    // — stream in parallel.  pool_mutex_ guards only the provider→pool map
    // lookup; ProviderPool::mu guards that pool's lease bookkeeping.  Neither
    // is ever held during network I/O.
    struct ProviderPool {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::unique_ptr<Conn>> conns;  // every Conn (leased or idle)
        std::vector<Conn*> idle;                    // subset available to lease
    };

    // Cap on concurrent connections per provider.  Bounds socket/fd growth
    // under heavy fan-out; callers past the cap briefly wait on the pool's
    // condition variable for a lease to free up rather than opening unbounded
    // sockets.  Comfortably exceeds kMaxPanes (8) plus the advisor / title /
    // a2a side calls that share this client.
    static constexpr int kMaxConnsPerProvider = 16;

    std::mutex pool_mutex_;                                       // guards pools_
    std::map<std::string, std::unique_ptr<ProviderPool>> pools_;

    // Find-or-create the pool for a provider (brief pool_mutex_ hold).
    ProviderPool& pool_for(const std::string& provider);

    // RAII connection lease.  Checks a Conn out of its provider pool on
    // construction (blocking on the pool CV if every slot is leased and the
    // cap is reached) and returns it to the idle set on destruction.  All
    // socket I/O runs on the leased Conn with no pool lock held, so leases on
    // different Conns proceed fully concurrently.  Nested so it can reach the
    // private Conn / ProviderPool types.
    class ConnLease {
    public:
        ConnLease(ApiClient& owner, const std::string& provider);
        ~ConnLease();
        ConnLease(const ConnLease&) = delete;
        ConnLease& operator=(const ConnLease&) = delete;
        Conn& conn() { return *conn_; }
    private:
        ProviderPool& pool_;
        Conn* conn_;
    };

    std::atomic<int>  total_in_{0};
    std::atomic<int>  total_out_{0};
    std::atomic<bool> cancelled_{false};

    // Process-wide observability hooks.  Both nullable; CLI / unit-test
    // contexts leave them null and complete() / stream() short-circuit
    // the bookkeeping.  Lifetime is the caller's responsibility (the
    // ApiServer owns these for the running-server case).
    ProviderCircuitBreaker* breaker_ = nullptr;
    Metrics*                metrics_ = nullptr;
    ReasoningCallback       reasoning_cb_;

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

    // Format-specific response parsers (kept private — they own the
    // ApiResponse construction and aren't used outside the wire path).
    static ApiResponse parse_body_anthropic(const std::string& body);
    static ApiResponse parse_body_openai   (const std::string& body);
    static ApiResponse parse_body_gemini   (const std::string& body);
};

} // namespace arbiter
