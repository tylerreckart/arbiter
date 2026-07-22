// arbiter/src/api_client.cpp — Multi-provider LLM client.
// See api_client.h for the routing model.
#include "api_client.h"
#include "circuit_breaker.h"
#include "metrics.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <string_view>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace arbiter {

// ─── Provider registry ────────────────────────────────────────────────────────
// First entry whose `prefix` matches the model string wins; empty prefix = catch-all.
// Ollama stays local via OLLAMA_HOST (default http://localhost:11434); hosted
// models route through OpenRouter's OpenAI-compatible chat endpoint.

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

static Provider make_openrouter_provider(const std::string& prefix = "") {
    Provider p;
    p.name = "openrouter";
    p.prefix = prefix;
    p.host = "openrouter.ai";
    p.port = 443;
    p.path = "/api/v1/chat/completions";
    p.tls = true;
    p.uses_api_key = true;
    p.format = Provider::FORMAT_OPENAI_CHAT;
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
        make_openrouter_provider("openrouter/"),
        make_openrouter_provider(),
    };
    return kProviders;
}

static bool starts_with(const std::string& s, const char* prefix) {
    const std::string_view p(prefix);
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static std::string strip_known_prefix(const std::string& model, const char* prefix) {
    return starts_with(model, prefix) ? model.substr(std::strlen(prefix)) : model;
}

static std::string openrouter_model_id(const std::string& model) {
    std::string m = strip_known_prefix(model, "openrouter/");
    if (starts_with(m, "claude-")) return "anthropic/" + m;
    if (starts_with(m, "gemini/")) return "google/" + m.substr(std::strlen("gemini/"));
    return m;
}

static std::string model_for_provider_request(const Provider& p,
                                              const std::string& model) {
    if (p.name == "openrouter") return openrouter_model_id(model);
    if (!p.prefix.empty() && starts_with(model, p.prefix.c_str())) {
        return model.substr(p.prefix.size());
    }
    return model;
}

static std::string header_value_from_env(const char* env_var,
                                         const char* fallback) {
    const char* v = std::getenv(env_var);
    std::string out = (v && v[0]) ? v : fallback;
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
    return out;
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

bool is_weak_executor(const std::string& model) {
    // Local small models need the tool-vocabulary-first prompt profile;
    // frontier hosted models follow abstract tool
    // instructions reliably.
    return provider_for(model).name == "ollama";
}

std::string strip_model_prefix(const std::string& model) {
    const auto& p = provider_for(model);
    return model_for_provider_request(p, model);
}

// Gemini embeds model + action in the URL; other providers use a static path.
static std::string path_for(const Provider& p, const ApiRequest& req,
                             bool streaming) {
    if (p.format != Provider::FORMAT_GEMINI) return p.path;
    const std::string model = model_for_provider_request(p, req.model);
    return std::string("/v1beta/models/") + model +
           (streaming ? ":streamGenerateContent?alt=sse"
                      : ":generateContent");
}

// ─── ApiClient lifecycle ─────────────────────────────────────────────────────

ApiClient::ApiClient(std::map<std::string, std::string> api_keys) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#endif

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) throw std::runtime_error("Failed to create SSL context");

    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ssl_ctx_);

    // Store each provider's key XOR-masked at rest.  The mask is a
    // same-length random buffer — trivially reversible by anything
    // running in-process, but it keeps the plaintext out of the obvious
    // string-scan paths (core dumps, debugger strings, memory forensics
    // tools that grep for "sk-ant-" / "sk-").  Plaintext is only
    // reconstituted on demand in unmask_api_key(), and call sites wipe
    // their copy immediately after the wire flush.
    for (auto& [name, key] : api_keys) {
        if (key.empty()) continue;
        MaskedKey mk;
        mk.masked.resize(key.size());
        mk.mask.resize(key.size());
        if (RAND_bytes(mk.mask.data(), static_cast<int>(mk.mask.size())) != 1) {
            throw std::runtime_error("CSPRNG failure masking API key");
        }
        for (size_t i = 0; i < key.size(); ++i) {
            mk.masked[i] = static_cast<unsigned char>(key[i]) ^ mk.mask[i];
        }
        // Lock both buffers into RAM so they are never written to swap.
        // mlock() may fail when the process hits RLIMIT_MEMLOCK (common in
        // containers); degrade gracefully — the XOR masking still protects
        // against simple credential-scanner passes.
        if (::mlock(mk.masked.data(), mk.masked.size()) != 0 ||
            ::mlock(mk.mask.data(),   mk.mask.size())   != 0) {
            ::fprintf(stderr,
                "WARN: mlock failed for API key '%s' — key may appear in swap\n",
                name.c_str());
        }
        // Wipe the plaintext from the input map before dropping it.
        OPENSSL_cleanse(key.data(), key.size());
        api_keys_.emplace(name, std::move(mk));
    }
}

