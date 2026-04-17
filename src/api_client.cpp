// index_ai/src/api_client.cpp — Multi-provider LLM client.
// See api_client.h for the routing model.
#include "api_client.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>

namespace index_ai {

// ─── Provider registry ────────────────────────────────────────────────────────
//
// First entry whose `prefix` matches the start of the model string wins.  An
// empty prefix is the catch-all (bare model names like "claude-sonnet-4-…").
// To add a provider: append a row here, teach build_body_*/parse_body_* if
// the format differs, done.  Ollama is reached via OLLAMA_HOST (defaults to
// http://localhost:11434); other providers wanting a configurable host can
// follow the same pattern.

namespace {

struct ParsedHost { std::string scheme, host; int port; };

// Parse a URL-ish string into scheme/host/port.  Accepts:
//   "http://host:port", "https://host:port", "host:port", "host".
static ParsedHost parse_host(const std::string& s,
                              const std::string& default_scheme,
                              int default_port) {
    ParsedHost r{default_scheme, "", default_port};
    std::string rest = s;
    auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        r.scheme = rest.substr(0, scheme_end);
        rest = rest.substr(scheme_end + 3);
    }
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        r.host = rest.substr(0, colon);
        const char* port_start = rest.c_str() + colon + 1;
        char* end = nullptr;
        long parsed = std::strtol(port_start, &end, 10);
        if (end == port_start || parsed < 1 || parsed > 65535)
            r.port = default_port;
        else
            r.port = static_cast<int>(parsed);
    } else {
        r.host = rest;
    }
    if (r.host.empty()) r.host = "localhost";
    return r;
}

static Provider make_anthropic_provider() {
    Provider p;
    p.name = "anthropic";
    p.prefix = "";                      // catch-all; also matches "claude-…"
    p.host = "api.anthropic.com";
    p.port = 443;
    p.path = "/v1/messages";
    p.tls = true;
    p.uses_api_key = true;
    p.format = Provider::FORMAT_ANTHROPIC;
    return p;
}

static Provider make_ollama_provider() {
    Provider p;
    p.name = "ollama";
    p.prefix = "ollama/";
    const char* env = std::getenv("OLLAMA_HOST");
    auto ph = parse_host(env ? env : "http://localhost:11434", "http", 11434);
    p.host = ph.host;
    p.port = ph.port;
    p.tls  = (ph.scheme == "https");
    p.path = "/v1/chat/completions";    // OpenAI-compatible surface
    p.uses_api_key = false;
    p.format = Provider::FORMAT_OPENAI_CHAT;
    return p;
}

// NOTE: order matters — longest/most-specific prefix first, fallback last.
static const std::vector<Provider>& registry() {
    static const std::vector<Provider> kProviders = {
        make_ollama_provider(),
        make_anthropic_provider(),
    };
    return kProviders;
}

} // namespace

const Provider& provider_for(const std::string& model) {
    const auto& reg = registry();
    for (const auto& p : reg) {
        if (!p.prefix.empty() &&
            model.size() >= p.prefix.size() &&
            model.compare(0, p.prefix.size(), p.prefix) == 0) {
            return p;
        }
    }
    // Fallback: first provider with an empty prefix.
    for (const auto& p : reg) if (p.prefix.empty()) return p;
    return reg.front();
}

bool is_priced(const std::string& model) {
    return provider_for(model).name == "anthropic";
}

bool is_weak_executor(const std::string& model) {
    return provider_for(model).name != "anthropic";
}

std::string strip_model_prefix(const std::string& model) {
    const auto& p = provider_for(model);
    if (!p.prefix.empty() && model.compare(0, p.prefix.size(), p.prefix) == 0)
        return model.substr(p.prefix.size());
    return model;
}

// ─── ApiClient lifecycle ─────────────────────────────────────────────────────

ApiClient::ApiClient(const std::string& api_key) : api_key_(api_key) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) throw std::runtime_error("Failed to create SSL context");

    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ssl_ctx_);
}

