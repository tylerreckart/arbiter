// index_ai/src/api_client.cpp — Claude API client implementation
#include "api_client.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>

static constexpr const char* API_HOST = "api.anthropic.com";
static constexpr int         API_PORT = 443;
static constexpr const char* API_PATH = "/v1/messages";
static constexpr const char* API_VERSION = "2023-06-01";

namespace index_ai {

ApiClient::ApiClient(const std::string& api_key) : api_key_(api_key) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) throw std::runtime_error("Failed to create SSL context");

    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ssl_ctx_);
}

ApiClient::~ApiClient() {
    close_connection();
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

bool ApiClient::ensure_connection() {
    if (connected_) return true;

    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(API_HOST, "443", &hints, &res);
    if (gai != 0) {
        last_conn_error_ = std::string("DNS lookup failed: ") + gai_strerror(gai);
        return false;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) {
        last_conn_error_ = std::string("socket() failed: ") + strerror(errno);
        freeaddrinfo(res);
        return false;
    }

    // TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
        last_conn_error_ = std::string("connect() failed: ") + strerror(errno);
        freeaddrinfo(res);
        close(sock_);
        sock_ = -1;
        return false;
    }
    freeaddrinfo(res);

    // TLS handshake
    ssl_ = SSL_new(ssl_ctx_);
    SSL_set_fd(ssl_, sock_);
    SSL_set_tlsext_host_name(ssl_, API_HOST);

    if (SSL_connect(ssl_) != 1) {
        unsigned long err = ERR_get_error();
        char errbuf[256];
        ERR_error_string_n(err, errbuf, sizeof(errbuf));
        last_conn_error_ = std::string("TLS handshake failed: ") + errbuf;
        SSL_free(ssl_);
        ssl_ = nullptr;
        close(sock_);
        sock_ = -1;
        return false;
    }

    connected_ = true;
    return true;
}

void ApiClient::cancel() {
    cancelled_.store(true);
    // Shut down the socket so any blocking SSL_read returns immediately.
    // We use shutdown() rather than close() to avoid a race with the thread
    // that owns the fd — the streaming loop will call close_connection() on
    // its own after seeing the error.
    std::lock_guard<std::mutex> lk(conn_mutex_);
    if (sock_ >= 0) ::shutdown(sock_, SHUT_RDWR);
}

void ApiClient::close_connection() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
    connected_ = false;
}

std::string ApiClient::build_request_body(const ApiRequest& req, bool streaming) {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["model"] = jstr(req.model);
    m["max_tokens"] = jnum(static_cast<double>(req.max_tokens));
    m["temperature"] = jnum(req.temperature);

    if (!req.system_prompt.empty()) {
        // Structured system block with prompt caching — cached tokens count at
        // 1/10th toward input rate limits after the first request in a 5-min window.
        auto cache_ctrl = jobj();
        cache_ctrl->as_object_mut()["type"] = jstr("ephemeral");

        auto sys_block = jobj();
        auto& sb = sys_block->as_object_mut();
        sb["type"]          = jstr("text");
        sb["text"]          = jstr(req.system_prompt);
        sb["cache_control"] = cache_ctrl;

        auto sys_arr = jarr();
        sys_arr->as_array_mut().push_back(sys_block);
        m["system"] = sys_arr;
    }

    auto msgs = jarr();
    for (auto& msg : req.messages) {
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);
        mo->as_object_mut()["content"] = jstr(msg.content);
        msgs->as_array_mut().push_back(mo);
    }
    m["messages"] = msgs;

    if (streaming) {
        m["stream"] = jbool(true);
    }

    // Advisor tool (beta: advisor-tool-2026-03-01)
    if (!req.advisor_model.empty()) {
        auto tool = jobj();
        tool->as_object_mut()["type"]  = jstr("advisor_20260301");
        tool->as_object_mut()["name"]  = jstr("advisor");
        tool->as_object_mut()["model"] = jstr(req.advisor_model);
        auto tools_arr = jarr();
        tools_arr->as_array_mut().push_back(tool);
        m["tools"] = tools_arr;
    }

    return json_serialize(*obj);
}