ApiClient::~ApiClient() {
    for (auto& [_, mk] : api_keys_) {
        // Unlock before zeroing — munlock after cleanse would be a no-op
        // on the already-zeroed pages, but order matters for correctness.
        if (!mk.masked.empty()) {
            ::munlock(mk.masked.data(), mk.masked.size());
            OPENSSL_cleanse(mk.masked.data(), mk.masked.size());
        }
        if (!mk.mask.empty()) {
            ::munlock(mk.mask.data(), mk.mask.size());
            OPENSSL_cleanse(mk.mask.data(), mk.mask.size());
        }
    }
    api_keys_.clear();
    for (auto& [_, pool] : pools_) {
        for (auto& c : pool->conns) close_connection(*c);
    }
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

// Zeros its buffer on destruction so plaintext key copies don't outlive their scope.
struct SensitiveString {
    std::string value;
    SensitiveString() = default;
    explicit SensitiveString(std::string s) : value(std::move(s)) {}
    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;
    SensitiveString(SensitiveString&&) = default;
    SensitiveString& operator=(SensitiveString&&) = default;
    ~SensitiveString() {
        if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
    }
};

std::string ApiClient::unmask_api_key(const std::string& provider) const {
    auto it = api_keys_.find(provider);
    if (it == api_keys_.end()) return {};
    const auto& mk = it->second;
    std::string out;
    out.resize(mk.masked.size());
    for (size_t i = 0; i < mk.masked.size(); ++i) {
        out[i] = static_cast<char>(mk.masked[i] ^ mk.mask[i]);
    }
    return out;
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

ApiClient::ProviderPool& ApiClient::pool_for(const std::string& provider) {
    std::lock_guard<std::mutex> lk(pool_mutex_);
    auto& slot = pools_[provider];
    if (!slot) slot = std::make_unique<ProviderPool>();
    return *slot;
}

namespace {

// Current thread's request token installed by RequestCancelScope.
thread_local std::shared_ptr<CancelToken> tls_request_token;

}  // namespace

void CancelToken::request_cancel() {
    cancelled_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mu_);
    if (owner_ && conn_) {
        owner_->shutdown_conn(static_cast<ApiClient::Conn*>(conn_));
    }
}

RequestCancelScope::RequestCancelScope(ApiClient& client,
                                       std::shared_ptr<CancelToken> token)
    : client_(client), token_(std::move(token)), prev_(tls_request_token) {
    tls_request_token = token_;
    if (token_) client_.register_token(token_.get());
}

RequestCancelScope::~RequestCancelScope() {
    if (token_) client_.unregister_token(token_.get());
    tls_request_token = std::move(prev_);
}

std::shared_ptr<CancelToken> current_request_cancel_token() {
    return tls_request_token;
}

void ApiClient::register_token(CancelToken* token) {
    if (!token) return;
    std::lock_guard<std::mutex> lk(tokens_mu_);
    active_tokens_.push_back(token);
}

void ApiClient::unregister_token(CancelToken* token) {
    if (!token) return;
    std::lock_guard<std::mutex> lk(tokens_mu_);
    active_tokens_.erase(
        std::remove(active_tokens_.begin(), active_tokens_.end(), token),
        active_tokens_.end());
}

void ApiClient::shutdown_conn(Conn* conn) {
    if (!conn) return;
    if (conn->sock >= 0) ::shutdown(conn->sock, SHUT_RDWR);
}

void ApiClient::bind_token_conn(CancelToken* token, Conn* conn) {
    if (!token || !conn) return;
    std::lock_guard<std::mutex> lk(token->mu_);
    token->owner_ = this;
    token->conn_ = conn;
    if (token->cancelled_.load(std::memory_order_acquire) && conn->sock >= 0) {
        ::shutdown(conn->sock, SHUT_RDWR);
    }
}

void ApiClient::unbind_token_conn(CancelToken* token, Conn* conn) {
    if (!token) return;
    std::lock_guard<std::mutex> lk(token->mu_);
    if (token->conn_ == conn) {
        token->conn_ = nullptr;
        token->owner_ = nullptr;
    }
}

bool ApiClient::is_request_cancelled() const {
    if (cancelled_.load(std::memory_order_acquire)) return true;
    if (tls_request_token && tls_request_token->is_cancelled()) return true;
    return false;
}

// Lease a Conn out of the provider's pool.  Prefer an idle Conn (reuse its
// keep-alive socket); otherwise open a new slot up to the cap; otherwise wait
// for a lease to be returned.  The leased Conn stays owned by the pool (via
// unique_ptr) so cancel() and ~ApiClient can still reach its socket while it's
// in use — the lease only hands out exclusive *use* of it, not ownership.
ApiClient::ConnLease::ConnLease(ApiClient& owner, const std::string& provider)
    : owner_(owner), pool_(owner.pool_for(provider)), conn_(nullptr) {
    std::unique_lock<std::mutex> lk(pool_.mu);
    pool_.cv.wait(lk, [&] {
        return !pool_.idle.empty() ||
               static_cast<int>(pool_.conns.size()) < kMaxConnsPerProvider;
    });
    if (!pool_.idle.empty()) {
        conn_ = pool_.idle.back();
        pool_.idle.pop_back();
    } else {
        pool_.conns.push_back(std::make_unique<Conn>());
        conn_ = pool_.conns.back().get();
    }
    lk.unlock();
    if (tls_request_token) {
        owner_.bind_token_conn(tls_request_token.get(), conn_);
    }
}

ApiClient::ConnLease::~ConnLease() {
    if (tls_request_token) {
        owner_.unbind_token_conn(tls_request_token.get(), conn_);
    }
    {
        std::lock_guard<std::mutex> lk(pool_.mu);
        pool_.idle.push_back(conn_);
    }
    pool_.cv.notify_one();
}

void ApiClient::cancel(CancelToken& token) {
    token.request_cancel();
}

void ApiClient::cancel() {
    cancelled_.store(true);
    // Fan out to every scoped token first so their conn_ pointers are shut
    // down even if we also sweep the pools below.
    {
        std::lock_guard<std::mutex> lk(tokens_mu_);
        for (CancelToken* t : active_tokens_) {
            if (t) t->request_cancel();
        }
    }
    // Shut down every open socket across every provider pool so an in-flight
    // SSL_read / read returns immediately.  Lock order is pool_mutex_ then the
    // per-pool mu; leases only ever take the per-pool mu (pool_for releases
    // pool_mutex_ before the lease locks mu), so this ordering can't deadlock.
    std::lock_guard<std::mutex> lk(pool_mutex_);
    for (auto& [_, pool] : pools_) {
        std::lock_guard<std::mutex> plk(pool->mu);
        for (auto& c : pool->conns) {
            if (c->sock >= 0) ::shutdown(c->sock, SHUT_RDWR);
        }
    }
}

// ─── I/O helpers (format-agnostic) ───────────────────────────────────────────

// Internal helpers — non-namespace-anon so they can friend into ApiClient::Conn.
static int conn_send(arbiter::ApiClient::Conn& c, const char* data, int n) {
    if (c.ssl) return SSL_write(c.ssl, data, n);
    return (int)::send(c.sock, data, n, 0);
}

static int conn_recv(arbiter::ApiClient::Conn& c, char* data, int n) {
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
    // (above) and one on the LAST block of the LAST message.  This makes the
    // entire prompt through that final message a cacheable prefix — the next
    // turn appends new messages and hits the cache for everything before its
    // own tail, paying only for the delta tokens.
    //
    // cache_control requires block-form content (the string form doesn't
    // accept the key), so the tail message is always restructured.  Earlier
    // text-only messages stay as plain strings to keep the request body
    // small; messages with image parts are always restructured into a block
    // array regardless of position.
    auto msgs = jarr();
    const size_t count = req.messages.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& msg = req.messages[i];
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);

        const bool is_tail   = (i + 1 == count);
        const bool has_parts = !msg.parts.empty();

        if (!has_parts && !is_tail) {
            // Fast path: text-only middle message, emitted as a string.
            mo->as_object_mut()["content"] = jstr(msg.content);
        } else {
            // Build a block array.  When parts is empty we synthesise a single
            // text block from `content`.  Cache breakpoint goes on the LAST
            // block of the tail message.
            auto arr = jarr();
            auto emit_text = [&](const std::string& text, bool with_cache) {
                auto block = jobj();
                auto& bm = block->as_object_mut();
                bm["type"] = jstr("text");
                bm["text"] = jstr(text);
                if (with_cache) {
                    auto cc = jobj();
                    cc->as_object_mut()["type"] = jstr("ephemeral");
                    bm["cache_control"] = cc;
                }
                arr->as_array_mut().push_back(block);
            };
            auto emit_image = [&](const ContentPart& part, bool with_cache) {
                auto block = jobj();
                auto& bm = block->as_object_mut();
                bm["type"] = jstr("image");
                auto src = jobj();
                auto& sm = src->as_object_mut();
                if (!part.image_url.empty()) {
                    sm["type"] = jstr("url");
                    sm["url"]  = jstr(part.image_url);
                } else {
                    sm["type"]       = jstr("base64");
                    sm["media_type"] = jstr(part.media_type);
                    sm["data"]       = jstr(part.image_data);
                }
                bm["source"] = src;
                if (with_cache) {
                    auto cc = jobj();
                    cc->as_object_mut()["type"] = jstr("ephemeral");
                    bm["cache_control"] = cc;
                }
                arr->as_array_mut().push_back(block);
            };

            if (!has_parts) {
                emit_text(msg.content, /*with_cache=*/is_tail);
            } else {
                const size_t pcount = msg.parts.size();
                for (size_t pi = 0; pi < pcount; ++pi) {
                    const bool last_block = is_tail && (pi + 1 == pcount);
                    const auto& part = msg.parts[pi];
                    if (part.kind == ContentPart::TEXT) {
                        emit_text(part.text, last_block);
                    } else {
                        emit_image(part, last_block);
                    }
                }
            }
            mo->as_object_mut()["content"] = arr;
        }
        msgs->as_array_mut().push_back(mo);
    }
    m["messages"] = msgs;

    if (streaming) m["stream"] = jbool(true);

    return json_serialize(*obj);
}