ApiClient::~ApiClient() {
    if (!api_key_.empty()) {
        OPENSSL_cleanse(api_key_.data(), api_key_.size());
        api_key_.clear();
    }
    for (auto& [_, c] : conns_) close_connection(c);
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

// ─── Connection management ───────────────────────────────────────────────────

namespace {
// RAII wrapper for socket file descriptors — prevents leaks on exception paths.
struct unique_fd {
    int fd = -1;
    unique_fd() = default;
    explicit unique_fd(int f) : fd(f) {}
    ~unique_fd() { if (fd >= 0) ::close(fd); }
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    unique_fd(unique_fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    unique_fd& operator=(unique_fd&& o) noexcept {
        if (fd >= 0) ::close(fd);
        fd = o.fd; o.fd = -1; return *this;
    }
    int release() noexcept { int f = fd; fd = -1; return f; }
};
} // namespace

bool ApiClient::ensure_connection(const Provider& p, Conn& c) {
    if (c.connected) return true;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(p.port);
    int gai = getaddrinfo(p.host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0) {
        c.last_error = "DNS lookup failed (" + p.host + "): " + gai_strerror(gai);
        return false;
    }

    // Walk the addrinfo list and try each entry until one connects.  macOS
    // (and most Linuxes with IPv6 enabled) return AAAA records first, so
    // localhost resolves to ::1 ahead of 127.0.0.1 — but services like
    // Ollama bind IPv4-only by default.  If we only ever tried the first
    // entry we'd get ECONNREFUSED against the IPv6 address and never
    // discover that 127.0.0.1 works.  curl walks the list; we do too.
    std::string last_connect_err;
    unique_fd guard;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        guard = unique_fd(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (guard.fd < 0) {
            last_connect_err = std::string("socket() failed: ") + strerror(errno);
            continue;
        }
        int flag = 1;
        setsockopt(guard.fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect(guard.fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;                                     // success
        }
        last_connect_err = std::string("connect() failed (") + p.host + ":" +
                           port_str + "): " + strerror(errno);
        guard = unique_fd{};  // close and reset
    }
    freeaddrinfo(res);

    if (guard.fd < 0) {
        c.last_error = last_connect_err.empty()
            ? "connect() failed: no usable address returned by getaddrinfo"
            : last_connect_err;
        return false;
    }
    c.sock = guard.release();  // transfer ownership to Conn

    c.tls = p.tls;
    if (p.tls) {
        c.ssl = SSL_new(ssl_ctx_);
        SSL_set_fd(c.ssl, c.sock);
        SSL_set_tlsext_host_name(c.ssl, p.host.c_str());

        if (SSL_connect(c.ssl) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            ERR_error_string_n(err, errbuf, sizeof(errbuf));
            c.last_error = std::string("TLS handshake failed: ") + errbuf;
            SSL_free(c.ssl);
            c.ssl = nullptr;
            close(c.sock);
            c.sock = -1;
            return false;
        }
    }

    c.connected = true;
    c.last_error.clear();
    return true;
}

void ApiClient::close_connection(Conn& c) {
    if (c.ssl) {
        SSL_shutdown(c.ssl);
        SSL_free(c.ssl);
        c.ssl = nullptr;
    }
    if (c.sock >= 0) {
        close(c.sock);
        c.sock = -1;
    }
    c.connected = false;
}

void ApiClient::cancel() {
    cancelled_.store(true);
    std::lock_guard<std::mutex> lk(conn_mutex_);
    for (auto& [_, c] : conns_) {
        if (c.sock >= 0) ::shutdown(c.sock, SHUT_RDWR);
    }
}

// ─── I/O helpers (format-agnostic) ───────────────────────────────────────────

// Internal helpers — non-namespace-anon so they can friend into ApiClient::Conn.
static int conn_send(index_ai::ApiClient::Conn& c, const char* data, int n) {
    if (c.ssl) return SSL_write(c.ssl, data, n);
    return (int)::send(c.sock, data, n, 0);
}

static int conn_recv(index_ai::ApiClient::Conn& c, char* data, int n) {
    if (c.ssl) return SSL_read(c.ssl, data, n);
    return (int)::recv(c.sock, data, n, 0);
}

// ─── Request body builders ───────────────────────────────────────────────────

std::string ApiClient::build_body_anthropic(const ApiRequest& req, bool streaming) {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["model"] = jstr(req.model);
    m["max_tokens"] = jnum(static_cast<double>(req.max_tokens));
    if (req.include_temperature) m["temperature"] = jnum(req.temperature);

    if (!req.system_prompt.empty()) {
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

    // Anthropic prompt caching: we put a cache breakpoint on the system block
    // (above) and one on the LAST message of the history.  This makes the
    // entire prompt through that final message a cacheable prefix — the next
    // turn appends new messages and hits the cache for everything before its
    // own tail, paying only for the delta tokens.
    //
    // cache_control requires block-form content (the string form doesn't
    // accept the key), so only the tail message is restructured.  All other
    // messages stay as plain strings to keep the request body small.
    auto msgs = jarr();
    const size_t count = req.messages.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& msg = req.messages[i];
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);
        if (i + 1 == count) {
            auto cc = jobj();
            cc->as_object_mut()["type"] = jstr("ephemeral");

            auto block = jobj();
            auto& bm = block->as_object_mut();
            bm["type"]          = jstr("text");
            bm["text"]          = jstr(msg.content);
            bm["cache_control"] = cc;

            auto arr = jarr();
            arr->as_array_mut().push_back(block);
            mo->as_object_mut()["content"] = arr;
        } else {
            mo->as_object_mut()["content"] = jstr(msg.content);
        }
        msgs->as_array_mut().push_back(mo);
    }
    m["messages"] = msgs;

    if (streaming) m["stream"] = jbool(true);

    return json_serialize(*obj);
}

std::string ApiClient::build_body_openai(const ApiRequest& req, bool streaming) {
    // OpenAI chat completions: single flat messages array with optional
    // "system" role at index 0.  No prompt-caching metadata.  Model name
    // has any provider prefix stripped.
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["model"] = jstr(strip_model_prefix(req.model));
    m["max_tokens"] = jnum(static_cast<double>(req.max_tokens));
    if (req.include_temperature) m["temperature"] = jnum(req.temperature);
    if (streaming) m["stream"] = jbool(true);

    auto msgs = jarr();
    if (!req.system_prompt.empty()) {
        auto sm = jobj();
        sm->as_object_mut()["role"]    = jstr("system");
        sm->as_object_mut()["content"] = jstr(req.system_prompt);
        msgs->as_array_mut().push_back(sm);
    }
    for (auto& msg : req.messages) {
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);
        mo->as_object_mut()["content"] = jstr(msg.content);
        msgs->as_array_mut().push_back(mo);
    }
    m["messages"] = msgs;

    return json_serialize(*obj);
}

// ─── Outgoing HTTP request ───────────────────────────────────────────────────

void ApiClient::send_request(const Provider& p, Conn& c,
                              const std::string& body, bool streaming) {
    std::ostringstream http;
    http << "POST " << p.path << " HTTP/1.1\r\n";
    http << "Host: " << p.host;
    // Ollama + typical non-443 local servers want the port in the Host header
    // even for http; harmless for Anthropic (servers ignore a :443 there).
    if (!((p.tls && p.port == 443) || (!p.tls && p.port == 80))) {
        http << ":" << p.port;
    }
    http << "\r\n";
    http << "Content-Type: application/json\r\n";
    if (p.uses_api_key && p.format == Provider::FORMAT_ANTHROPIC) {
        http << "x-api-key: " << api_key_ << "\r\n";
        http << "anthropic-version: 2023-06-01\r\n";
        http << "anthropic-beta: prompt-caching-2024-07-31\r\n";
    } else if (p.uses_api_key) {
        // Generic bearer-auth path — useful when we later add openai-proper.
        http << "Authorization: Bearer " << api_key_ << "\r\n";
    }
    http << "Content-Length: " << body.size() << "\r\n";
    if (streaming) http << "Accept: text/event-stream\r\n";
    http << "Connection: keep-alive\r\n";
    http << "\r\n";
    http << body;

    std::string raw = http.str();
    int total = static_cast<int>(raw.size());
    int sent = 0;
    while (sent < total) {
        int n = conn_send(c, raw.data() + sent, total - sent);
        if (n <= 0) {
            close_connection(c);
            throw std::runtime_error("write failed");
        }
        sent += n;
    }
}

// ─── Incoming HTTP response helpers ──────────────────────────────────────────

static int parse_http_status(const std::string& headers) {
    auto sp = headers.find(' ');
    if (sp == std::string::npos) return 0;
    return std::atoi(headers.c_str() + sp + 1);
}

// Read HTTP headers in buffered chunks instead of one byte at a time.  The
// sentinel "\r\n\r\n" can straddle a read boundary, so each scan revisits the
// last 3 bytes of the prior buffer.  Bytes past the sentinel belong to the
// response body and are returned via `leftover` for the body reader to
// consume before touching the socket again.
static bool read_http_headers(index_ai::ApiClient::Conn& c,
                              std::string& headers,
                              std::string& leftover) {
    static constexpr size_t kMaxHeaderSize = 65536;
    headers.clear();
    headers.reserve(2048);
    leftover.clear();
    char buf[4096];
    while (true) {
        int n = conn_recv(c, buf, sizeof(buf));
        if (n <= 0) return false;
        size_t old_len = headers.size();
        headers.append(buf, n);
        if (headers.size() > kMaxHeaderSize) return false;
        size_t scan_from = old_len >= 3 ? old_len - 3 : 0;
        for (size_t i = scan_from; i + 4 <= headers.size(); ++i) {
            if (headers[i]     == '\r' && headers[i + 1] == '\n' &&
                headers[i + 2] == '\r' && headers[i + 3] == '\n') {
                size_t end = i + 4;
                leftover.assign(headers, end, headers.size() - end);
                headers.resize(end);
                return true;
            }
        }
    }
}

// Drain `prefix` before reading from the socket.  Lets the body reader
// consume header-read leftovers transparently — we use a small helper
// closure inside read_response_body / read_streaming_response to keep the
// consume-then-recv pattern tidy.
namespace { struct PrefixCursor { const std::string& s; size_t pos = 0; }; }

static std::string read_response_body(index_ai::ApiClient::Conn& c,
                                       const std::string& headers,
                                       const std::string& prefix) {
    bool chunked = headers.find("Transfer-Encoding: chunked") != std::string::npos;
    int content_length = -1;
    auto cl_pos = headers.find("Content-Length: ");
    if (cl_pos != std::string::npos)
        content_length = std::atoi(headers.c_str() + cl_pos + 16);

    PrefixCursor pc{prefix, 0};
    auto recv_some = [&](char* dst, int want) -> int {
        if (pc.pos < pc.s.size()) {
            int avail = static_cast<int>(pc.s.size() - pc.pos);
            int take = std::min(want, avail);
            std::memcpy(dst, pc.s.data() + pc.pos, take);
            pc.pos += take;
            return take;
        }
        return conn_recv(c, dst, want);
    };

    std::string body;
    char buf[4096];

    if (chunked) {
        while (true) {
            std::string size_line;
            while (true) {
                int n = recv_some(buf, 1);
                if (n <= 0) goto body_done;
                if (buf[0] == '\n') break;
                if (buf[0] != '\r') size_line += buf[0];
            }
            int chunk_size = static_cast<int>(std::strtol(size_line.c_str(), nullptr, 16));
            if (chunk_size == 0) break;
            int rd = 0;
            while (rd < chunk_size) {
                int n = recv_some(buf, std::min(chunk_size - rd, (int)sizeof(buf)));
                if (n <= 0) goto body_done;
                body.append(buf, n);
                rd += n;
            }
            recv_some(buf, 2);  // trailing \r\n
        }
    } else if (content_length > 0) {
        int rd = 0;
        while (rd < content_length) {
            int n = recv_some(buf, std::min(content_length - rd, (int)sizeof(buf)));
            if (n <= 0) break;
            body.append(buf, n);
            rd += n;
        }
    }
body_done:
    return body;
}

std::string ApiClient::read_response(Conn& c) {
    std::string headers, leftover;
    if (!read_http_headers(c, headers, leftover)) return {};
    int status = parse_http_status(headers);
    std::string body = read_response_body(c, headers, leftover);
    if (status != 200) {
        if (body.find("\"error\"") != std::string::npos) return body;
        return "{\"error\":{\"type\":\"http_error\",\"message\":\"HTTP "
               + std::to_string(status) + "\"}}";
    }
    return body;
}

// ─── Response parsers ────────────────────────────────────────────────────────

ApiResponse ApiClient::parse_body_anthropic(const std::string& body) {
    ApiResponse resp;
    resp.raw_body = body;
    try {
        auto root = json_parse(body);
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok         = false;
            resp.error_type = err->get_string("type");
            resp.error      = err->get_string("message", "Unknown API error");
            return resp;
        }
        auto content = root->get("content");
        if (content && content->is_array()) {
            for (auto& block : content->as_array()) {
                if (block && block->get_string("type") == "text") {
                    resp.content += block->get_string("text");
                }
            }
        }
        resp.stop_reason = root->get_string("stop_reason");
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

ApiResponse ApiClient::parse_body_openai(const std::string& body) {
    ApiResponse resp;
    resp.raw_body = body;
    try {
        auto root = json_parse(body);
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok         = false;
            resp.error_type = err->get_string("type");
            resp.error      = err->get_string("message", "Unknown API error");
            return resp;
        }
        // choices[0].message.content — single assistant turn, we ignore n>1.
        auto choices = root->get("choices");
        if (choices && choices->is_array() && !choices->as_array().empty()) {
            auto& ch0 = choices->as_array().front();
            if (ch0) {
                auto msg = ch0->get("message");
                if (msg) resp.content = msg->get_string("content");
                resp.stop_reason = ch0->get_string("finish_reason");
            }
        }
        // Usage is optional on openai-compat servers.  Ollama reports it.
        auto usage = root->get("usage");
        if (usage && usage->is_object()) {
            resp.input_tokens  = usage->get_int("prompt_tokens");
            resp.output_tokens = usage->get_int("completion_tokens");
        }
        resp.ok = true;
    } catch (const std::exception& e) {
        resp.ok    = false;
        resp.error = std::string("Parse error: ") + e.what();
    }
    return resp;
}

// ─── Retry policy ────────────────────────────────────────────────────────────

static bool is_retryable(const std::string& error_type) {
    return error_type == "rate_limit_error" || error_type == "overloaded_error";
}

// ─── Blocking complete() ─────────────────────────────────────────────────────

ApiResponse ApiClient::complete(const ApiRequest& req) {
    static const int kMaxAttempts = 4;
    const Provider& prov = provider_for(req.model);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) usleep((1 << (attempt - 1)) * 1000000);

        ApiResponse resp;
        bool threw = false;

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            Conn& c = conns_[prov.name];
            try {
                if (!ensure_connection(prov, c)) {
                    ApiResponse r;
                    r.ok    = false;
                    r.error = c.last_error.empty() ? "Connection failed" : c.last_error;
                    return r;
                }
                std::string body = (prov.format == Provider::FORMAT_ANTHROPIC)
                                   ? build_body_anthropic(req, false)
                                   : build_body_openai(req, false);
                send_request(prov, c, body, false);
                std::string raw = read_response(c);
                resp = (prov.format == Provider::FORMAT_ANTHROPIC)
                       ? parse_body_anthropic(raw)
                       : parse_body_openai(raw);
            } catch (...) {
                close_connection(c);
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

        if (!is_retryable(resp.error_type) || attempt >= kMaxAttempts - 1) return resp;
    }

    ApiResponse r;
    r.ok    = false;
    r.error = "Unreachable";
    return r;
}