std::string ApiClient::send_request(const std::string& body, bool streaming, bool advisor) {
    std::ostringstream http;
    http << "POST " << API_PATH << " HTTP/1.1\r\n";
    http << "Host: " << API_HOST << "\r\n";
    http << "Content-Type: application/json\r\n";
    http << "x-api-key: " << api_key_ << "\r\n";
    http << "anthropic-version: " << API_VERSION << "\r\n";
    // Combine all beta features into a single header
    std::string beta = "prompt-caching-2024-07-31";
    if (advisor) beta += ", advisor-tool-2026-03-01";
    http << "anthropic-beta: " << beta << "\r\n";
    http << "Content-Length: " << body.size() << "\r\n";
    if (streaming) http << "Accept: text/event-stream\r\n";
    http << "Connection: keep-alive\r\n";
    http << "\r\n";
    http << body;

    std::string raw = http.str();
    int total = static_cast<int>(raw.size());
    int sent = 0;
    while (sent < total) {
        int n = SSL_write(ssl_, raw.data() + sent, total - sent);
        if (n <= 0) {
            close_connection();
            throw std::runtime_error("SSL write failed");
        }
        sent += n;
    }

    return {};  // response read separately
}

// ─── HTTP response helpers ────────────────────────────────────────────────────

// Parse HTTP status code from the first response line ("HTTP/1.1 200 OK\r\n...").
static int parse_http_status(const std::string& headers) {
    auto sp = headers.find(' ');
    if (sp == std::string::npos) return 0;
    return std::atoi(headers.c_str() + sp + 1);
}

// Read the full response body (handles both chunked and content-length).
// Used to capture error bodies that aren't SSE streams.
static std::string read_response_body(SSL* ssl, const std::string& headers) {
    bool chunked = headers.find("Transfer-Encoding: chunked") != std::string::npos;
    int content_length = -1;
    auto cl_pos = headers.find("Content-Length: ");
    if (cl_pos != std::string::npos)
        content_length = std::atoi(headers.c_str() + cl_pos + 16);

    std::string body;
    char buf[4096];

    if (chunked) {
        while (true) {
            std::string size_line;
            while (true) {
                int n = SSL_read(ssl, buf, 1);
                if (n <= 0) goto body_done;
                if (buf[0] == '\n') break;
                if (buf[0] != '\r') size_line += buf[0];
            }
            int chunk_size = static_cast<int>(std::strtol(size_line.c_str(), nullptr, 16));
            if (chunk_size == 0) break;
            int rd = 0;
            while (rd < chunk_size) {
                int n = SSL_read(ssl, buf, std::min(chunk_size - rd, (int)sizeof(buf)));
                if (n <= 0) goto body_done;
                body.append(buf, n);
                rd += n;
            }
            SSL_read(ssl, buf, 2);  // trailing \r\n
        }
    } else if (content_length > 0) {
        int rd = 0;
        while (rd < content_length) {
            int n = SSL_read(ssl, buf, std::min(content_length - rd, (int)sizeof(buf)));
            if (n <= 0) break;
            body.append(buf, n);
            rd += n;
        }
    }
body_done:
    return body;
}

std::string ApiClient::read_response(bool /*streaming*/, StreamCallback /*cb*/) {
    std::string headers;
    char buf[1];
    while (true) {
        int n = SSL_read(ssl_, buf, 1);
        if (n <= 0) return {};
        headers += buf[0];
        if (headers.size() >= 4 &&
            headers.substr(headers.size() - 4) == "\r\n\r\n") break;
    }

    // Return status + body as a combined string so complete() can detect errors.
    // Format: first line is "HTTP_STATUS <code>\n", rest is the response body.
    int status = parse_http_status(headers);
    std::string body = read_response_body(ssl_, headers);
    if (status != 200) {
        // Wrap non-200 so parse_response sees an API error JSON.
        // If body is already JSON with an error field, return it directly.
        if (body.find("\"error\"") != std::string::npos) return body;
        return "{\"error\":{\"type\":\"http_error\",\"message\":\"HTTP "
               + std::to_string(status) + "\"}}";
    }
    return body;
}