std::string ApiClient::build_body_openai(const Provider& prov,
                                          const ApiRequest& req, bool streaming) {
    // OpenAI chat completions shape: single flat messages array with optional
    // "system" role at index 0.  Model name has any provider prefix stripped.
    //
    // Minor format divergences between OpenAI proper and Ollama are handled
    // below.  OpenAI's Chat Completions has moved to `max_completion_tokens`
    // (max_tokens is deprecated for newer models and rejected by the o-series
    // reasoning models).  Ollama keeps the classic `max_tokens`.  Reasoning
    // models also reject non-default `temperature`, so we drop it for them.
    const bool is_openai = (prov.name == "openai");
    const std::string stripped = model_for_provider_request(prov, req.model);

    // Reasoning-model detection: "o<digit>..." (o3, o4-mini, …).
    auto is_reasoning_model = [](const std::string& m) {
        return m.size() >= 2 && m[0] == 'o' && std::isdigit(static_cast<unsigned char>(m[1]));
    };
    const bool reasoning = is_openai && is_reasoning_model(stripped);

    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["model"] = jstr(stripped);
    if (is_openai) m["max_completion_tokens"] = jnum(static_cast<double>(req.max_tokens));
    else           m["max_tokens"]            = jnum(static_cast<double>(req.max_tokens));
    if (req.include_temperature && !reasoning) m["temperature"] = jnum(req.temperature);
    if (streaming) {
        m["stream"] = jbool(true);
        // OpenAI only reports usage on the terminal stream chunk when you opt
        // in.  Ollama emits usage without the flag and older builds reject
        // unknown top-level fields, so keep this OpenAI-only.
        if (is_openai) {
            auto so = jobj();
            so->as_object_mut()["include_usage"] = jbool(true);
            m["stream_options"] = so;
        }
    }

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
        if (msg.parts.empty()) {
            // Fast path: text-only message, emit as a string.
            mo->as_object_mut()["content"] = jstr(msg.content);
        } else {
            // OpenAI Chat Completions multipart shape:
            //   content: [
            //     {type:"text",     text:"..."},
            //     {type:"image_url", image_url:{url:"..."}}
            //   ]
            // Both inline base64 and hosted URLs ride on `image_url.url`;
            // base64 goes as a `data:` URL.  Ollama's OpenAI-compatible
            // /v1/chat/completions accepts the same shape on llama-3.2-vision
            // and friends, so this code path doubles for ollama.
            auto arr = jarr();
            for (auto& part : msg.parts) {
                auto block = jobj();
                auto& bm = block->as_object_mut();
                if (part.kind == ContentPart::TEXT) {
                    bm["type"] = jstr("text");
                    bm["text"] = jstr(part.text);
                } else {
                    bm["type"] = jstr("image_url");
                    auto iu = jobj();
                    if (!part.image_url.empty()) {
                        iu->as_object_mut()["url"] = jstr(part.image_url);
                    } else {
                        std::string data_url =
                            "data:" + part.media_type +
                            ";base64," + part.image_data;
                        iu->as_object_mut()["url"] = jstr(data_url);
                    }
                    bm["image_url"] = iu;
                }
                arr->as_array_mut().push_back(block);
            }
            mo->as_object_mut()["content"] = arr;
        }
        msgs->as_array_mut().push_back(mo);
    }
    m["messages"] = msgs;

    return json_serialize(*obj);
}

