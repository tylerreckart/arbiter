#pragma once
// index/include/api_client.h — Claude API client over raw TLS sockets
// Zero dependencies beyond OpenSSL. Reuses connections.

#include "json.h"
#include <string>
#include <vector>
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
    // Advisor tool: if non-empty, include advisor_20260301 tool and beta header.
    std::string advisor_model;
};

struct ApiResponse {
    bool ok = false;
    std::string content;             // extracted text
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;       // prompt cache hits (count 1/10 toward rate limit)
    int cache_creation_tokens = 0;   // tokens written into cache this request
    std::string error;
    std::string error_type;          // e.g. "rate_limit_error", "overloaded_error"
    std::string raw_body;            // full response for debug
    std::string stop_reason;
    bool had_tool_calls = false;     // true if orchestrator ran any /fetch or /mem commands
};

// Streaming callback: receives chunks as they arrive
using StreamCallback = std::function<void(const std::string& chunk)>;

class ApiClient {
public:
    explicit ApiClient(const std::string& api_key);
    ~ApiClient();

    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    // Blocking completion
    ApiResponse complete(const ApiRequest& req);

    // Streaming completion (prints chunks via callback)
    ApiResponse stream(const ApiRequest& req, StreamCallback cb);

    // Stats
    int total_input_tokens()  const { return total_in_.load(); }
    int total_output_tokens() const { return total_out_.load(); }
    void reset_stats() { total_in_ = 0; total_out_ = 0; }

    // Interrupt any in-progress streaming call.  Thread-safe.
    // Shuts down the socket so SSL_read returns immediately, then marks the
    // client as cancelled.  The cancel flag is cleared at the start of the
    // next stream() or complete() call.
    void cancel();

private:
    std::string api_key_;
    SSL_CTX* ssl_ctx_ = nullptr;

    std::mutex conn_mutex_;
    SSL* ssl_ = nullptr;
    int  sock_ = -1;
    bool connected_ = false;
    std::string last_conn_error_;  // populated by ensure_connection() on failure

    std::atomic<int>  total_in_{0};
    std::atomic<int>  total_out_{0};
    std::atomic<bool> cancelled_{false};

    bool ensure_connection();
    void close_connection();
    std::string send_request(const std::string& body, bool streaming, bool advisor = false);
    std::string read_response(bool streaming, StreamCallback cb);
    ApiResponse read_streaming_response(StreamCallback cb);

    static std::string build_request_body(const ApiRequest& req, bool streaming);
    static ApiResponse parse_response(const std::string& body);
};

} // namespace index_ai