ApiResponse ApiClient::parse_response(const std::string& body) {
    ApiResponse resp;
    resp.raw_body = body;

    try {
        auto root = json_parse(body);

        // Check for error
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok         = false;
            resp.error_type = err->get_string("type");
            resp.error      = err->get_string("message", "Unknown API error");
            return resp;
        }

        // Extract content
        auto content = root->get("content");
        if (content && content->is_array()) {
            for (auto& block : content->as_array()) {
                if (block && block->get_string("type") == "text") {
                    resp.content += block->get_string("text");
                }
            }
        }

        resp.stop_reason = root->get_string("stop_reason");

        // Token usage — including prompt cache fields
        auto usage = root->get("usage");
        if (usage && usage->is_object()) {
            resp.input_tokens          = usage->get_int("input_tokens");
            resp.output_tokens         = usage->get_int("output_tokens");
            resp.cache_read_tokens     = usage->get_int("cache_read_input_tokens");
            resp.cache_creation_tokens = usage->get_int("cache_creation_input_tokens");
        }

        resp.ok = true;
    } catch (const std::exception& e) {
        resp.ok    = false;
        resp.error = std::string("Parse error: ") + e.what();
    }

    return resp;
}

// Retryable error types — rate limit and transient overload.
static bool is_retryable(const std::string& error_type) {
    return error_type == "rate_limit_error" || error_type == "overloaded_error";
}

ApiResponse ApiClient::complete(const ApiRequest& req) {
    static const int kMaxAttempts = 4;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            // Exponential backoff outside the mutex: 1s, 2s, 4s
            usleep((1 << (attempt - 1)) * 1000000);
        }

        ApiResponse resp;
        bool threw = false;

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            try {
                if (!ensure_connection()) {
                    ApiResponse r;
                    r.ok    = false;
                    r.error = last_conn_error_.empty() ? "Connection failed" : last_conn_error_;
                    return r;
                }
                std::string body = build_request_body(req, false);
                send_request(body, false, !req.advisor_model.empty());
                std::string raw = read_response(false, nullptr);
                resp = parse_response(raw);
            } catch (...) {
                close_connection();
                threw = true;
            }
        }

        if (threw) {
            if (attempt >= kMaxAttempts - 1) {
                ApiResponse r;
                r.ok    = false;
                r.error = "Request failed after retries";
                return r;
            }
            continue;
        }

        if (resp.ok) {
            total_in_  += resp.input_tokens;
            total_out_ += resp.output_tokens;
            return resp;
        }

        // Non-retryable error — return immediately
        if (!is_retryable(resp.error_type) || attempt >= kMaxAttempts - 1) {
            return resp;
        }
        // Otherwise loop with backoff
    }

    ApiResponse r;
    r.ok    = false;
    r.error = "Unreachable";
    return r;
}

// ─── SSE helpers ─────────────────────────────────────────────────────────────

static void process_sse_event(const std::string& data,
                              std::string& content,
                              ApiResponse& resp,
                              StreamCallback cb) {
    if (data.empty() || data == "[DONE]") return;
    try {
        auto root = json_parse(data);
        std::string type = root->get_string("type");

        if (type == "content_block_delta") {
            auto delta = root->get("delta");
            if (delta && delta->get_string("type") == "text_delta") {
                std::string text = delta->get_string("text");
                content += text;
                if (cb) cb(text);
            }
        } else if (type == "message_start") {
            auto msg = root->get("message");
            if (msg) {
                auto usage = msg->get("usage");
                if (usage) {
                    resp.input_tokens          = usage->get_int("input_tokens");
                    resp.cache_creation_tokens = usage->get_int("cache_creation_input_tokens");
                    resp.cache_read_tokens     = usage->get_int("cache_read_input_tokens");
                }
            }
        } else if (type == "message_delta") {
            auto delta = root->get("delta");
            if (delta) resp.stop_reason = delta->get_string("stop_reason");
            auto usage = root->get("usage");
            if (usage) resp.output_tokens = usage->get_int("output_tokens");
        } else if (type == "error") {
            resp.ok = false;
            auto err = root->get("error");
            if (err) resp.error = err->get_string("message", "Stream error");
        }
    } catch (...) {
        // Ignore malformed SSE events
    }
}