std::string ApiClient::build_body_gemini(const ApiRequest& req) {
    // Gemini's generateContent shape:
    //   {
    //     "contents": [{ "role": "user"|"model",
    //                    "parts": [{"text": "..."}] }, ...],
    //     "systemInstruction": { "parts": [{"text": "..."}] },
    //     "generationConfig": { "temperature": ..., "maxOutputTokens": ... }
    //   }
    //
    // Roles diverge from the rest of the codebase: Gemini calls the assistant
    // turn "model".  We translate "assistant" → "model" on the way out and
    // leave "user" as-is.  System prompt is hoisted to a top-level field
    // rather than living as a synthetic message at index 0.
    //
    // The model id itself is *not* in the body — it's in the URL path,
    // built by path_for() and passed straight through send_request.
    //
    // Streaming is requested via the URL action token (`:streamGenerateContent`),
    // not via a `stream:true` body field, so this builder ignores the
    // streaming flag entirely — both code paths use the same body.
    auto obj = jobj();
    auto& m = obj->as_object_mut();

    if (!req.system_prompt.empty()) {
        auto part = jobj();
        part->as_object_mut()["text"] = jstr(req.system_prompt);
        auto parts = jarr();
        parts->as_array_mut().push_back(part);
        auto sys = jobj();
        sys->as_object_mut()["parts"] = parts;
        m["systemInstruction"] = sys;
    }

    auto contents = jarr();
    for (auto& msg : req.messages) {
        auto entry = jobj();
        const std::string role = (msg.role == "assistant") ? "model" : msg.role;
        entry->as_object_mut()["role"] = jstr(role);

        auto parts = jarr();
        if (msg.parts.empty()) {
            // Text-only path — single {text} part.
            auto part = jobj();
            part->as_object_mut()["text"] = jstr(msg.content);
            parts->as_array_mut().push_back(part);
        } else {
            // Gemini parts shape: {text} for prose, {inlineData:{mimeType,
            // data}} for base64 images, {fileData:{mimeType, fileUri}} for
            // hosted-URL images.  fileData accepts http(s) URLs and
            // gs:// URIs; the model fetches the bytes itself.
            for (auto& part : msg.parts) {
                auto p = jobj();
                if (part.kind == ContentPart::TEXT) {
                    p->as_object_mut()["text"] = jstr(part.text);
                } else if (!part.image_url.empty()) {
                    auto fd = jobj();
                    fd->as_object_mut()["mimeType"] = jstr(part.media_type);
                    fd->as_object_mut()["fileUri"]  = jstr(part.image_url);
                    p->as_object_mut()["fileData"] = fd;
                } else {
                    auto inl = jobj();
                    inl->as_object_mut()["mimeType"] = jstr(part.media_type);
                    inl->as_object_mut()["data"]     = jstr(part.image_data);
                    p->as_object_mut()["inlineData"] = inl;
                }
                parts->as_array_mut().push_back(p);
            }
        }
        entry->as_object_mut()["parts"] = parts;

        contents->as_array_mut().push_back(entry);
    }
    m["contents"] = contents;

    auto gen = jobj();
    auto& g = gen->as_object_mut();
    g["maxOutputTokens"] = jnum(static_cast<double>(req.max_tokens));
    if (req.include_temperature) g["temperature"] = jnum(req.temperature);
    // Gemini 2.5 / 3 thinking models: ask for thought summaries so the TUI
    // can render a ThinkingSegment.  Older Gemini models ignore unknown
    // generationConfig keys or reject them — gate on model id.
    // Flash-Lite defaults thinking off unless thinkingBudget is set.
    {
        const std::string stripped = strip_model_prefix(req.model);
        const bool thinking_model =
            stripped.find("gemini-2.5") != std::string::npos ||
            stripped.find("gemini-3") != std::string::npos;
        if (thinking_model) {
            auto tc = jobj();
            tc->as_object_mut()["includeThoughts"] = jbool(true);
            const bool lite =
                stripped.find("flash-lite") != std::string::npos ||
                stripped.find("flash_lite") != std::string::npos;
            if (lite) {
                tc->as_object_mut()["thinkingBudget"] = jnum(1024);
            }
            g["thinkingConfig"] = tc;
        }
    }
    m["generationConfig"] = gen;

    return json_serialize(*obj);
}