// ─── Streaming ───────────────────────────────────────────────────────────────

// Anthropic event-stream: `event: <type>\ndata: <json>\n\n`.  We key off the
// JSON's "type" field rather than the event: line — same information, and
// message_start / message_delta carry token usage.
static void process_anthropic_event(const std::string& data,
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
        // Malformed SSE payloads are skipped silently.
    }
}

// OpenAI-compat chunk: `data: {..., "choices":[{"delta":{"content":"…"}}]}`
// and a terminating `data: [DONE]`.  Token usage shows up in the final chunk
// when `stream_options.include_usage` is set — Ollama emits it by default.
static void process_openai_event(const std::string& data,
                                  std::string& content,
                                  ApiResponse& resp,
                                  StreamCallback cb) {
    if (data.empty() || data == "[DONE]") return;
    try {
        auto root = json_parse(data);
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok         = false;
            resp.error_type = err->get_string("type");
            resp.error      = err->get_string("message", "Stream error");
            return;
        }
        auto choices = root->get("choices");
        if (choices && choices->is_array() && !choices->as_array().empty()) {
            auto& ch0 = choices->as_array().front();
            if (ch0) {
                auto delta = ch0->get("delta");
                if (delta) {
                    std::string text = delta->get_string("content");
                    if (!text.empty()) {
                        content += text;
                        if (cb) cb(text);
                    }
                }
                std::string finish = ch0->get_string("finish_reason");
                if (!finish.empty()) resp.stop_reason = finish;
            }
        }
        auto usage = root->get("usage");
        if (usage && usage->is_object()) {
            resp.input_tokens  = usage->get_int("prompt_tokens");
            resp.output_tokens = usage->get_int("completion_tokens");
        }
    } catch (...) {
        // Ignore malformed chunk.
    }
}