ApiResponse ApiClient::read_streaming_response(StreamCallback cb) {
    ApiResponse resp;
    resp.ok = true;
    std::string content;

    // Read HTTP headers byte-by-byte
    std::string headers;
    {
        char ch;
        while (true) {
            int n = SSL_read(ssl_, &ch, 1);
            if (n <= 0) { resp.ok = false; resp.error = "Connection closed reading headers"; return resp; }
            headers += ch;
            if (headers.size() >= 4 &&
                headers.substr(headers.size() - 4) == "\r\n\r\n") break;
        }
    }

    // Non-200 responses are JSON error bodies, not SSE streams — parse them directly.
    int http_status = parse_http_status(headers);
    if (http_status != 200) {
        std::string body = read_response_body(ssl_, headers);
        close_connection();
        return parse_response(body.empty()
            ? "{\"error\":{\"type\":\"http_error\",\"message\":\"HTTP " + std::to_string(http_status) + "\"}}"
            : body);
    }

    bool chunked = headers.find("Transfer-Encoding: chunked") != std::string::npos;

    // Line buffer for SSE event reassembly across chunk boundaries
    std::string line_buf;
    char buf[4096];

    auto process_line = [&](const std::string& line) {
        if (line.empty() || line == "\r") return;
        if (line.size() > 6 && line.substr(0, 6) == "data: ") {
            std::string data = line.substr(6);
            if (!data.empty() && data.back() == '\r') data.pop_back();
            process_sse_event(data, content, resp, cb);
        }
    };

    auto feed = [&](const char* data, int n) {
        for (int i = 0; i < n; ++i) {
            if (data[i] == '\n') {
                process_line(line_buf);
                line_buf.clear();
            } else {
                line_buf += data[i];
            }
        }
    };

    if (chunked) {
        while (!cancelled_.load()) {
            // Read chunk size line
            std::string size_line;
            while (true) {
                int n = SSL_read(ssl_, buf, 1);
                if (n <= 0 || cancelled_.load()) goto stream_done;
                if (buf[0] == '\n') break;
                if (buf[0] != '\r') size_line += buf[0];
            }
            int chunk_size = static_cast<int>(std::strtol(size_line.c_str(), nullptr, 16));
            if (chunk_size == 0) break;

            int read_so_far = 0;
            while (read_so_far < chunk_size) {
                if (cancelled_.load()) goto stream_done;
                int to_read = std::min(chunk_size - read_so_far, (int)sizeof(buf));
                int n = SSL_read(ssl_, buf, to_read);
                if (n <= 0) goto stream_done;
                feed(buf, n);
                read_so_far += n;
            }
            SSL_read(ssl_, buf, 2);  // trailing \r\n
        }
        if (!cancelled_.load()) SSL_read(ssl_, buf, 2);  // final \r\n
    } else {
        int content_length = -1;
        auto cl_pos = headers.find("Content-Length: ");
        if (cl_pos != std::string::npos)
            content_length = std::atoi(headers.c_str() + cl_pos + 16);
        int read_so_far = 0;
        while ((content_length < 0 || read_so_far < content_length) && !cancelled_.load()) {
            int n = SSL_read(ssl_, buf, sizeof(buf));
            if (n <= 0) break;
            feed(buf, n);
            read_so_far += n;
        }
    }

stream_done:
    if (!line_buf.empty()) process_line(line_buf);
    resp.content = content;
    return resp;
}

ApiResponse ApiClient::stream(const ApiRequest& req, StreamCallback cb) {
    cancelled_.store(false);  // clear any prior interrupt before starting
    static const int kMaxAttempts = 3;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            // Brief backoff between retries (only safe if no content was sent yet).
            usleep((1 << (attempt - 1)) * 1000000);
        }

        ApiResponse resp;
        bool threw = false;

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            try {
                if (!ensure_connection()) {
                    resp.ok    = false;
                    resp.error = last_conn_error_.empty() ? "Connection failed" : last_conn_error_;
                } else {
                    std::string body = build_request_body(req, true);
                    send_request(body, true, !req.advisor_model.empty());
                    resp = read_streaming_response(cb);
                }
            } catch (const std::exception& e) {
                close_connection();
                threw = true;
                resp.ok    = false;
                resp.error = std::string("Stream error: ") + e.what();
            }
        }

        if (resp.ok) {
            total_in_  += resp.input_tokens;
            total_out_ += resp.output_tokens;
            return resp;
        }

        // Only retry if no content reached the caller (avoids duplicate output).
        bool can_retry = resp.content.empty() &&
                         (threw || is_retryable(resp.error_type));
        if (!can_retry || attempt >= kMaxAttempts - 1) {
            return resp;
        }

        close_connection();  // force fresh connection on retry
    }

    ApiResponse r;
    r.ok    = false;
    r.error = "Stream failed after retries";
    return r;
}

} // namespace index_ai