// ─── Outgoing HTTP request ───────────────────────────────────────────────────

void ApiClient::send_request(const Provider& p, Conn& c,
                              const std::string& path,
                              const std::string& body, bool streaming) {
    std::ostringstream http;
    http << "POST " << path << " HTTP/1.1\r\n";
    http << "Host: " << p.host;
    // Ollama + typical non-443 local servers want the port in the Host header
    // even for http; harmless for Anthropic (servers ignore a :443 there).
    if (!((p.tls && p.port == 443) || (!p.tls && p.port == 80))) {
        http << ":" << p.port;
    }
    http << "\r\n";
    http << "Content-Type: application/json\r\n";
    // Materialise the plaintext key only while building the request header.
    // SensitiveString zeroes its buffer on scope exit regardless of how the
    // scope exits (normal return, throw, etc.), so the key can't leak through
    // an exception path that bypasses the manual wipe below.  Missing key for
    // a provider that requires one → clean error via the caller's catch(...)
    // in complete() / stream().
    SensitiveString key_sensitive;
    if (p.uses_api_key) {
        if (api_keys_.find(p.name) == api_keys_.end()) {
            throw std::runtime_error(
                "No API key configured for provider '" + p.name + "'");
        }
        key_sensitive.value = unmask_api_key(p.name);
        if (key_sensitive.value.empty()) {
            throw std::runtime_error(
                "No API key configured for provider '" + p.name + "'");
        }
        if (p.format == Provider::FORMAT_ANTHROPIC) {
            http << "x-api-key: " << key_sensitive.value << "\r\n";
            http << "anthropic-version: 2023-06-01\r\n";
            http << "anthropic-beta: prompt-caching-2024-07-31\r\n";
        } else if (p.format == Provider::FORMAT_GEMINI) {
            // Gemini supports both `?key=…` and `x-goog-api-key`; the header
            // form keeps the token out of URLs and proxy access logs.
            http << "x-goog-api-key: " << key_sensitive.value << "\r\n";
        } else {
            http << "Authorization: Bearer " << key_sensitive.value << "\r\n";
            if (p.name == "openrouter") {
                const std::string referer = header_value_from_env(
                    "ARBITER_OPENROUTER_REFERER",
                    "https://github.com/tylerreckart/arbiter");
                const std::string title = header_value_from_env(
                    "ARBITER_OPENROUTER_TITLE",
                    "Arbiter");
                http << "HTTP-Referer: " << referer << "\r\n";
                http << "X-OpenRouter-Title: " << title << "\r\n";
                http << "X-OpenRouter-Categories: cli-agent,cloud-agent\r\n";
            }
        }
    }
    http << "Content-Length: " << body.size() << "\r\n";
    if (streaming) http << "Accept: text/event-stream\r\n";
    http << "Connection: keep-alive\r\n";
    http << "\r\n";
    http << body;
    // key_sensitive destructor zeroes the key here.  The `raw` buffer still
    // holds it (streamed into the ostringstream above); that buffer is wiped
    // immediately after send() completes.

    std::string raw = http.str();
    int total = static_cast<int>(raw.size());
    int sent = 0;
    while (sent < total) {
        int n = conn_send(c, raw.data() + sent, total - sent);
        if (n <= 0) {
            // Wipe before throwing — the key lives in `raw` until the
            // destructor, which on the error path may be delayed by stack
            // unwinding through exception handlers.
            OPENSSL_cleanse(raw.data(), raw.size());
            close_connection(c);
            throw std::runtime_error("write failed");
        }
        sent += n;
    }
    // Sent successfully — scrub the request buffer now rather than waiting
    // for std::string's destructor to release it to the allocator's free
    // list (where it would linger unzeroed until overwritten).
    OPENSSL_cleanse(raw.data(), raw.size());
}

// ─── Incoming HTTP response helpers ──────────────────────────────────────────

static int parse_http_status(const std::string& headers) {
    auto sp = headers.find(' ');
    if (sp == std::string::npos) return 0;
    return std::atoi(headers.c_str() + sp + 1);
}

// Reads until "\r\n\r\n"; body bytes past the sentinel come back in `leftover`.
static bool read_http_headers(arbiter::ApiClient::Conn& c,
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

namespace { struct PrefixCursor { const std::string& s; size_t pos = 0; }; }

static std::string read_response_body(arbiter::ApiClient::Conn& c,
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
        // OpenAI nests cached-prompt tokens under prompt_tokens_details
        // (implicit caching, no write cost); surface as cache_read_tokens.
        auto usage = root->get("usage");
        if (usage && usage->is_object()) {
            resp.input_tokens  = usage->get_int("prompt_tokens");
            resp.output_tokens = usage->get_int("completion_tokens");
            auto details = usage->get("prompt_tokens_details");
            if (details && details->is_object()) {
                resp.cache_read_tokens = details->get_int("cached_tokens");
            }
        }
        resp.ok = true;
    } catch (const std::exception& e) {
        resp.ok    = false;
        resp.error = std::string("Parse error: ") + e.what();
    }
    return resp;
}