ApiResponse ApiClient::read_streaming_response(Conn& c, StreamCallback cb,
                                                 Provider::Format fmt) {
    ApiResponse resp;
    resp.ok = true;
    std::string content;

    std::string headers, leftover;
    if (!read_http_headers(c, headers, leftover)) {
        resp.ok = false;
        resp.error = "Connection closed reading headers";
        return resp;
    }

    int http_status = parse_http_status(headers);
    if (http_status != 200) {
        std::string body = read_response_body(c, headers, leftover);
        close_connection(c);
        if (body.empty())
            body = "{\"error\":{\"type\":\"http_error\",\"message\":\"HTTP "
                   + std::to_string(http_status) + "\"}}";
        return (fmt == Provider::FORMAT_ANTHROPIC)
               ? parse_body_anthropic(body)
               : parse_body_openai(body);
    }

    bool chunked = headers.find("Transfer-Encoding: chunked") != std::string::npos;

    std::string line_buf;
    line_buf.reserve(1024);
    char buf[4096];

    // Drain `leftover` before reading from the socket — same pattern as
    // read_response_body, so body bytes caught during the buffered header
    // read don't get dropped.
    PrefixCursor pc{leftover, 0};
    auto recv_some = [&](char* dst, int want) -> int {
        if (pc.pos < pc.s.size()) {
            int avail = static_cast<int>(pc.s.size() - pc.pos);
            int take = std::min(want, avail);
            std::memcpy(dst, pc.s.data() + pc.pos, take);
            pc.pos += take;
            return take;
        }
        return conn_recv(c, dst, want);
    };

    auto process_line = [&](const std::string& line) {
        if (line.empty() || line == "\r") return;
        if (line.size() > 6 && line.compare(0, 6, "data: ") == 0) {
            std::string data = line.substr(6);
            if (!data.empty() && data.back() == '\r') data.pop_back();
            if (fmt == Provider::FORMAT_ANTHROPIC)
                process_anthropic_event(data, content, resp, cb);
            else
                process_openai_event(data, content, resp, cb);
        }
    };

    auto feed = [&](const char* data, int n) {
        for (int i = 0; i < n; ++i) {
            if (data[i] == '\n') {
                process_line(line_buf);
                line_buf.clear();
            } else {
                static constexpr size_t kMaxLineSize = 1048576;
                if (line_buf.size() < kMaxLineSize)
                    line_buf += data[i];
            }
        }
    };

    if (chunked) {
        while (!cancelled_.load()) {
            std::string size_line;
            while (true) {
                int n = recv_some(buf, 1);
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
                int n = recv_some(buf, to_read);
                if (n <= 0) goto stream_done;
                feed(buf, n);
                read_so_far += n;
            }
            recv_some(buf, 2);  // trailing \r\n
        }
        if (!cancelled_.load()) recv_some(buf, 2);
    } else {
        int content_length = -1;
        auto cl_pos = headers.find("Content-Length: ");
        if (cl_pos != std::string::npos)
            content_length = std::atoi(headers.c_str() + cl_pos + 16);
        int read_so_far = 0;
        while ((content_length < 0 || read_so_far < content_length) && !cancelled_.load()) {
            int n = recv_some(buf, sizeof(buf));
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
    cancelled_.store(false);
    static const int kMaxAttempts = 3;
    const Provider& prov = provider_for(req.model);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) usleep((1 << (attempt - 1)) * 1000000);

        ApiResponse resp;
        bool threw = false;

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            Conn& c = conns_[prov.name];
            try {
                if (!ensure_connection(prov, c)) {
                    resp.ok    = false;
                    resp.error = c.last_error.empty() ? "Connection failed" : c.last_error;
                } else {
                    std::string body = (prov.format == Provider::FORMAT_ANTHROPIC)
                                       ? build_body_anthropic(req, true)
                                       : build_body_openai(req, true);
                    send_request(prov, c, body, true);
                    resp = read_streaming_response(c, cb, prov.format);
                }
            } catch (const std::exception& e) {
                close_connection(c);
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

        bool can_retry = resp.content.empty() &&
                         (threw || is_retryable(resp.error_type));
        if (!can_retry || attempt >= kMaxAttempts - 1) return resp;

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            close_connection(conns_[prov.name]);
        }
    }

    ApiResponse r;
    r.ok    = false;
    r.error = "Stream failed after retries";
    return r;
}

} // namespace index_ai