ApiResponse ApiClient::parse_body_gemini(const std::string& body) {
    ApiResponse resp;
    resp.raw_body = body;
    try {
        auto root = json_parse(body);
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok         = false;
            resp.error_type = err->get_string("status");
            resp.error      = err->get_string("message", "Unknown API error");
            return resp;
        }
        auto candidates = root->get("candidates");
        if (candidates && candidates->is_array() && !candidates->as_array().empty()) {
            auto& c0 = candidates->as_array().front();
            if (c0) {
                auto cont = c0->get("content");
                if (cont) {
                    auto parts = cont->get("parts");
                    if (parts && parts->is_array()) {
                        for (auto& part : parts->as_array()) {
                            if (!part) continue;
                            std::string text = part->get_string("text");
                            if (text.empty()) continue;
                            // Thought summaries carry `"thought": true`.
                            if (part->get_bool("thought")) {
                                resp.reasoning += text;
                            } else {
                                resp.content += text;
                            }
                        }
                    }
                }
                resp.stop_reason = c0->get_string("finishReason");
            }
        }
        auto usage = root->get("usageMetadata");
        if (usage && usage->is_object()) {
            resp.input_tokens      = usage->get_int("promptTokenCount");
            resp.output_tokens     = usage->get_int("candidatesTokenCount");
            resp.cache_read_tokens = usage->get_int("cachedContentTokenCount");
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
    // Anthropic uses "rate_limit_error" / "overloaded_error"; Gemini uses
    // "RESOURCE_EXHAUSTED" / "UNAVAILABLE" in its `status` field.  OpenAI's
    // shape collides with Anthropic's, so the union below covers all three.
    return error_type == "rate_limit_error" ||
           error_type == "overloaded_error" ||
           error_type == "RESOURCE_EXHAUSTED" ||
           error_type == "UNAVAILABLE";
}

// ─── Blocking complete() ─────────────────────────────────────────────────────

ApiResponse ApiClient::complete(const ApiRequest& req) {
    // New call — clear any cancellation left over from a previous turn
    // (mirrors stream()).  Checked at every attempt boundary below so a
    // cancel() aborts this call instead of burning the retry budget
    // reconnecting through sockets it just shut down.
    cancelled_.store(false);
    static const int kMaxAttempts = 4;
    const Provider& prov = provider_for(req.model);

    // Circuit breaker: short-circuit before the retry loop when the
    // provider is currently Open.  Returning a structured "circuit
    // open" error lets the caller surface a fast-fail in the SSE
    // `done.error_code` taxonomy instead of burning 4 retries on a
    // provider we already know is unhealthy.
    if (breaker_ && !breaker_->allow(prov.name)) {
        ApiResponse r;
        r.ok         = false;
        r.error_type = "circuit_open";
        r.error      = "circuit breaker open for provider '" + prov.name + "'";
        return r;
    }
    if (metrics_) metrics_->inc_provider_call(prov.name);

    // Lease one connection for the whole call (kept across retries so a
    // reconnect reuses the same pool slot).  The lease is exclusive, so no
    // lock is held around the socket I/O below — concurrent callers on other
    // leased Conns run fully in parallel.
    ConnLease lease(*this, prov.name);
    Conn& c = lease.conn();

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (is_request_cancelled()) {
            // record_abandoned (not record_failure): the provider wasn't
            // proven bad, and a leaked HalfOpen probe would otherwise
            // reject the provider forever.
            if (breaker_) breaker_->record_abandoned(prov.name);
            ApiResponse r;
            r.ok         = false;
            r.error_type = "cancelled";
            r.error      = "request cancelled";
            return r;
        }

        if (attempt > 0) {
            usleep((1 << (attempt - 1)) * 1000000);
            if (metrics_) metrics_->inc_provider_retry(prov.name);
        }

        ApiResponse resp;
        bool threw = false;

        {
            try {
                if (!ensure_connection(prov, c)) {
                    // Counts as a provider failure so a HalfOpen probe
                    // admitted by allow() above is resolved rather than
                    // leaked (a leaked probe pins the breaker in HalfOpen
                    // and rejects the provider until process restart).
                    if (breaker_) breaker_->record_failure(prov.name);
                    ApiResponse r;
                    r.ok    = false;
                    r.error = c.last_error.empty() ? "Connection failed" : c.last_error;
                    return r;
                }
                std::string body;
                switch (prov.format) {
                    case Provider::FORMAT_ANTHROPIC:
                        body = build_body_anthropic(req, false); break;
                    case Provider::FORMAT_GEMINI:
                        body = build_body_gemini(req); break;
                    case Provider::FORMAT_OPENAI_CHAT: default:
                        body = build_body_openai(prov, req, false); break;
                }
                std::string path = path_for(prov, req, false);
                send_request(prov, c, path, body, false);
                std::string raw = read_response(c);
                switch (prov.format) {
                    case Provider::FORMAT_ANTHROPIC:
                        resp = parse_body_anthropic(raw); break;
                    case Provider::FORMAT_GEMINI:
                        resp = parse_body_gemini(raw); break;
                    case Provider::FORMAT_OPENAI_CHAT: default:
                        resp = parse_body_openai(raw); break;
                }
            } catch (...) {
                close_connection(c);
                threw = true;
            }
        }

        if (threw) {
            if (metrics_) metrics_->inc_provider_5xx(prov.name);
            if (attempt >= kMaxAttempts - 1) {
                if (breaker_) breaker_->record_failure(prov.name);
                ApiResponse r;
                r.ok    = false;
                r.error = "Request failed after retries";
                return r;
            }
            continue;
        }

        if (resp.ok) {
            if (breaker_) breaker_->record_success(prov.name);
            total_in_  += resp.input_tokens;
            total_out_ += resp.output_tokens;
            return resp;
        }

        // Metrics taxonomy: 429-shaped errors → 429 counter; everything
        // else retryable → 5xx counter (both anthropic "overloaded" and
        // gemini "UNAVAILABLE" map to the upstream-overloaded case).
        if (metrics_) {
            if (resp.error_type == "rate_limit_error" ||
                resp.error_type == "RESOURCE_EXHAUSTED") {
                metrics_->inc_provider_429(prov.name);
            } else if (is_retryable(resp.error_type)) {
                metrics_->inc_provider_5xx(prov.name);
            }
        }

        if (!is_retryable(resp.error_type) || attempt >= kMaxAttempts - 1) {
            if (breaker_) breaker_->record_failure(prov.name);
            return resp;
        }
    }

    if (breaker_) breaker_->record_failure(prov.name);
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
                                     StreamCallback cb,
                                     ReasoningCallback reasoning_cb) {
    if (data.empty() || data == "[DONE]") return;
    try {
        auto root = json_parse(data);
        std::string type = root->get_string("type");
        if (type == "content_block_delta") {
            auto delta = root->get("delta");
            if (delta) {
                const std::string dtype = delta->get_string("type");
                if (dtype == "text_delta") {
                    std::string text = delta->get_string("text");
                    content += text;
                    if (cb) cb(text);
                } else if (dtype == "thinking_delta") {
                    // Anthropic extended thinking — separate from prose.
                    std::string think = delta->get_string("thinking");
                    if (think.empty()) think = delta->get_string("text");
                    if (!think.empty()) {
                        resp.reasoning += think;
                        if (reasoning_cb) reasoning_cb(think);
                    }
                }
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
                                  StreamCallback cb,
                                  ReasoningCallback reasoning_cb) {
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
                    // OpenAI / OpenRouter reasoning models.
                    std::string think = delta->get_string("reasoning_content");
                    if (think.empty()) think = delta->get_string("reasoning");
                    if (!think.empty()) {
                        resp.reasoning += think;
                        if (reasoning_cb) reasoning_cb(think);
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
            auto details = usage->get("prompt_tokens_details");
            if (details && details->is_object()) {
                resp.cache_read_tokens = details->get_int("cached_tokens");
            }
        }
    } catch (...) {
        // Ignore malformed chunk.
    }
}

// Gemini SSE chunk: `data: {"candidates":[{"content":{"parts":[{"text":"…"}],
// "role":"model"},"finishReason":"STOP","index":0}],"usageMetadata":{...}}`.
// Thought summaries arrive as parts with `"thought": true` when
// generationConfig.thinkingConfig.includeThoughts is set.
// Gemini SSE: no [DONE] marker; final chunk carries finishReason + usageMetadata.
static void process_gemini_event(const std::string& data,
                                  std::string& content,
                                  ApiResponse& resp,
                                  StreamCallback cb,
                                  ReasoningCallback reasoning_cb) {
    if (data.empty()) return;
    try {
        auto root = json_parse(data);
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok         = false;
            resp.error_type = err->get_string("status");
            resp.error      = err->get_string("message", "Stream error");
            return;
        }
        auto candidates = root->get("candidates");
        if (candidates && candidates->is_array() && !candidates->as_array().empty()) {
            auto& c0 = candidates->as_array().front();
            if (c0) {
                auto cont = c0->get("content");
                if (cont) {
                    auto parts = cont->get("parts");
                    if (parts && parts->is_array()) {
                        for (auto& part : parts->as_array()) {
                            if (!part) continue;
                            std::string text = part->get_string("text");
                            if (text.empty()) continue;
                            if (part->get_bool("thought")) {
                                resp.reasoning += text;
                                if (reasoning_cb) reasoning_cb(text);
                            } else {
                                content += text;
                                if (cb) cb(text);
                            }
                        }
                    }
                }
                std::string finish = c0->get_string("finishReason");
                if (!finish.empty()) resp.stop_reason = finish;
            }
        }
        auto usage = root->get("usageMetadata");
        if (usage && usage->is_object()) {
            resp.input_tokens  = usage->get_int("promptTokenCount");
            resp.output_tokens = usage->get_int("candidatesTokenCount");
            // Implicit context caching shows up as `cachedContentTokenCount`
            // on usageMetadata (not always present); surface as cache_read_tokens.
            resp.cache_read_tokens = usage->get_int("cachedContentTokenCount");
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
        switch (fmt) {
            case Provider::FORMAT_ANTHROPIC:   return parse_body_anthropic(body);
            case Provider::FORMAT_GEMINI:      return parse_body_gemini(body);
            case Provider::FORMAT_OPENAI_CHAT: default:
                                                return parse_body_openai(body);
        }
    }

    bool chunked = headers.find("Transfer-Encoding: chunked") != std::string::npos;

    std::string line_buf;
    // SSE `data:` payloads are whole JSON events; 8 KB covers typical
    // Anthropic/OpenAI chunks without a realloc, and the growth path is
    // still there for the occasional large tool-result event.
    line_buf.reserve(8192);
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
            switch (fmt) {
                case Provider::FORMAT_ANTHROPIC:
                    process_anthropic_event(data, content, resp, cb, reasoning_cb_);
                    break;
                case Provider::FORMAT_GEMINI:
                    process_gemini_event(data, content, resp, cb, reasoning_cb_);
                    break;
                case Provider::FORMAT_OPENAI_CHAT: default:
                    process_openai_event(data, content, resp, cb, reasoning_cb_);
                    break;
            }
        }
    };

    auto feed = [&](const char* data, int n) {
        static constexpr size_t kMaxLineSize = 1048576;
        int i = 0;
        while (i < n) {
            // Bulk-copy the run up to the next '\n'.  SSE events are JSON
            // blobs that can be several KB; char-by-char `+=` reallocated
            // the line buffer repeatedly.  memchr lets us append one chunk
            // per newline rather than one per byte.
            const char* nl = static_cast<const char*>(
                std::memchr(data + i, '\n', n - i));
            int chunk_end = nl ? static_cast<int>(nl - data) : n;
            int take = chunk_end - i;
            if (take > 0 && line_buf.size() < kMaxLineSize) {
                size_t room = kMaxLineSize - line_buf.size();
                if (static_cast<size_t>(take) > room)
                    take = static_cast<int>(room);
                line_buf.append(data + i, take);
            }
            if (nl) {
                process_line(line_buf);
                line_buf.clear();
                i = chunk_end + 1;
            } else {
                i = n;
            }
        }
    };

    if (chunked) {
        while (!is_request_cancelled()) {
            std::string size_line;
            while (true) {
                int n = recv_some(buf, 1);
                if (n <= 0 || is_request_cancelled()) goto stream_done;
                if (buf[0] == '\n') break;
                if (buf[0] != '\r') size_line += buf[0];
            }
            int chunk_size = static_cast<int>(std::strtol(size_line.c_str(), nullptr, 16));
            if (chunk_size == 0) break;

            int read_so_far = 0;
            while (read_so_far < chunk_size) {
                if (is_request_cancelled()) goto stream_done;
                int to_read = std::min(chunk_size - read_so_far, (int)sizeof(buf));
                int n = recv_some(buf, to_read);
                if (n <= 0) goto stream_done;
                feed(buf, n);
                read_so_far += n;
            }
            recv_some(buf, 2);  // trailing \r\n
        }
        if (!is_request_cancelled()) recv_some(buf, 2);
    } else {
        int content_length = -1;
        auto cl_pos = headers.find("Content-Length: ");
        if (cl_pos != std::string::npos)
            content_length = std::atoi(headers.c_str() + cl_pos + 16);
        int read_so_far = 0;
        while ((content_length < 0 || read_so_far < content_length) && !is_request_cancelled()) {
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

    if (breaker_ && !breaker_->allow(prov.name)) {
        ApiResponse r;
        r.ok         = false;
        r.error_type = "circuit_open";
        r.error      = "circuit breaker open for provider '" + prov.name + "'";
        return r;
    }
    if (metrics_) metrics_->inc_provider_call(prov.name);

    // Lease one connection for the whole streaming call (see complete()).
    // The lease is exclusive to this thread, so the full SSE read runs with
    // no shared lock held — other panes streaming from their own leased Conns
    // proceed concurrently instead of blocking behind a single mutex.
    ConnLease lease(*this, prov.name);
    Conn& c = lease.conn();

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            usleep((1 << (attempt - 1)) * 1000000);
            if (metrics_) metrics_->inc_provider_retry(prov.name);
        }

        ApiResponse resp;
        bool threw = false;

        {
            try {
                if (!ensure_connection(prov, c)) {
                    resp.ok    = false;
                    resp.error = c.last_error.empty() ? "Connection failed" : c.last_error;
                } else {
                    std::string body;
                    switch (prov.format) {
                        case Provider::FORMAT_ANTHROPIC:
                            body = build_body_anthropic(req, true); break;
                        case Provider::FORMAT_GEMINI:
                            body = build_body_gemini(req); break;
                        case Provider::FORMAT_OPENAI_CHAT: default:
                            body = build_body_openai(prov, req, true); break;
                    }
                    std::string path = path_for(prov, req, true);
                    send_request(prov, c, path, body, true);
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
            if (breaker_) breaker_->record_success(prov.name);
            total_in_  += resp.input_tokens;
            total_out_ += resp.output_tokens;
            return resp;
        }

        if (metrics_) {
            if (threw) {
                metrics_->inc_provider_5xx(prov.name);
            } else if (resp.error_type == "rate_limit_error" ||
                       resp.error_type == "RESOURCE_EXHAUSTED") {
                metrics_->inc_provider_429(prov.name);
            } else if (is_retryable(resp.error_type)) {
                metrics_->inc_provider_5xx(prov.name);
            }
        }

        bool can_retry = resp.content.empty() &&
                         (threw || is_retryable(resp.error_type));
        if (!can_retry || attempt >= kMaxAttempts - 1) {
            if (breaker_) breaker_->record_failure(prov.name);
            return resp;
        }

        close_connection(c);
    }

    if (breaker_) breaker_->record_failure(prov.name);
    ApiResponse r;
    r.ok    = false;
    r.error = "Stream failed after retries";
    return r;
}

} // namespace arbiter
