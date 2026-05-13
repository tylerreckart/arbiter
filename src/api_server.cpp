// arbiter/src/api_server.cpp — see api_server.h
//
// Minimal HTTP/1.1 server that exposes the Orchestrator as a streaming SSE
// endpoint.  Purpose-built (no external HTTP library) — just enough parser
// to handle the one POST endpoint, a bearer-auth check, and an SSE
// response framer.  Production deployments live behind a reverse proxy
// that terminates TLS, so we don't bother with HTTPS here.

#include "api_server.h"

#include "commands.h"
#include "config.h"
#include "constitution.h"
#include "json.h"
#include "a2a/event_translator.h"
#include "a2a/manager.h"
#include "a2a/server.h"
#include "mcp/manager.h"
#include "orchestrator.h"
#include "billing_client.h"
#include "notification_bus.h"
#include "request_event_bus.h"
#include "schedule_parser.h"
#include "scheduler.h"
#include "circuit_breaker.h"
#include "idempotency_cache.h"
#include "logger.h"
#include "metrics.h"
#include "sandbox.h"
#include "tenant_limiter.h"
#include "tenant_store.h"
#include "tui/stream_filter.h"
#include "api_client.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <curl/curl.h>
#include <errno.h>
#include <execinfo.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace arbiter {

namespace {

// ─── Socket helpers ──────────────────────────────────────────────────────────

// Write the full buffer or fail silently (caller closes on error).  Uses
// MSG_NOSIGNAL on Linux; on macOS SO_NOSIGPIPE would be cleaner but
// MSG_NOSIGNAL is not available — we just accept EPIPE as "client gone".
void write_all(int fd, const char* data, size_t n) {
#ifdef MSG_NOSIGNAL
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::send(fd, data + off, n - off, flags);
        if (w <= 0) return;
        off += static_cast<size_t>(w);
    }
}

void write_all(int fd, const std::string& s) { write_all(fd, s.data(), s.size()); }

// ─── HTTP request parsing ───────────────────────────────────────────────────

struct HttpRequest {
    std::string method;     // "GET", "POST"
    std::string path;       // "/v1/orchestrate"
    std::string version;    // "HTTP/1.1"
    // Header name is stored lowercase because HTTP headers are case-
    // insensitive; callers look up by the canonical lowercase key.
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Read from `fd` until we see CRLFCRLF or the buffer exceeds a hard cap.
// Bytes read past the sentinel belong to the body and are returned via
// `leftover` so the body reader can consume them before touching the
// socket again.
bool read_http_headers(int fd, std::string& headers, std::string& leftover) {
    static constexpr size_t kMaxHeaderSize = 64 * 1024;
    static constexpr char kSentinel[] = "\r\n\r\n";
    static constexpr size_t kSentinelLen = 4;

    headers.clear();
    leftover.clear();
    char buf[4096];
    while (headers.size() < kMaxHeaderSize) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        size_t old = headers.size();
        headers.append(buf, static_cast<size_t>(n));
        // Backtrack up to 3 bytes so the sentinel isn't missed across a
        // read boundary.
        size_t scan_from = old >= kSentinelLen - 1 ? old - (kSentinelLen - 1) : 0;
        auto pos = headers.find(kSentinel, scan_from);
        if (pos != std::string::npos) {
            size_t end = pos + kSentinelLen;
            leftover.assign(headers, end, headers.size() - end);
            headers.resize(end);
            return true;
        }
    }
    return false;
}

bool parse_http_request(int fd, HttpRequest& req) {
    std::string raw, leftover;
    if (!read_http_headers(fd, raw, leftover)) return false;

    std::istringstream ss(raw);
    std::string line;

    // Request line: "METHOD PATH HTTP/1.1"
    if (!std::getline(ss, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream rs(line);
        rs >> req.method >> req.path >> req.version;
    }
    if (req.method.empty() || req.path.empty()) return false;

    // Headers until the empty line.
    //
    // Smuggling defense: a downstream proxy may interpret the request
    // differently from us if (a) Content-Length appears more than once,
    // (b) Transfer-Encoding is present (we don't speak chunked, so the
    // proxy and us would disagree on body framing), or (c) both
    // Content-Length and Transfer-Encoding are sent.  Reject all three
    // shapes outright.  We track this via duplicate-key detection
    // because the unordered_map below otherwise silently last-wins.
    bool saw_cl = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = to_lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        // Trim leading whitespace from value.
        size_t vstart = 0;
        while (vstart < value.size() && (value[vstart] == ' ' || value[vstart] == '\t'))
            ++vstart;
        if (name == "transfer-encoding") return false;     // not supported, also smuggling vector
        if (name == "content-length") {
            if (saw_cl) return false;                       // duplicate CL — refuse
            saw_cl = true;
        }
        req.headers[std::move(name)] = value.substr(vstart);
    }

    // Body — Content-Length only.  Chunked / keep-alive / pipelining are
    // out of scope; the one caller of this API sends a simple POST.
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        // Strict digit-only parse — std::stoul would silently accept
        // "+5", trailing junk ("100garbage"), or spaces, which a
        // misbehaving proxy could interpret differently.
        const std::string& v = it->second;
        if (v.empty()) return false;
        size_t want = 0;
        for (char c : v) {
            if (c < '0' || c > '9') return false;
            size_t prev = want;
            want = want * 10 + static_cast<size_t>(c - '0');
            if (want < prev) return false;                  // overflow
        }
        static constexpr size_t kMaxBody = 16 * 1024 * 1024;  // hard cap
        if (want > kMaxBody) return false;
        req.body = leftover;
        char buf[4096];
        while (req.body.size() < want) {
            size_t remaining = want - req.body.size();
            size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
            ssize_t n = ::recv(fd, buf, chunk, 0);
            if (n <= 0) return false;
            req.body.append(buf, static_cast<size_t>(n));
        }
    }
    return true;
}

// ─── CORS ───────────────────────────────────────────────────────────────────
//
// Default permissive (`*`) so a frontend on any origin can hit the API in
// dev with zero config.  Bearer auth carries in the Authorization header —
// no cookies — so we don't need Allow-Credentials.  To harden in production,
// put an origin allowlist in the reverse proxy OR extend these helpers to
// read a CSV from ARBITER_CORS_ORIGINS and echo only matches.
constexpr const char* kCorsHeaders =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Authorization, Content-Type, Accept, Idempotency-Key, If-None-Match\r\n"
    "Access-Control-Max-Age: 86400\r\n";

} // namespace (anon paused for response writers below)

// ─── HTTP response writers (non-SSE) ────────────────────────────────────────
//
// These three are at arbiter-namespace scope (not the surrounding
// anonymous namespace) so other TUs in arbiter — currently src/a2a/server.cpp
// — can write directly without going through the route-dispatch loop.
// The TU-local helpers they call (write_all, kCorsHeaders) live in the
// anonymous namespace above; same-TU access is unaffected by the linkage
// boundary.

void write_plain_response(int fd, int code, const std::string& reason,
                          const std::string& body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << reason << "\r\n"
       << "Content-Type: text/plain; charset=utf-8\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << kCorsHeaders
       << "Connection: close\r\n\r\n"
       << body;
    write_all(fd, ss.str());
}

void write_json_response(int fd, int code, std::shared_ptr<JsonValue> body) {
    std::string payload = json_serialize(*body);
    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << (code == 200 ? "OK" : "Error") << "\r\n"
       << "Content-Type: application/json; charset=utf-8\r\n"
       << "Content-Length: " << payload.size() << "\r\n"
       << kCorsHeaders
       << "Connection: close\r\n\r\n"
       << payload;
    write_all(fd, ss.str());
}

// 429 Too Many Requests with Retry-After.  Used by the per-tenant
// limiter when an expensive route (orchestrate, conversation messages,
// agent chat, A2A dispatch) is denied.  `reason` distinguishes
// concurrency exhaustion from rate-bucket exhaustion in the body —
// callers can branch on it without parsing the human string.
void write_429_response(int fd, int retry_after_seconds, const char* reason,
                         Metrics* metrics = nullptr, int64_t tenant_id = 0) {
    if (metrics) metrics->inc_rate_limited(tenant_id, reason);
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["error"]              = jstr("rate limit exceeded");
    m["reason"]             = jstr(reason);
    m["retry_after_seconds"] = jnum(retry_after_seconds);
    std::string payload = json_serialize(*body);
    std::ostringstream ss;
    ss << "HTTP/1.1 429 Too Many Requests\r\n"
       << "Content-Type: application/json; charset=utf-8\r\n"
       << "Content-Length: " << payload.size() << "\r\n"
       << "Retry-After: " << retry_after_seconds << "\r\n"
       << kCorsHeaders
       << "Connection: close\r\n\r\n"
       << payload;
    write_all(fd, ss.str());
}

namespace {

// CORS preflight response — 204 No Content + headers.  Browsers fire this
// ahead of any non-simple request (custom headers like Authorization, or
// PATCH/DELETE methods); answering it fast keeps perceived latency low.
void write_preflight_response(int fd) {
    std::ostringstream ss;
    ss << "HTTP/1.1 204 No Content\r\n"
       << kCorsHeaders
       << "Content-Length: 0\r\n"
       << "Connection: close\r\n\r\n";
    write_all(fd, ss.str());
}

// ─── SSE stream writer ──────────────────────────────────────────────────────
//
// Thread-safe: orchestrator callbacks fire from the request thread (which
// calls orch.send_streaming → API client stream decoder → filter → sse
// emit) so in principle one writer at a time; the mutex is belt-and-braces
// in case future callbacks get fanned out across threads.

class SseStream {
public:
    explicit SseStream(int fd) : fd_(fd) {}

    ~SseStream() { stop_heartbeat(); }

    void write_headers() {
        // Standard SSE headers.  X-Accel-Buffering: no tells nginx and
        // similar proxies to not buffer the response — without it, events
        // stall until the buffer fills.  Connection: close is fine for
        // our one-request-per-connection model.
        static const std::string kHdr =
            std::string("HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n"
                        "X-Accel-Buffering: no\r\n") +
            kCorsHeaders +
            "Connection: close\r\n\r\n";
        std::lock_guard<std::mutex> lk(mu_);
        write_all(fd_, kHdr);
        last_write_ = std::chrono::steady_clock::now();
        start_heartbeat_locked();
    }

    // Wire up durable persistence.  Each subsequent emit mirrors to
    // request_events (text events are coalesced into ~2 KB chunks
    // before persistence; other events persist 1:1) and broadcasts to
    // any RequestEventBus subscribers.  Borrowed pointers must outlive
    // this stream — typical lifetime is the request handler stack
    // frame.
    void set_persistence(TenantStore* ts, RequestEventBus* bus,
                          int64_t tenant_id, std::string request_id) {
        std::lock_guard<std::mutex> lk(mu_);
        ts_ = ts;
        bus_ = bus;
        persist_tenant_id_ = tenant_id;
        persist_request_id_ = std::move(request_id);
    }

    void emit(const std::string& event, std::shared_ptr<JsonValue> data) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        std::string payload = data ? json_serialize(*data) : "{}";
        // Wire write is always immediate and uncoalesced — clients see
        // each delta as it arrives.  Persistence is batched per the
        // operator's choice.
        std::string frame = "event: " + event + "\ndata: " + payload + "\n\n";
        write_all(fd_, frame);
        last_write_ = std::chrono::steady_clock::now();

        if (!ts_) return;
        if (event == "text") {
            try_coalesce_text_locked(data, payload);
        } else {
            flush_pending_text_locked();
            persist_event_locked(event, payload);
        }
    }

    // Flush any pending coalesced text and stop persisting / broadcasting.
    // Called by handle_orchestrate's finalisation code.  Wire-side close
    // is independent — closing the stream just stops accepting further
    // events, it doesn't shut the fd.
    void close() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_) return;
            flush_pending_text_locked();
            closed_ = true;
        }
        // Wake the heartbeat thread so it observes `closed_` and exits.
        // Joining happens in the destructor; close() shouldn't block on a
        // 5s wait_for cycle.
        hb_cv_.notify_all();
    }

private:
    // Spawn the heartbeat thread.  Called from write_headers under mu_.
    // The thread writes `: heartbeat\n\n` (an SSE comment, ignored by
    // clients but keeps TCP alive past idle-killing proxies) whenever
    // it's been more than kHeartbeatSeconds since the last wire write.
    void start_heartbeat_locked() {
        if (hb_thread_.joinable()) return;
        hb_thread_ = std::thread([this]() {
            using clock = std::chrono::steady_clock;
            std::unique_lock<std::mutex> lk(mu_);
            while (!hb_stop_ && !closed_) {
                hb_cv_.wait_for(lk, std::chrono::seconds(kHeartbeatTickSeconds));
                if (hb_stop_ || closed_) break;
                auto now = clock::now();
                if (now - last_write_ < std::chrono::seconds(kHeartbeatSeconds))
                    continue;
                // Wire write is fast for 14 bytes — keep the lock to
                // serialize against emit().  If the client is wedged
                // and write_all blocks, the connection thread is also
                // wedged; the drain deadline will eventually SIGKILL us.
                static const std::string kHb = ": heartbeat\n\n";
                write_all(fd_, kHb);
                last_write_ = now;
            }
        });
    }

    void stop_heartbeat() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            hb_stop_ = true;
        }
        hb_cv_.notify_all();
        if (hb_thread_.joinable()) hb_thread_.join();
    }

    void persist_event_locked(const std::string& kind,
                                const std::string& payload_json) {
        if (!ts_) return;
        ++seq_;
        try {
            ts_->append_request_event(persist_tenant_id_,
                                       persist_request_id_,
                                       seq_, kind, payload_json);
        } catch (...) {
            // Persistence is best-effort.  Failure (typically a
            // duplicate seq from a concurrent path, or a transient
            // SQLite busy) shouldn't kill the live wire stream.
            return;
        }
        if (bus_) {
            RequestEventEnvelope env;
            env.request_id    = persist_request_id_;
            env.seq           = seq_;
            env.event_kind    = kind;
            env.payload_json  = payload_json;
            env.terminal      = (kind == "done");
            try { bus_->publish(env); } catch (...) {}
        }
    }

    void try_coalesce_text_locked(const std::shared_ptr<JsonValue>& data,
                                    const std::string& payload_json) {
        if (!data || !data->is_object()) {
            // Unknown shape — persist as-is; coalescing only applies
            // to the canonical {agent, stream_id, delta} envelope.
            persist_event_locked("text", payload_json);
            return;
        }
        auto agent_v  = data->get("agent");
        auto stream_v = data->get("stream_id");
        auto delta_v  = data->get("delta");
        if (!agent_v  || !agent_v->is_string()  ||
            !stream_v || !stream_v->is_number() ||
            !delta_v  || !delta_v->is_string()) {
            persist_event_locked("text", payload_json);
            return;
        }
        std::string key = agent_v->as_string() + "|" +
                          std::to_string(static_cast<int64_t>(stream_v->as_number()));
        if (pending_text_size_ > 0 && key != pending_text_key_) {
            flush_pending_text_locked();
        }
        if (pending_text_size_ == 0) {
            pending_text_first_ = data;
            pending_text_key_   = key;
            pending_text_concat_.clear();
        }
        pending_text_concat_ += delta_v->as_string();
        pending_text_size_   += delta_v->as_string().size();
        if (pending_text_size_ >= kCoalesceThreshold) {
            flush_pending_text_locked();
        }
    }

    void flush_pending_text_locked() {
        if (pending_text_size_ == 0) return;
        // Build a coalesced payload that reuses the first chunk's
        // identity (agent, stream_id) but concatenates every delta.
        // Replay-time clients see one bigger chunk in place of many
        // small ones; the assembled string is identical.
        auto coalesced = jobj();
        auto& m = coalesced->as_object_mut();
        if (auto a = pending_text_first_->get("agent")) m["agent"] = a;
        if (auto s = pending_text_first_->get("stream_id")) m["stream_id"] = s;
        if (auto d = pending_text_first_->get("depth")) m["depth"] = d;
        m["delta"] = jstr(pending_text_concat_);
        std::string payload = json_serialize(*coalesced);
        persist_event_locked("text", payload);
        pending_text_first_.reset();
        pending_text_key_.clear();
        pending_text_concat_.clear();
        pending_text_size_ = 0;
    }

    int        fd_;
    std::mutex mu_;
    bool       closed_ = false;

    // Optional durable sink.  When ts_ is null, persistence + broadcast
    // are skipped; the stream behaves exactly as before.
    TenantStore*       ts_ = nullptr;
    RequestEventBus*   bus_ = nullptr;
    int64_t            persist_tenant_id_ = 0;
    std::string        persist_request_id_;
    int64_t            seq_ = 0;

    // Text-coalesce buffer.  Held under mu_ so all coalesce/flush
    // paths are serialized with wire writes.
    std::shared_ptr<JsonValue> pending_text_first_;
    std::string                pending_text_key_;
    std::string                pending_text_concat_;
    size_t                     pending_text_size_ = 0;
    static constexpr size_t    kCoalesceThreshold = 2048;

    // Heartbeat.  An SSE comment (`: heartbeat\n\n`) emitted whenever the
    // stream has been quiet longer than kHeartbeatSeconds, so reverse
    // proxies with idle-kill policies (nginx 60s default, cloudflare 100s)
    // don't drop long turns mid-LLM-call.  Comment frames are ignored by
    // spec-compliant clients; the only effect is keeping TCP alive.
    std::thread                       hb_thread_;
    std::condition_variable           hb_cv_;
    bool                              hb_stop_ = false;
    std::chrono::steady_clock::time_point last_write_{};
    static constexpr int              kHeartbeatSeconds      = 30;
    static constexpr int              kHeartbeatTickSeconds  = 5;
};

// ─── URL helpers ────────────────────────────────────────────────────────────

// Split "/v1/admin/tenants/42" → ["v1","admin","tenants","42"], stripping
// the query string first.  Empty segments ("/foo//bar") are dropped so the
// handler doesn't have to special-case trailing slashes.
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> out;
    auto q = path.find('?');
    std::string p = (q == std::string::npos) ? path : path.substr(0, q);
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i+1]), lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Parse "a=1&b=hello%20world" into a flat map.  Last value wins on repeats;
// values that fail url-decode are silently dropped.
std::map<std::string, std::string> parse_query(const std::string& path) {
    std::map<std::string, std::string> out;
    auto q = path.find('?');
    if (q == std::string::npos) return out;
    std::string qs = path.substr(q + 1);
    size_t start = 0;
    while (start < qs.size()) {
        size_t amp = qs.find('&', start);
        size_t end = (amp == std::string::npos) ? qs.size() : amp;
        auto eq = qs.find('=', start);
        if (eq != std::string::npos && eq < end) {
            out[url_decode(qs.substr(start, eq - start))] =
                url_decode(qs.substr(eq + 1, end - eq - 1));
        }
        start = (amp == std::string::npos) ? qs.size() : amp + 1;
    }
    return out;
}

// ─── Auth ───────────────────────────────────────────────────────────────────

// Extract the bearer token from an Authorization header, or empty if missing.
std::string extract_bearer(const HttpRequest& req) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return {};
    static constexpr const char* kPrefix = "Bearer ";
    static constexpr size_t      kPrefixLen = 7;
    const std::string& hdr = it->second;
    if (hdr.size() <= kPrefixLen ||
        hdr.compare(0, kPrefixLen, kPrefix) != 0)
        return {};
    return hdr.substr(kPrefixLen);
}

// ─── Orchestrate endpoint ───────────────────────────────────────────────────

// EventLogger — mirrors SSE events to stderr in real time so the operator
// running `arbiter --api` can watch what every tenant request is doing
// without a packet capture.  Thread-safe: parallel sub-agents emit
// concurrently, and the mutex serializes complete lines so they don't
// interleave mid-token.  Color is auto-disabled when stderr isn't a TTY.
//
// Two log levels:
//   • Always — request_received, done, error, cap_exceeded.  These are
//     low-volume and useful even in production: one INFO line per call,
//     plus errors.
//   • Verbose — text/thinking/tool_call/stream_end/file.  Off by default;
//     enabled per process via `--verbose` or env `ARBITER_API_VERBOSE=1`.
//     Each stream's deltas are line-buffered so a 200-token response
//     doesn't fragment into 200 stderr lines — we flush on newline.
//
// The output format is tuned for live demos: timestamps, request ids,
// tenant names, and internal stream/depth markers are all suppressed.
// Agent names are coloured per-agent so two parallel streams stay
// visually distinct; tool calls and file paths get their own colours.
// Each line stays narrow enough to avoid terminal wrapping.
class EventLogger {
public:
    EventLogger(bool verbose, std::string request_id, std::string tenant_name)
        : verbose_(verbose),
          color_(::isatty(fileno(stderr)) != 0),
          request_id_(std::move(request_id)),
          tenant_name_(std::move(tenant_name)) {
        (void)tenant_name_;   // retained for future structured logging.
    }

    // Emit one event.  `ev` is the SSE event name; `payload` mirrors the
    // JSON body about to be written to the wire.  The logger reads only
    // the fields it cares about; unknown shapes are tolerated.
    //
    // Layout (matches the design mock at docs):
    //   • A two-line block header on request_received (title + subtitle +
    //     horizontal rule).
    //   • A "marker form" for turn boundaries (request_received,
    //     stream_start): `event: <gap> <meta>` on one line followed by
    //     the event name in colour on its own line.
    //   • An "inline form" for events with a primary value (tool_call,
    //     gate, file, done, error): `event: <name> <gap> <value>`.
    //
    // Streamed text content (text/thinking deltas) is intentionally
    // suppressed: it already mirrors back to the client over SSE, and
    // duplicating multi-thousand-token agent prose into the operator's
    // stderr drowns out the event spine the verbose log is meant to
    // surface.  An operator who wants the full text reads the SSE
    // response directly.
    void log(const std::string& ev, const std::shared_ptr<JsonValue>& payload) {
        const bool always = (ev == "request_received" || ev == "done" ||
                             ev == "error");
        if (!always && !verbose_) return;

        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream line;

        if (ev == "request_received") {
            const std::string agent = payload ? payload->get_string("agent") : "";
            const std::string msg   = payload ? payload->get_string("message") : "";
            emit_header_locked("POST", "/v1/orchestrate", agent, msg);
            // Then the request_received marker, with the short request id
            // as its meta value.
            std::string short_id = request_id_;
            if (short_id.size() > 4) short_id.resize(4);
            std::string meta = std::string("req_") + short_id;
            emit_marker_locked(meta, "request_received", kBoldMagenta, line);
            return;
        }
        if (ev == "stream_start") {
            const std::string agent = payload ? payload->get_string("agent") : "";
            const int depth = payload ? static_cast<int>(payload->get_number("depth")) : 0;
            std::ostringstream meta;
            // Display depth+1 so the master shows as "depth 1" rather than
            // "depth 0" — matches the design's 1-indexed convention and
            // reads more naturally to humans skimming the log.
            meta << color_for_agent(agent) << display_agent(agent) << reset()
                 << " " << color(kDim) << "·" << reset()
                 << " depth " << (depth + 1);
            emit_marker_locked(meta.str(), "stream_start", kBoldMagenta, line);
            return;
        }
        if (ev == "agent_start") {
            // Already represented by stream_start; second announce is noise.
            return;
        }
        if (ev == "stream_end") {
            // Successful ends stay quiet (a coloured success marker on
            // its own line adds noise across many parallel streams);
            // failures surface so an operator notices a stalled
            // sub-agent.
            const bool ok = payload && payload->get_bool("ok");
            if (ok) return;
            emit_inline_locked("stream_end", kBoldRed, "stream ended without ok", line);
            return;
        }
        if (ev == "text" || ev == "thinking") {
            // Suppressed — the SSE response already carries the agent's
            // text to the client; mirroring it on the operator's stderr
            // turned the verbose log into a wall of prose.  Tool /
            // status events alone tell the operator-relevant story.
            return;
        }
        if (ev == "tool_call") {
            const std::string tool = payload ? payload->get_string("tool") : "";
            const bool ok = payload && payload->get_bool("ok");
            std::ostringstream value;
            value << color_for_tool(tool) << "/" << tool << reset();
            if (!ok) value << " " << color(kBoldRed) << "ERR" << reset();
            emit_inline_locked("tool_call", kBoldCyan, value.str(), line);
            return;
        }
        if (ev == "token_usage" || ev == "sub_agent_response") {
            // Suppressed — the design surfaces aggregate token counts on
            // `done`, and sub-agent text already streamed via deltas.
            return;
        }
        if (ev == "file") {
            const std::string path = payload ? payload->get_string("path") : "";
            const double size = payload ? payload->get_number("size") : 0;
            std::ostringstream value;
            value << "wrote " << path
                  << " " << color(kDim) << "("
                  << fmt_size(static_cast<int64_t>(size)) << ")" << reset();
            emit_inline_locked("file", kBoldMagenta, value.str(), line);
            return;
        }
        if (ev == "advisor") {
            const std::string kind    = payload ? payload->get_string("kind")  : "";
            const std::string detail  = payload ? payload->get_string("detail") : "";
            const std::string preview = payload ? payload->get_string("preview") : "";
            const bool malformed      = payload && payload->get_bool("malformed");

            const char* clr = kBoldYellow;
            std::string label = "gate";
            std::ostringstream value;
            if (kind == "consult") {
                clr = kBoldCyan;
                label = "advise";
                value << quote_short(detail.empty() ? preview : detail, 80);
            } else if (kind == "gate_continue") {
                clr = kBoldYellow;
                value << "verdict: continue " << color(kGreen) << "✓" << reset();
            } else if (kind == "gate_redirect") {
                clr = kBoldYellow;
                value << "verdict: redirect " << color(kYellow) << "↻" << reset();
                if (!detail.empty()) value << "  " << quote_short(detail, 60);
            } else if (kind == "gate_halt") {
                clr = kBoldRed;
                value << "verdict: halt " << color(kBoldRed) << "✗" << reset();
                if (!detail.empty()) value << "  " << quote_short(detail, 60);
            } else if (kind == "gate_budget") {
                clr = kBoldRed;
                value << "verdict: budget " << color(kBoldRed) << "⛔" << reset();
            } else {
                value << kind << " " << detail;
            }
            if (malformed) value << " " << color(kDim) << "(malformed)" << reset();
            emit_inline_locked(label, clr, value.str(), line);
            return;
        }
        if (ev == "escalation") {
            const std::string reason = payload ? payload->get_string("reason") : "";
            emit_inline_locked("escalation", kBoldRed,
                                quote_short(reason, 80), line);
            return;
        }
        if (ev == "done") {
            const bool ok    = payload && payload->get_bool("ok");
            const double dur = payload ? payload->get_number("duration_ms") : 0;
            const double in  = payload ? payload->get_number("input_tokens") : 0;
            const double out = payload ? payload->get_number("output_tokens") : 0;
            std::ostringstream value;
            value << (ok ? "ok=true" : "ok=false")
                  << " " << color(kDim) << "·" << reset() << " "
                  << std::fixed << std::setprecision(1) << (dur / 1000.0) << "s";
            const double cost = estimate_cost(in, out);
            if (cost > 0) {
                value << " " << color(kDim) << "·" << reset() << " "
                      << "$" << std::fixed << std::setprecision(4) << cost;
            } else if (in > 0 || out > 0) {
                value << " " << color(kDim) << "·" << reset() << " "
                      << "in=" << static_cast<int>(in)
                      << " out=" << static_cast<int>(out);
            }
            if (!ok && payload) {
                const std::string err = payload->get_string("error");
                if (!err.empty()) value << "  " << quote_short(err, 60);
            }
            emit_inline_locked("done", ok ? kBoldGreen : kBoldRed, value.str(), line);
            return;
        }
        if (ev == "error") {
            const std::string m = payload ? payload->get_string("message") : "";
            emit_inline_locked("error", kBoldRed, quote_short(m, 100), line);
            return;
        }
        // Unknown event — log the name only; useful while iterating.
        emit_inline_locked(ev, kDim, "", line);
    }

private:
    // ANSI colour codes — only emitted when stderr is a TTY.
    static constexpr const char* kReset         = "\033[0m";
    static constexpr const char* kBold          = "\033[1m";
    static constexpr const char* kDim           = "\033[2m";
    static constexpr const char* kRed           = "\033[31m";
    static constexpr const char* kGreen         = "\033[32m";
    static constexpr const char* kYellow        = "\033[33m";
    static constexpr const char* kCyan          = "\033[36m";
    static constexpr const char* kBoldRed       = "\033[1;31m";
    static constexpr const char* kBoldGreen     = "\033[1;32m";
    static constexpr const char* kBoldYellow    = "\033[1;33m";
    static constexpr const char* kBoldCyan      = "\033[1;36m";
    static constexpr const char* kBoldMagenta   = "\033[1;35m";

    // Column where event values align.  "event:" is 6 chars; "event: " + an
    // event-name token leaves us at "event: <name> <pad> value" with the
    // value starting at column 24 from line origin.  Tuned by eye against
    // the design mock — wide enough that "tool_call" and "request_received"
    // both fit comfortably without wrapping the value column off-screen.
    static constexpr int kValueCol = 24;
    // Per-agent colour palette — muted 256-colour shades.  The previous
    // bright-only palette read uniformly garish across siblings in a
    // /parallel fan-out; these tones stay distinguishable side-by-side
    // without competing for attention.  Hashed on the *display* name
    // (post-`seed-` strip) so a starter and its prefixed twin draw in
    // the same colour.
    static constexpr const char* kAgentPalette[] = {
        "\033[38;5;109m",  // soft cyan
        "\033[38;5;144m",  // khaki
        "\033[38;5;110m",  // light steel blue
        "\033[38;5;138m",  // dusty pink
        "\033[38;5;108m",  // sage
        "\033[38;5;180m",  // warm tan
        "\033[38;5;175m",  // mauve
        "\033[38;5;152m",  // pale aqua
        "\033[38;5;187m",  // light buff
        "\033[38;5;146m",  // periwinkle
    };
    // Per-tool colour palette — distinct from the agent palette so the
    // tool token visually separates from the agent token on the same
    // line.  Hashed on the tool name (`search`, `fetch`, `mem`, ...) so
    // every invocation of the same tool draws in the same colour.
    static constexpr const char* kToolPalette[] = {
        "\033[38;5;73m",   // teal
        "\033[38;5;178m",  // gold
        "\033[38;5;168m",  // rose
        "\033[38;5;105m",  // periwinkle (deeper)
        "\033[38;5;137m",  // terracotta
        "\033[38;5;79m",   // seafoam
        "\033[38;5;167m",  // coral
        "\033[38;5;115m",  // mint
        "\033[38;5;215m",  // peach
        "\033[38;5;141m",  // amethyst
    };

    // Strip a `seed-` prefix from the displayed agent name.  The starter
    // agents seeded by `arbiter --init` carry that prefix internally for
    // disambiguation; surfacing it in every log line is just noise.
    static std::string display_agent(const std::string& name) {
        constexpr const char* kPrefix = "seed-";
        constexpr size_t      kLen    = 5;
        if (name.size() > kLen && name.compare(0, kLen, kPrefix) == 0)
            return name.substr(kLen);
        return name;
    }
    const char* color_for_agent(const std::string& name) const {
        if (!color_ || name.empty()) return "";
        const std::string disp = display_agent(name);
        size_t h = 0;
        for (char c : disp) h = h * 131 + static_cast<unsigned char>(c);
        constexpr size_t N = sizeof(kAgentPalette) / sizeof(kAgentPalette[0]);
        return kAgentPalette[h % N];
    }
    const char* color_for_tool(const std::string& name) const {
        if (!color_ || name.empty()) return "";
        size_t h = 0;
        for (char c : name) h = h * 131 + static_cast<unsigned char>(c);
        constexpr size_t N = sizeof(kToolPalette) / sizeof(kToolPalette[0]);
        return kToolPalette[h % N];
    }

    const char* color(const char* c) const { return color_ ? c : ""; }
    const char* reset() const               { return color_ ? kReset : ""; }

// Pad an ostringstream out to `kValueCol` from line origin, given the
    // visible character count already written.  ANSI escapes don't count
    // toward visible width — callers pass the visible-only count.
    static void pad_to_value_col(std::ostringstream& line, int written) {
        int pad = kValueCol - written;
        if (pad < 1) pad = 1;
        for (int i = 0; i < pad; ++i) line << ' ';
    }

    // Emit a single SSE record in "marker form": one line `event: <gap>
    // <meta>`, then the event name on its own line in `event_color`.
    // Used for turn boundaries (request_received, stream_start) where
    // the event itself is the salient signal and the meta value just
    // contextualizes which agent / id the boundary applies to.
    void emit_marker_locked(const std::string& meta_value,
                             const std::string& event_name,
                             const char* event_color,
                             std::ostringstream& line) {
        line << color(kDim) << "event:" << reset();
        pad_to_value_col(line, /*written=*/6);   // "event:" is 6 chars
        line << color(kDim) << meta_value << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();

        line << color(event_color) << event_name << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();
    }

    // Emit a single SSE record in "inline form": `event: <name> <gap>
    // <value>` on one line.  Used for events with a primary value
    // (tool_call, gate, file, done, error).  The value column lines up
    // with the marker form's meta column so the two intermix cleanly.
    void emit_inline_locked(const std::string& event_name,
                             const char* event_color,
                             const std::string& value,
                             std::ostringstream& line) {
        line << color(kDim) << "event: " << reset()
             << color(event_color) << event_name << reset();
        const int written = 7 + static_cast<int>(event_name.size());
        pad_to_value_col(line, written);
        line << value;
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();
    }

    // Emit the request header — three lines anchoring the rest of the
    // log block.  Fired exactly once per request, on request_received.
    void emit_header_locked(const std::string& method,
                             const std::string& path,
                             const std::string& agent,
                             const std::string& message) {
        std::ostringstream line;
        line << color(kBold) << "arbiter "
             << color(kDim) << "↗" << reset()
             << color(kBold) << " " << method << " " << path << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();

        line << color(kDim) << "agent: " << reset()
             << color_for_agent(agent) << display_agent(agent) << reset()
             << color(kDim) << " message: " << reset()
             << quote_short(message, 70);
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();

        // Horizontal rule.  Width is screen-friendly without ever
        // wrapping in an 80-col terminal.  Drawn dim so it recedes.
        line << color(kDim);
        for (int i = 0; i < 70; ++i) line << "─";
        line << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
    }

    // Emit one text/thinking line in the column layout: streamed text
    // Rough cost estimate in USD for the demo log.  Sonnet-equivalent
    // pricing ($3/M input, $15/M output) — the actual ledger lives in
    // the billing service.  Returns 0 when no tokens were used so the
    // caller can fall back to a tokens display.
    static double estimate_cost(double in_tokens, double out_tokens) {
        if (in_tokens <= 0 && out_tokens <= 0) return 0.0;
        return (in_tokens / 1'000'000.0) * 3.0
             + (out_tokens / 1'000'000.0) * 15.0;
    }

    // Truncate to a screen-friendly preview and quote.  Newlines flatten to
    // spaces so a single log line stays one row in the operator's terminal.
    static std::string quote_short(const std::string& s, size_t cap = 110) {
        std::string out;
        out.reserve(std::min(s.size(), cap) + 8);
        out += '"';
        size_t take = std::min(s.size(), cap);
        for (size_t i = 0; i < take; ++i) {
            char c = s[i];
            if (c == '\n' || c == '\r' || c == '\t') out += ' ';
            else out += c;
        }
        out += '"';
        if (s.size() > cap) out += "…";
        return out;
    }

    // Bytes → "120B" / "3.4KB" / "1.2MB".  Demo-friendly file/size labels.
    static std::string fmt_size(int64_t bytes) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1);
        if (bytes < 1024)              o << bytes << "B";
        else if (bytes < 1024 * 1024)  o << (bytes / 1024.0) << "KB";
        else                            o << (bytes / (1024.0 * 1024.0)) << "MB";
        return o.str();
    }

    bool        verbose_;
    bool        color_;
    std::string request_id_;
    std::string tenant_name_;
    std::mutex  mu_;
};

void emit_error(SseStream& sse, const std::string& msg) {
    auto o = jobj();
    o->as_object_mut()["message"] = jstr(msg);
    sse.emit("error", o);
}

// ─── Admin endpoints ────────────────────────────────────────────────────────
//
// All admin routes are JSON-in, JSON-out.  Billing has moved to
// the billing service — this surface only manages tenant identity and the
// per-tenant access tokens used by the runtime hot path.

std::shared_ptr<JsonValue> tenant_to_json(const Tenant& t) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]           = jnum(static_cast<double>(t.id));
    m["name"]         = jstr(t.name);
    m["disabled"]     = jbool(t.disabled);
    m["created_at"]   = jnum(static_cast<double>(t.created_at));
    m["last_used_at"] = jnum(static_cast<double>(t.last_used_at));
    return o;
}

void admin_error(int fd, int code, const std::string& msg) {
    auto e = jobj();
    e->as_object_mut()["error"] = jstr(msg);
    write_json_response(fd, code, e);
}

// Compare bearer to the admin token.  Timing-safe is overkill for a shared
// secret loaded from disk at startup, but it costs nothing here and keeps
// future pen-test reviewers from flagging a naive ==.
bool admin_token_matches(const std::string& got, const std::string& want) {
    if (got.size() != want.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < got.size(); ++i)
        diff |= static_cast<unsigned char>(got[i] ^ want[i]);
    return diff == 0;
}

void handle_admin(int fd, const HttpRequest& req,
                  TenantStore& tenants,
                  InFlightRegistry& in_flight,
                  const ApiServerOptions& opts) {
    if (opts.admin_token.empty()) {
        admin_error(fd, 503, "admin endpoints disabled (no admin token configured)");
        return;
    }
    const std::string got = extract_bearer(req);
    if (got.empty() || !admin_token_matches(got, opts.admin_token)) {
        admin_error(fd, 401, "invalid admin bearer token");
        return;
    }

    const auto segs = split_path(req.path);
    // Expect: ["v1","admin","<resource>", ...]
    if (segs.size() < 3 || segs[0] != "v1" || segs[1] != "admin") {
        admin_error(fd, 404, "admin route not found");
        return;
    }
    const std::string& resource = segs[2];

    // ── /v1/admin/tenants and /v1/admin/tenants/{id} ────────────────────
    if (resource == "tenants") {
        if (segs.size() == 3) {
            if (req.method == "GET") {
                auto arr = jarr();
                auto& a = arr->as_array_mut();
                for (auto& t : tenants.list_tenants()) a.push_back(tenant_to_json(t));
                auto body = jobj();
                body->as_object_mut()["tenants"] = arr;
                write_json_response(fd, 200, body);
                return;
            }
            if (req.method == "POST") {
                std::shared_ptr<JsonValue> body;
                try { body = json_parse(req.body); }
                catch (const std::exception& e) {
                    admin_error(fd, 400, std::string("invalid JSON: ") + e.what());
                    return;
                }
                if (!body || !body->is_object()) {
                    admin_error(fd, 400, "body must be a JSON object");
                    return;
                }
                const std::string name = body->get_string("name");
                if (name.empty()) {
                    admin_error(fd, 400, "missing required field: 'name'");
                    return;
                }

                TenantStore::CreatedTenant created;
                try { created = tenants.create_tenant(name); }
                catch (const std::exception& e) {
                    admin_error(fd, 500, std::string("create failed: ") + e.what());
                    return;
                }
                // Audit log: record the create.  Token plaintext is
                // deliberately NOT included — we don't want it sitting
                // in the audit table where a future read endpoint
                // exposes it.
                {
                    auto after = jobj();
                    auto& am = after->as_object_mut();
                    am["id"]   = jnum(static_cast<double>(created.tenant.id));
                    am["name"] = jstr(created.tenant.name);
                    try {
                        tenants.append_admin_audit("admin", "create_tenant",
                            "tenant", std::to_string(created.tenant.id),
                            /*before=*/"", json_serialize(*after));
                    } catch (...) { /* audit is best-effort */ }
                }
                auto resp = tenant_to_json(created.tenant);
                // The plaintext token is ONLY returned here — the DB stores
                // SHA-256 digest, so a misplaced token means rotating it.
                resp->as_object_mut()["token"] = jstr(created.token);
                write_json_response(fd, 201, resp);
                return;
            }
            admin_error(fd, 405, "method not allowed");
            return;
        }

        if (segs.size() == 4) {
            int64_t id = 0;
            try { id = std::stoll(segs[3]); } catch (...) { id = 0; }
            if (id <= 0) { admin_error(fd, 400, "bad tenant id"); return; }

            if (req.method == "GET") {
                auto t = tenants.get_tenant(id);
                if (!t) { admin_error(fd, 404, "tenant not found"); return; }
                write_json_response(fd, 200, tenant_to_json(*t));
                return;
            }
            if (req.method == "PATCH") {
                std::shared_ptr<JsonValue> body;
                try { body = json_parse(req.body); }
                catch (const std::exception& e) {
                    admin_error(fd, 400, std::string("invalid JSON: ") + e.what());
                    return;
                }
                if (!body || !body->is_object()) {
                    admin_error(fd, 400, "body must be a JSON object");
                    return;
                }
                // `disabled` is the only mutable field — billing-related
                // fields have moved to the billing service.
                if (auto v = body->get("disabled"); v && v->is_bool()) {
                    const bool now_disabled = v->as_bool();
                    // Capture pre-update state for the audit row.  If
                    // the tenant doesn't exist we'll bail out before
                    // writing the audit, so capturing before set_disabled
                    // is safe.
                    auto before = tenants.get_tenant(id);
                    tenants.set_disabled(std::to_string(id), now_disabled);
                    // Kill in-flight streams immediately when disabling.
                    // Without this, an authenticated tenant's existing
                    // SSE stream keeps running until the model finishes —
                    // the operator believes the kill-switch is hot when
                    // it isn't.  Holding reg.mu across cancel() is safe:
                    // Orchestrator::cancel only flips an atomic and
                    // shuts down sockets under its own mutex.
                    if (now_disabled) {
                        std::lock_guard<std::mutex> lk(in_flight.mu);
                        for (auto& [_, entry] : in_flight.by_id) {
                            if (entry.tenant_id == id && entry.orch) {
                                entry.orch->cancel();
                            }
                        }
                    }
                    // Audit only if the row existed (else the PATCH
                    // would 404 below and we'd be auditing a no-op).
                    if (before) {
                        auto bj = jobj(); auto aj = jobj();
                        bj->as_object_mut()["disabled"] = jbool(before->disabled);
                        aj->as_object_mut()["disabled"] = jbool(now_disabled);
                        try {
                            tenants.append_admin_audit("admin", "update_tenant",
                                "tenant", std::to_string(id),
                                json_serialize(*bj), json_serialize(*aj));
                        } catch (...) {}
                    }
                }
                auto t = tenants.get_tenant(id);
                if (!t) { admin_error(fd, 404, "tenant not found"); return; }
                write_json_response(fd, 200, tenant_to_json(*t));
                return;
            }
            admin_error(fd, 405, "method not allowed");
            return;
        }

        admin_error(fd, 404, "admin route not found");
        return;
    }

    // ── /v1/admin/audit ─────────────────────────────────────────────────
    // Read-only log of admin mutations.  Newest first, paginated with
    // `?before_id=N` (the smallest id from the previous page).
    if (resource == "audit" && segs.size() == 3) {
        if (req.method != "GET") {
            admin_error(fd, 405, "method not allowed");
            return;
        }
        int64_t before_id = 0;
        int     limit     = 50;
        auto qpos = req.path.find('?');
        if (qpos != std::string::npos) {
            std::string qs = req.path.substr(qpos + 1);
            size_t i = 0;
            while (i < qs.size()) {
                size_t amp = qs.find('&', i);
                std::string pair = qs.substr(i, amp - i);
                auto eq = pair.find('=');
                if (eq != std::string::npos) {
                    std::string k = pair.substr(0, eq);
                    std::string v = pair.substr(eq + 1);
                    if (k == "before_id") {
                        try { before_id = std::stoll(v); } catch (...) {}
                    } else if (k == "limit") {
                        try { limit = std::stoi(v); } catch (...) {}
                    }
                }
                if (amp == std::string::npos) break;
                i = amp + 1;
            }
        }
        auto rows = tenants.list_admin_audit(before_id, limit);
        auto arr = jarr();
        auto& a  = arr->as_array_mut();
        for (auto& r : rows) {
            auto o = jobj();
            auto& m = o->as_object_mut();
            m["id"]          = jnum(static_cast<double>(r.id));
            m["ts"]          = jnum(static_cast<double>(r.ts));
            m["actor"]       = jstr(r.actor);
            m["action"]      = jstr(r.action);
            m["target_kind"] = jstr(r.target_kind);
            m["target_id"]   = jstr(r.target_id);
            // Re-parse the JSON payloads so consumers don't have to do
            // a double-decode; an unparseable payload (shouldn't happen
            // since we wrote them) is surfaced as a string.
            auto parse_or_str = [](const std::string& s)
                    -> std::shared_ptr<JsonValue> {
                if (s.empty()) return jnull();
                try { return json_parse(s); }
                catch (...) { return jstr(s); }
            };
            m["before"]      = parse_or_str(r.before_json);
            m["after"]       = parse_or_str(r.after_json);
            a.push_back(o);
        }
        auto body = jobj();
        body->as_object_mut()["entries"] = arr;
        write_json_response(fd, 200, body);
        return;
    }

    // Usage/billing endpoints have moved to the billing service.  The runtime
    // no longer exposes /v1/admin/usage or /v1/admin/usage/summary —
    // the sibling billing service owns the ledger and any rollups.
    admin_error(fd, 404, "admin resource not found");
}

// ─── Web search backend ─────────────────────────────────────────────────────
//
// One libcurl GET to the Brave Search API per /search call.  Brave is the
// v1 provider; the SearchProvider field in ApiServerOptions reserves slots
// for Tavily/Exa, which would each plug in here as a parallel branch.
//
// Output format matches the SearchInvoker contract documented in
// commands.h: numbered lines "<n>. <title> — <snippet>\n   <url>".  The
// dispatcher wraps this in [/search ...] / [END SEARCH] framing and
// applies the 16 KB body cap.

namespace {

// Parse a /mem-style id token, tolerantly.  /mem entries renders ids as
// `#<n>` for human readability (matches the convention of /read #<n>);
// agents copy that form back into follow-up calls and would otherwise
// hit ERR because std::stoll won't parse '#'.  Strip a leading '#' and
// surrounding whitespace before parsing so the rendered form and the
// accepted form agree.  Returns 0 on any parse failure — callers should
// reject 0 with a usage hint.
inline int64_t mem_parse_id(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(0, 1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t'))
        s.pop_back();
    if (!s.empty() && s.front() == '#') s.erase(0, 1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(0, 1);
    try { return std::stoll(s); }
    catch (...) { return 0; }
}

size_t brave_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    // Cap inbound JSON at 256 KB — Brave's web/search response is ~10–50 KB
    // for the default 10 results, but a misbehaving response shouldn't be
    // able to exhaust process memory.
    constexpr size_t kMaxResponseBytes = 256 * 1024;
    if (buf->size() + bytes > kMaxResponseBytes) return 0;
    buf->append(ptr, bytes);
    return bytes;
}

// Percent-encode the query string for use in a URL.  We can't rely on
// curl_easy_escape because we'd need a CURL handle to call it; the inline
// encoder here is fine for the small character set we care about.
std::string url_encode(const std::string& in) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%';
            if (c < 0x10) out << '0';
            out << static_cast<int>(c);
        }
    }
    return out.str();
}

// Trim whitespace from both ends.
std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                           s.front() == '\n' || s.front() == '\r')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                           s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Render a Brave Search /web/search response into the SearchInvoker output
// format.  Pulls title, URL, and description from each web.results entry.
// On any shape surprise (missing field, non-string type) we skip the entry
// rather than failing the whole call — partial results beat no results.
std::string brave_render(const std::string& json_body, int top_n) {
    std::shared_ptr<JsonValue> root;
    try { root = json_parse(json_body); }
    catch (const std::exception& e) {
        return std::string("ERR: Brave returned non-JSON response: ") + e.what();
    }
    if (!root || !root->is_object()) {
        return "ERR: Brave response was not a JSON object";
    }
    // Surface API-level errors verbatim — the caller wants to see "rate
    // limited" or "invalid token" instead of a silent empty result list.
    if (auto err = root->get("error"); err && err->is_object()) {
        std::string msg = err->get_string("message", "");
        std::string code = err->get_string("code", "");
        return "ERR: Brave API error" +
               (code.empty() ? "" : " [" + code + "]") +
               (msg.empty()  ? "" : ": "  + msg);
    }
    auto web = root->get("web");
    if (!web || !web->is_object()) return "(no web results)\n";
    auto results = web->get("results");
    if (!results || !results->is_array() || results->as_array().empty())
        return "(no web results)\n";

    std::ostringstream out;
    int n = 0;
    for (auto& item : results->as_array()) {
        if (!item || !item->is_object()) continue;
        std::string title = item->get_string("title", "");
        std::string url   = item->get_string("url", "");
        std::string desc  = item->get_string("description", "");
        if (url.empty()) continue;
        // Trim long descriptions — Brave can return 300+ chars; 240 keeps
        // the per-line block readable while preserving the gist.
        if (desc.size() > 240) { desc.resize(237); desc += "..."; }
        // Normalise <strong>...</strong> highlighting Brave injects into
        // titles + descriptions; stripping the tags keeps the model's
        // output clean.
        for (const char* tag : {"<strong>", "</strong>", "<b>", "</b>"}) {
            for (auto pos = desc.find(tag); pos != std::string::npos; pos = desc.find(tag)) {
                desc.erase(pos, std::strlen(tag));
            }
            for (auto pos = title.find(tag); pos != std::string::npos; pos = title.find(tag)) {
                title.erase(pos, std::strlen(tag));
            }
        }
        ++n;
        out << n << ". " << title;
        if (!desc.empty()) out << " — " << desc;
        out << "\n   " << url << "\n";
        if (n >= top_n) break;
    }
    if (n == 0) return "(no web results)\n";
    return out.str();
}

std::string brave_search(const std::string& query, const std::string& api_key,
                          int top_n) {
    if (api_key.empty()) {
        return "ERR: search provider configured without an API key — set "
               "ARBITER_SEARCH_API_KEY (or BRAVE_SEARCH_API_KEY) in the "
               "API server's environment.";
    }
    if (query.empty()) return "ERR: empty query";

    const int requested = std::clamp(top_n, 1, 20);
    std::string url = "https://api.search.brave.com/res/v1/web/search?q=" +
                       url_encode(query) +
                       "&count=" + std::to_string(requested);

    CURL* curl = curl_easy_init();
    if (!curl) return "ERR: curl_easy_init failed";

    std::string response;
    struct curl_slist* headers = nullptr;
    const std::string subscription_header = "X-Subscription-Token: " + api_key;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, subscription_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, brave_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "arbiter/0.4.4");

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return std::string("ERR: HTTP failure (") + curl_easy_strerror(rc) + ")";
    }
    if (http_code == 401 || http_code == 403) {
        return "ERR: Brave returned " + std::to_string(http_code) +
               " — check ARBITER_SEARCH_API_KEY";
    }
    if (http_code == 429) {
        return "ERR: Brave rate-limited (429) — slow down or upgrade plan";
    }
    if (http_code < 200 || http_code >= 300) {
        return "ERR: Brave returned HTTP " + std::to_string(http_code);
    }
    return brave_render(response, requested);
}

} // namespace

// ─── Tenant-scoped agents + memory ──────────────────────────────────────────

namespace fs = std::filesystem;

std::string brevity_s(Brevity b) {
    switch (b) {
        case Brevity::Lite:  return "lite";
        case Brevity::Full:  return "full";
        case Brevity::Ultra: return "ultra";
    }
    return "full";
}

std::shared_ptr<JsonValue> constitution_to_json(const std::string& id,
                                                 const Constitution& c) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]           = jstr(id);
    m["name"]         = jstr(c.name);
    m["role"]         = jstr(c.role);
    m["model"]        = jstr(c.model);
    m["goal"]         = jstr(c.goal);
    m["brevity"]      = jstr(brevity_s(c.brevity));
    m["max_tokens"]   = jnum(static_cast<double>(c.max_tokens));
    m["temperature"]  = jnum(c.temperature);
    if (!c.advisor_model.empty()) m["advisor_model"] = jstr(c.advisor_model);
    if (!c.mode.empty())          m["mode"]          = jstr(c.mode);
    if (!c.personality.empty())   m["personality"]   = jstr(c.personality);
    if (!c.rules.empty()) {
        auto arr = jarr();
        for (auto& r : c.rules) arr->as_array_mut().push_back(jstr(r));
        m["rules"] = arr;
    }
    if (!c.capabilities.empty()) {
        auto arr = jarr();
        for (auto& x : c.capabilities) arr->as_array_mut().push_back(jstr(x));
        m["capabilities"] = arr;
    }
    return o;
}

// Fire up a disposable orchestrator with no agents loaded.  Used for
// reflection of the master `index` agent only — the API does not read
// .json definitions from disk.  Inline agents live for the duration of
// one request; the catalog endpoints below don't see them.
std::unique_ptr<Orchestrator> make_reflect_orchestrator(const ApiServerOptions& opts) {
    return std::make_unique<Orchestrator>(opts.api_keys);
}

// Forward decl — defined further down (shared with the memory file
// scratchpad path-safety check).
bool agent_id_is_safe(const std::string& id);

// Render a stored AgentRecord as the same JSON shape as the built-in
// `index` master, plus `created_at`/`updated_at` so the front-end can
// show a "last edited" hint.  We re-parse `agent_def_json` through
// Constitution::from_json so the response always reflects the canonical
// blob — and so a stored row whose blob has somehow diverged from its
// denormalised columns can't lie to the caller.
std::shared_ptr<JsonValue> agent_record_to_json(const AgentRecord& a) {
    Constitution c;
    try {
        c = Constitution::from_json(a.agent_def_json);
    } catch (...) {
        // Defensive: the only way to land here is a blob that passed
        // validation at write time but fails today (schema drift after
        // an upgrade, manual DB poke).  Surface the row metadata so the
        // caller can still find and replace it.
        auto o = jobj();
        auto& m = o->as_object_mut();
        m["id"]              = jstr(a.agent_id);
        m["name"]            = jstr(a.name);
        m["role"]            = jstr(a.role);
        m["model"]           = jstr(a.model);
        m["created_at"]      = jnum(static_cast<double>(a.created_at));
        m["updated_at"]      = jnum(static_cast<double>(a.updated_at));
        m["agent_def_raw"]   = jstr(a.agent_def_json);
        m["error"]           = jstr("stored agent_def fails validation — "
                                     "PATCH a fresh blob to repair");
        return o;
    }
    auto o = constitution_to_json(a.agent_id, c);
    auto& m = o->as_object_mut();
    m["created_at"] = jnum(static_cast<double>(a.created_at));
    m["updated_at"] = jnum(static_cast<double>(a.updated_at));
    return o;
}

void handle_agents_list(int fd, const ApiServerOptions& opts,
                         TenantStore& tenants, const Tenant& tenant) {
    // List the tenant's stored agents + the built-in `index` master.
    // `index` is always first so callers never have to special-case
    // its absence in their UI.
    std::unique_ptr<Orchestrator> orch;
    try { orch = make_reflect_orchestrator(opts); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr(std::string("orchestrator init failed: ") + e.what());
        write_json_response(fd, 500, err);
        return;
    }

    auto arr = jarr();
    auto& a = arr->as_array_mut();
    try {
        a.push_back(constitution_to_json("index", orch->get_constitution("index")));
    } catch (...) { /* index master always exists; defensive */ }

    for (auto& rec : tenants.list_agent_records(tenant.id, /*limit=*/200)) {
        a.push_back(agent_record_to_json(rec));
    }

    auto body = jobj();
    auto& m = body->as_object_mut();
    m["agents"] = arr;
    m["count"]  = jnum(static_cast<double>(a.size()));
    write_json_response(fd, 200, body);
}

void handle_agent_get(int fd, const std::string& agent_id,
                       const ApiServerOptions& opts,
                       TenantStore& tenants, const Tenant& tenant) {
    // The built-in master short-circuits — every tenant sees the same
    // `index` constitution, so we don't store it per-tenant.
    if (agent_id == "index") {
        std::unique_ptr<Orchestrator> orch;
        try { orch = make_reflect_orchestrator(opts); }
        catch (const std::exception& e) {
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr(std::string("orchestrator init failed: ") + e.what());
            write_json_response(fd, 500, err);
            return;
        }
        try {
            auto& c = orch->get_constitution("index");
            write_json_response(fd, 200, constitution_to_json("index", c));
            return;
        } catch (const std::out_of_range&) {
            auto err = jobj();
            err->as_object_mut()["error"] = jstr("index master missing");
            write_json_response(fd, 500, err);
            return;
        }
    }

    auto rec = tenants.get_agent_record(tenant.id, agent_id);
    if (!rec) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("no agent '" + agent_id + "' for this tenant");
        write_json_response(fd, 404, err);
        return;
    }
    write_json_response(fd, 200, agent_record_to_json(*rec));
}

// ─── A2A protocol HTTP handlers ────────────────────────────────────────────
//
// The pure transforms (build_agent_card, build_well_known_stub,
// resolve_public_base_url) live in src/a2a/server.cpp.  These wrappers
// are TU-local because they touch the orchestrator factory + tenant
// store, both of which already live in this translation unit.

void handle_a2a_well_known_card(int fd, const HttpRequest& req,
                                 const ApiServerOptions& opts) {
    const std::string base = a2a::resolve_public_base_url(opts, req.headers);
    auto card = a2a::build_well_known_stub(base);
    write_json_response(fd, 200, a2a::to_json(card));
}

void handle_a2a_agent_card_get(int fd, const std::string& agent_id,
                                const HttpRequest& req,
                                const ApiServerOptions& opts,
                                TenantStore& tenants, const Tenant& tenant) {
    const std::string base = a2a::resolve_public_base_url(opts, req.headers);

    // The built-in master is tenant-agnostic and lives in the orchestrator
    // factory — it doesn't have a row in tenant_agents, so we resolve it
    // through the same path GET /v1/agents/index uses.
    if (agent_id == "index") {
        std::unique_ptr<Orchestrator> orch;
        try { orch = make_reflect_orchestrator(opts); }
        catch (const std::exception& e) {
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr(std::string("orchestrator init failed: ") + e.what());
            write_json_response(fd, 500, err);
            return;
        }
        try {
            auto& cons = orch->get_constitution("index");
            auto card  = a2a::build_agent_card(cons, "index", base, "index");
            write_json_response(fd, 200, a2a::to_json(card));
            return;
        } catch (const std::out_of_range&) {
            auto err = jobj();
            err->as_object_mut()["error"] = jstr("index master missing");
            write_json_response(fd, 500, err);
            return;
        }
    }

    auto rec = tenants.get_agent_record(tenant.id, agent_id);
    if (!rec) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("no agent '" + agent_id + "' for this tenant");
        write_json_response(fd, 404, err);
        return;
    }

    Constitution cons;
    try {
        cons = Constitution::from_json(rec->agent_def_json);
    } catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr(std::string("agent_def parse failed: ") + e.what());
        write_json_response(fd, 500, err);
        return;
    }

    // Version threads the catalog row's updated_at so card consumers can
    // cheap-cache.  Clients re-fetch on a version mismatch.
    auto card = a2a::build_agent_card(cons, agent_id, base,
                                       std::to_string(rec->updated_at));
    write_json_response(fd, 200, a2a::to_json(card));
}

// A2A JSON-RPC dispatch lives below `InFlightScope` (defined later in
// this TU) — see "── A2A JSON-RPC dispatch" further down.

// Validate an inbound agent_def body and pull out the (id, name, role,
// model, canonical JSON) tuple we persist.  Wraps Constitution::from_json
// so callers get a clean 400 on shape errors; returns std::nullopt on
// failure with `err_msg` populated.  `enforce_id` is the path :id when
// the route mandates one (PATCH); empty for POST where the body owns
// the id.
struct ParsedAgentDef {
    std::string agent_id;
    std::string name;
    std::string role;
    std::string model;
    std::string canonical_json;
};
std::optional<ParsedAgentDef>
parse_agent_def_body(const HttpRequest& req,
                      const std::string& enforce_id,
                      std::string& err_msg) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        err_msg = std::string("invalid JSON: ") + e.what();
        return std::nullopt;
    }
    if (!body || !body->is_object()) {
        err_msg = "body must be a JSON object containing an agent_def";
        return std::nullopt;
    }
    // Accept either `{ ...constitution... }` directly or `{ "agent_def": {...} }`
    // for symmetry with the /v1/orchestrate request shape.
    std::shared_ptr<JsonValue> def = body;
    if (auto v = body->get("agent_def"); v && v->is_object()) def = v;

    const std::string body_id = def->get_string("id", "");
    if (!enforce_id.empty()) {
        if (!body_id.empty() && body_id != enforce_id) {
            err_msg = "agent_def.id (\"" + body_id + "\") does not match path :id (\""
                      + enforce_id + "\")";
            return std::nullopt;
        }
    } else if (body_id.empty()) {
        err_msg = "agent_def.id is required (caller-chosen identifier; reused on every reference)";
        return std::nullopt;
    }
    const std::string agent_id = enforce_id.empty() ? body_id : enforce_id;
    if (agent_id == "index") {
        err_msg = "'index' is reserved for the built-in master and cannot be stored per-tenant";
        return std::nullopt;
    }
    // Same id sanity bounds as the memory file path — keeps stored ids
    // routable through every existing slash-command and URL surface
    // without an extra escaping layer.
    if (!agent_id_is_safe(agent_id)) {
        err_msg = "agent_def.id must be 1..64 chars, [A-Za-z0-9_-], not starting with '.' or '/'";
        return std::nullopt;
    }

    ParsedAgentDef p;
    p.agent_id       = agent_id;
    p.canonical_json = json_serialize(*def);

    try {
        Constitution c = Constitution::from_json(p.canonical_json);
        p.name  = c.name;
        p.role  = c.role;
        p.model = c.model;
    } catch (const std::exception& e) {
        err_msg = std::string("invalid agent_def: ") + e.what();
        return std::nullopt;
    }
    return p;
}

void handle_agent_create(int fd, const HttpRequest& req,
                          TenantStore& tenants, const Tenant& tenant) {
    std::string err;
    auto p = parse_agent_def_body(req, /*enforce_id=*/"", err);
    if (!p) {
        auto e = jobj();
        e->as_object_mut()["error"] = jstr(err);
        write_json_response(fd, 400, e);
        return;
    }

    auto created = tenants.create_agent_record(tenant.id, p->agent_id,
                                                p->name, p->role, p->model,
                                                p->canonical_json);
    if (!created) {
        // Conflict — surface the existing row so the caller can decide
        // whether to PATCH or pick a different id.
        auto existing = tenants.get_agent_record(tenant.id, p->agent_id);
        auto e = jobj();
        auto& m = e->as_object_mut();
        m["error"] = jstr("agent '" + p->agent_id + "' already exists for this tenant");
        if (existing) m["existing"] = agent_record_to_json(*existing);
        write_json_response(fd, 409, e);
        return;
    }
    write_json_response(fd, 201, agent_record_to_json(*created));
}

void handle_agent_patch(int fd, const std::string& agent_id,
                         const HttpRequest& req,
                         TenantStore& tenants, const Tenant& tenant) {
    if (agent_id == "index") {
        auto e = jobj();
        e->as_object_mut()["error"] =
            jstr("'index' is the built-in master and cannot be modified");
        write_json_response(fd, 400, e);
        return;
    }
    if (!tenants.get_agent_record(tenant.id, agent_id)) {
        auto e = jobj();
        e->as_object_mut()["error"] =
            jstr("no agent '" + agent_id + "' for this tenant");
        write_json_response(fd, 404, e);
        return;
    }
    std::string err;
    auto p = parse_agent_def_body(req, /*enforce_id=*/agent_id, err);
    if (!p) {
        auto e = jobj();
        e->as_object_mut()["error"] = jstr(err);
        write_json_response(fd, 400, e);
        return;
    }
    if (!tenants.update_agent_record(tenant.id, agent_id, p->name, p->role,
                                      p->model, p->canonical_json)) {
        // Race: row vanished between existence check and update.  Treat
        // as 404 — caller can re-POST to recreate.
        auto e = jobj();
        e->as_object_mut()["error"] = jstr("agent disappeared mid-request; re-create");
        write_json_response(fd, 404, e);
        return;
    }
    auto fresh = tenants.get_agent_record(tenant.id, agent_id);
    write_json_response(fd, 200, agent_record_to_json(*fresh));
}

void handle_agent_delete(int fd, const std::string& agent_id,
                          TenantStore& tenants, const Tenant& tenant) {
    if (agent_id == "index") {
        auto e = jobj();
        e->as_object_mut()["error"] =
            jstr("'index' is the built-in master and cannot be deleted");
        write_json_response(fd, 400, e);
        return;
    }
    if (!tenants.delete_agent_record(tenant.id, agent_id)) {
        auto e = jobj();
        e->as_object_mut()["error"] =
            jstr("no agent '" + agent_id + "' for this tenant");
        write_json_response(fd, 404, e);
        return;
    }
    auto body = jobj();
    body->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, body);
}

// Memory file path for an agent within a tenant's memory directory.  Agent
// ids go through sanitization because they become path components — no
// traversal, no absolute paths, no hidden files.  Mirrors cmd_mem_*'s
// naming convention so /v1/memory/:id returns exactly what /mem read would
// have surfaced to the agent mid-turn.
bool agent_id_is_safe(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    if (id[0] == '.' || id[0] == '/') return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string tenant_memory_dir(const ApiServerOptions& opts, const Tenant& t) {
    if (opts.memory_root.empty()) return {};
    return opts.memory_root + "/t" + std::to_string(t.id);
}

// Read entire file into a string.  Returns nullopt if the file doesn't
// exist; throws only on IO errors we can't recover from (permission,
// mid-read corruption).  Size cap prevents a runaway /mem write from OOM'ing
// the response path — 4 MiB is plenty for a markdown note log.
std::optional<std::string> read_small_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size > 4 * 1024 * 1024) size = 4 * 1024 * 1024;
    f.seekg(0);
    std::string out(static_cast<size_t>(size), '\0');
    f.read(out.data(), size);
    out.resize(static_cast<size_t>(f.gcount()));
    return out;
}

void handle_memory_list(int fd, const ApiServerOptions& /*opts*/,
                         TenantStore& tenants, const Tenant& tenant) {
    // Scratchpads now live in `agent_scratchpad`, not on the filesystem.
    // We surface them with the same shape the legacy file-listing
    // produced (agent_id + kind + size) so the front-end's renderer
    // doesn't need a parallel code path.
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& scope : tenants.list_scratchpad_scopes(tenant.id)) {
        auto entry = jobj();
        auto& m = entry->as_object_mut();
        m["agent_id"] = jstr(scope);    // "" for the shared scratchpad
        m["kind"]     = jstr(scope.empty() ? "shared" : "agent");
        const std::string content = tenants.read_scratchpad(tenant.id, scope);
        m["size"]     = jnum(static_cast<double>(content.size()));
        a.push_back(std::move(entry));
    }
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["tenant_id"] = jnum(static_cast<double>(tenant.id));
    m["entries"]   = arr;
    m["count"]     = jnum(static_cast<double>(a.size()));
    write_json_response(fd, 200, body);
}

// ─── Conversations ──────────────────────────────────────────────────────────

std::shared_ptr<JsonValue> conversation_to_json(const Conversation& c) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]            = jnum(static_cast<double>(c.id));
    m["tenant_id"]     = jnum(static_cast<double>(c.tenant_id));
    m["title"]         = jstr(c.title);
    m["agent_id"]      = jstr(c.agent_id);
    m["created_at"]    = jnum(static_cast<double>(c.created_at));
    m["updated_at"]    = jnum(static_cast<double>(c.updated_at));
    m["message_count"] = jnum(static_cast<double>(c.message_count));
    m["archived"]      = jbool(c.archived);
    if (!c.agent_def_json.empty()) {
        // Re-parse so it serializes as nested JSON, not an escaped string.
        try {
            m["agent_def"] = json_parse(c.agent_def_json);
        } catch (...) {
            m["agent_def_raw"] = jstr(c.agent_def_json);
        }
    }
    return o;
}

std::shared_ptr<JsonValue>
conversation_message_to_json(const ConversationMessage& m) {
    auto o = jobj();
    auto& obj = o->as_object_mut();
    obj["id"]              = jnum(static_cast<double>(m.id));
    obj["conversation_id"] = jnum(static_cast<double>(m.conversation_id));
    obj["role"]            = jstr(m.role);
    obj["content"]         = jstr(m.content);
    obj["input_tokens"]    = jnum(static_cast<double>(m.input_tokens));
    obj["output_tokens"]   = jnum(static_cast<double>(m.output_tokens));
    obj["created_at"]      = jnum(static_cast<double>(m.created_at));
    if (!m.request_id.empty()) obj["request_id"] = jstr(m.request_id);
    return o;
}

void handle_conversation_create(int fd, const HttpRequest& req,
                                 TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }

    const std::string title    = body->get_string("title", "");
    const std::string agent_id = body->get_string("agent_id",
                                  body->get_string("agent", "index"));
    if (!agent_id_is_safe(agent_id)) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("invalid agent_id");
        write_json_response(fd, 400, err);
        return;
    }

    // Snapshot inline agent_def into the conversation row so the thread
    // continues to work even if the caller's DB-side definition disappears.
    std::string agent_def_json;
    if (auto v = body->get("agent_def"); v && v->is_object()) {
        agent_def_json = json_serialize(*v);
    }

    auto c = tenants.create_conversation(tenant.id, title, agent_id, agent_def_json);
    write_json_response(fd, 201, conversation_to_json(c));
}

void handle_conversation_list(int fd, const HttpRequest& req,
                               TenantStore& tenants, const Tenant& tenant) {
    const auto qp = parse_query(req.path);
    auto as_int64 = [&](const std::string& k) -> int64_t {
        auto it = qp.find(k);
        if (it == qp.end()) return 0;
        try { return std::stoll(it->second); } catch (...) { return 0; }
    };
    const int64_t before = as_int64("before_updated_at");
    const int     limit  = static_cast<int>(as_int64("limit"));

    auto convs = tenants.list_conversations(tenant.id, before, limit);
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& c : convs) a.push_back(conversation_to_json(c));
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["conversations"] = arr;
    m["count"]         = jnum(static_cast<double>(convs.size()));
    write_json_response(fd, 200, body);
}

void handle_conversation_get(int fd, int64_t id,
                              TenantStore& tenants, const Tenant& tenant) {
    auto c = tenants.get_conversation(tenant.id, id);
    if (!c) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("conversation not found");
        write_json_response(fd, 404, err);
        return;
    }
    write_json_response(fd, 200, conversation_to_json(*c));
}

void handle_conversation_patch(int fd, int64_t id, const HttpRequest& req,
                                TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }

    std::string new_title;
    if (auto v = body->get("title"); v && v->is_string()) new_title = v->as_string();

    int set_archived = -1;
    if (auto v = body->get("archived"); v && v->is_bool()) {
        set_archived = v->as_bool() ? 1 : 0;
    }

    if (!tenants.update_conversation(tenant.id, id, new_title, set_archived)) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("conversation not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto c = tenants.get_conversation(tenant.id, id);
    write_json_response(fd, 200, conversation_to_json(*c));
}

void handle_conversation_delete(int fd, int64_t id,
                                 TenantStore& tenants, const Tenant& tenant) {
    if (!tenants.delete_conversation(tenant.id, id)) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("conversation not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto body = jobj();
    body->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, body);
}

void handle_conversation_messages(int fd, int64_t id, const HttpRequest& req,
                                   TenantStore& tenants, const Tenant& tenant) {
    auto conv = tenants.get_conversation(tenant.id, id);
    if (!conv) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("conversation not found");
        write_json_response(fd, 404, err);
        return;
    }

    const auto qp = parse_query(req.path);
    auto as_int64 = [&](const std::string& k) -> int64_t {
        auto it = qp.find(k);
        if (it == qp.end()) return 0;
        try { return std::stoll(it->second); } catch (...) { return 0; }
    };
    const int64_t after = as_int64("after_id");
    const int     limit = static_cast<int>(as_int64("limit"));

    auto msgs = tenants.list_messages(tenant.id, id, after, limit);
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& m : msgs) a.push_back(conversation_message_to_json(m));
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["conversation_id"] = jnum(static_cast<double>(id));
    m["messages"]        = arr;
    m["count"]           = jnum(static_cast<double>(msgs.size()));
    write_json_response(fd, 200, body);
}

void handle_models_list(int fd) {
    // Static catalog of model ids the orchestrator can route to, paired
    // with the provider that handles them.  Pricing is intentionally
    // absent — the billing service's rate card is the source of truth for
    // billing-grade numbers; the runtime only needs to know what
    // routes to what provider.
    struct ModelEntry { const char* id; const char* provider; };
    static constexpr ModelEntry kModels[] = {
        // Anthropic Claude
        {"claude-opus-4-7",          "anthropic"},
        {"claude-opus-4-6",          "anthropic"},
        {"claude-opus-4-5",          "anthropic"},
        {"claude-sonnet-4-6",        "anthropic"},
        {"claude-sonnet-4-5",        "anthropic"},
        {"claude-haiku-4-5",         "anthropic"},
        // OpenAI
        {"openai/gpt-5.4",           "openai"},
        {"openai/gpt-4.1",           "openai"},
        {"openai/gpt-4o",            "openai"},
        {"openai/gpt-4o-mini",       "openai"},
        {"openai/o4-mini",           "openai"},
        // Google Gemini
        {"gemini/gemini-2.5-pro",        "gemini"},
        {"gemini/gemini-2.5-flash",      "gemini"},
        {"gemini/gemini-2.5-flash-lite", "gemini"},
        {"gemini/gemini-2.0-flash",      "gemini"},
    };

    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& m : kModels) {
        auto entry = jobj();
        auto& o = entry->as_object_mut();
        o["id"]       = jstr(m.id);
        o["provider"] = jstr(m.provider);
        a.push_back(std::move(entry));
    }
    auto body = jobj();
    body->as_object_mut()["models"] = arr;
    body->as_object_mut()["count"]  = jnum(static_cast<double>(a.size()));
    write_json_response(fd, 200, body);
}

void handle_memory_read(int fd, const std::string& agent_id,
                         TenantStore& tenants, const Tenant& tenant) {
    // Path-shape: GET /v1/memory/:agent_id (any segment), or
    //             GET /v1/memory/shared    (legacy shared alias).
    // We accept "shared" as a special agent id for the shared scratchpad
    // — keeps the front-end URL stable while the storage now lives in
    // the agent_scratchpad table under scope_key="".
    const std::string scope = (agent_id == "shared") ? std::string{} : agent_id;
    if (!scope.empty() && !agent_id_is_safe(scope)) {
        auto e = jobj();
        e->as_object_mut()["error"] = jstr("invalid agent id");
        write_json_response(fd, 400, e);
        return;
    }
    const std::string content = tenants.read_scratchpad(tenant.id, scope);
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["agent_id"] = jstr(scope);
    m["kind"]     = jstr(scope.empty() ? "shared" : "agent");
    m["content"]  = jstr(content);
    m["size"]     = jnum(static_cast<double>(content.size()));
    m["exists"]   = jbool(!content.empty());
    write_json_response(fd, 200, body);
}

// ─── Structured memory: entries + relations ─────────────────────────────────
//
// Backs the frontend graph UI.  Distinct from the file-scratchpad endpoints
// above — these store typed nodes (entries) and directed labeled edges
// (relations) in SQLite, with full CRUD over HTTP.  The two surfaces don't
// share storage: an entry is not a parsed agent scratchpad and an agent's
// `/mem write` does not create entries.

// Closed enums.  Adding/removing a value here is a frontend+API coordinated
// change — keep this list in sync with arbiter.run's seed data.
bool memory_entry_type_is_valid(const std::string& t) {
    return t == "user" || t == "feedback" || t == "project" ||
           t == "reference" || t == "learning" || t == "context";
}

// Lightweight question-intent classifier.  Returns a list of memory-entry
// types to *boost* (not filter on) when the caller hasn't supplied an
// explicit `type=` filter.  Boosting goes through the existing
// list_entries machinery (1.3x BM25 multiplier — see kTypeBoost in
// tenant_store.cpp), so a typed entry with a comparable lexical match
// surfaces above an untyped one when intent agrees.
//
// Intentionally regex-free.  The cues are short, common, and case-folded
// — substring matching on a normalized query is fast and gives the
// caller no new dependencies.  We bias toward false negatives: a query
// that doesn't match any cue keeps the original (no-boost) behavior,
// which is monotonic vs. pre-routing.
std::vector<std::string>
classify_question_intent(const std::string& q) {
    if (q.empty()) return {};

    std::string norm;
    norm.reserve(q.size() + 2);
    norm.push_back(' ');
    for (char c : q) {
        norm.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    norm.push_back(' ');

    auto has = [&](const char* needle) {
        return norm.find(needle) != std::string::npos;
    };

    std::vector<std::string> types;
    auto push_unique = [&](std::string t) {
        for (auto& x : types) if (x == t) return;
        types.push_back(std::move(t));
    };

    // Preference / personal-state cues — questions about what the user
    // likes, prefers, or feels about something.  These map to the
    // `user` type (memory-of-user) and `feedback` (corrections,
    // opinions the user expressed).
    if (has(" favorite ")     || has(" prefer ")  ||
        has(" preference ")   || has(" likes ")   || has(" liked ") ||
        has(" love ")         || has(" loved ")   || has(" hate ")  ||
        has(" hated ")        || has(" enjoy ")   || has(" enjoys ") ||
        has(" dislike ")      || has(" wants ")   || has(" wanted ")) {
        push_unique("user");
        push_unique("feedback");
    }

    // Reference / lookup cues — identity/location/specification
    // questions.  Reference material (names, URLs, specs, contact
    // info) lives in the `reference` type.
    if (has(" what is ")  || has(" what's ")   || has(" where is ") ||
        has(" where's ")  || has(" who is ")   || has(" who's ")    ||
        has(" how many ") || has(" how much ") || has(" url ")      ||
        has(" address ")  || has(" endpoint ") || has(" port ")) {
        push_unique("reference");
    }

    // Learning / process cues — questions about how to do something or
    // what was learned.  Maps to the `learning` type (didactic notes,
    // resolved questions, captured methodology).
    if (has(" how to ")  || has(" how do ")    || has(" how does ") ||
        has(" why is ")  || has(" why does ")  || has(" explain ")  ||
        has(" tutorial ")|| has(" guide ")) {
        push_unique("learning");
    }

    // Temporal / event cues — when/before/after/during questions.
    // Project entries are the typical home for dated events
    // (decisions, commits, incidents, milestones).
    if (has(" when ")     || has(" before ")  || has(" after ")    ||
        has(" since ")    || has(" until ")   || has(" first ")    ||
        has(" earliest ") || has(" latest ")  || has(" recent ")   ||
        has(" recently ") || has(" ago ")     || has(" yesterday ") ||
        has(" today ")    || has(" did ")     || has(" happened ")) {
        push_unique("project");
    }

    return types;
}

bool memory_relation_is_valid(const std::string& r) {
    return r == "relates_to" || r == "refines" || r == "contradicts" ||
           r == "supersedes" || r == "supports";
}

// Validate that a JsonValue is an array of strings, each ≤ 64 chars, with
// ≤ 32 elements.  Returns the canonical re-serialized form on success
// (never the user's exact bytes, so we don't store odd whitespace).  On
// failure returns nullopt and writes a reason into `err`.
std::optional<std::string>
canonical_tags_json(const std::shared_ptr<JsonValue>& v, std::string& err) {
    if (!v) return std::string("[]");
    if (!v->is_array()) { err = "tags must be a JSON array"; return std::nullopt; }
    const auto& arr = v->as_array();
    if (arr.size() > 32) { err = "tags: at most 32 entries"; return std::nullopt; }
    auto out = jarr();
    auto& a = out->as_array_mut();
    for (auto& el : arr) {
        if (!el || !el->is_string()) {
            err = "tags entries must be strings"; return std::nullopt;
        }
        const std::string& s = el->as_string();
        if (s.empty() || s.size() > 64) {
            err = "tag length must be 1..64 chars"; return std::nullopt;
        }
        a.push_back(jstr(s));
    }
    return json_serialize(*out);
}

// Lightweight logger for the memory handlers.  Writes one timestamped
// line per call to stderr — picked up by `arbiter --api --verbose` in
// the same stream as the orchestrate-side log_error closure.  Always-on
// (not gated by verbose) since these events are infrequent and any
// segfault investigation needs the breadcrumbs unconditionally.
void log_memory_event(const std::string& tag,
                       int64_t tenant_id,
                       const std::string& detail) {
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "[%s] [memory] tenant=%lld %s: %s\n",
                  ts, static_cast<long long>(tenant_id),
                  tag.c_str(), detail.c_str());
    std::fflush(stderr);
}

// ─── Rerank helper ──────────────────────────────────────────────────────────
//
// Reorders FTS candidates using a one-shot advisor LLM call.  The advisor
// callable abstracts the only thing that varies between paths:
//   • Agent-side `/mem search --rerank` builds it via Orchestrator's
//     make_advisor_invoker(caller_id), which resolves the calling agent's
//     advisor_model and routes cost attribution through cost_cb_.
//   • HTTP `GET /v1/memory/entries?rerank=<model>` builds it as a per-
//     request lambda using ApiClient + opts.api_keys + the explicit model
//     from the query string.
//
// The advisor returns either the model's reply text or "ERR: <reason>"
// on failure (model not configured, transport error, etc.); rerank
// gracefully falls back to FTS order in either case.

struct RerankResult {
    std::vector<MemoryEntry> entries;   // reordered (or original on fallback)
    std::string              note;      // empty on success
    bool                     applied = false;
};

// Type of an advisor invocation: prompt → response, where ERR-prefixed
// responses signal transport / model failure (to be handled gracefully
// by the caller, never crashing the search path).
using AdvisorFn = std::function<std::string(const std::string&)>;

// Build an advisor invoker that targets `model` and uses `sys_prompt`.
// Captures `opts_keys` by value so the returned closure can outlive the
// caller's local frame.  Centralized here so the rerank, expand, and
// any future advisor-driven memory features share one code path.
AdvisorFn build_memory_advisor(
    const std::map<std::string, std::string>& opts_keys,
    const std::string& model,
    const std::string& sys_prompt) {
    return [opts_keys, model, sys_prompt]
           (const std::string& prompt) -> std::string {
        ApiClient client(opts_keys);
        ApiRequest r;
        r.model               = model;
        r.max_tokens          = 1024;
        r.include_temperature = false;
        r.system_prompt       = sys_prompt;
        r.messages            = {{"user", prompt}};
        ApiResponse resp = client.complete(r);
        if (!resp.ok) return "ERR: " + resp.error;
        return resp.content;
    };
}

// One-shot query reformulation.  Returns up to `max_paraphrases`
// alternative phrasings of `query`; never returns the original.  On
// advisor error or unparseable output, returns an empty list — the
// caller falls back to the un-expanded query without surfacing the
// failure as a hard error.
//
// The system prompt the caller provides should pin a strict output
// format ("one paraphrase per line, no numbering, no commentary").
// We split on newlines and trim; lines shorter than 3 chars or
// matching the original query verbatim are dropped.
struct ExpansionResult {
    std::vector<std::string> queries;  // paraphrases only
    std::string              note;     // empty on success; reason on fail
};
ExpansionResult expand_query_with_advisor(
    const AdvisorFn& advisor,
    const std::string& query,
    size_t max_paraphrases = 2) {
    ExpansionResult out;
    if (query.empty()) return out;

    std::ostringstream prompt;
    prompt << "Original query: " << query << "\n\n"
           << "Produce " << max_paraphrases
           << " alternative phrasings of this query. Each phrasing "
              "should preserve the user's intent but vary the wording — "
              "use synonyms, different sentence structure, or restate "
              "from a different angle. The phrasings will be used as "
              "additional search queries against a memory index, so "
              "lexical diversity is the goal. Output the phrasings "
              "one per line, with no numbering, no quotes, no "
              "commentary, no preamble.";

    std::string resp = advisor(prompt.str());
    if (resp.size() >= 4 && resp.compare(0, 4, "ERR:") == 0) {
        out.note = "(query expansion advisor unavailable:" +
                   resp.substr(4) + " — searching original only)";
        return out;
    }

    // Split on \n, trim, dedupe-against-original, cap at max_paraphrases.
    auto trim = [](std::string s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };

    size_t i = 0;
    while (i < resp.size() && out.queries.size() < max_paraphrases) {
        size_t nl = resp.find('\n', i);
        std::string line =
            trim(resp.substr(i, nl == std::string::npos
                                  ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? resp.size() : nl + 1;
        if (line.size() < 3) continue;
        // Strip leading "1. " / "- " / "* " in case the model ignores
        // the no-numbering instruction.
        if (line.size() > 2 &&
            (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            line = trim(line.substr(2));
        } else if (line.size() > 3 &&
                   std::isdigit(static_cast<unsigned char>(line[0])) &&
                   (line[1] == '.' || line[1] == ')') && line[2] == ' ') {
            line = trim(line.substr(3));
        }
        if (line.empty() || line == query) continue;
        bool dup = false;
        for (auto& q : out.queries) if (q == line) { dup = true; break; }
        if (!dup) out.queries.push_back(line);
    }

    if (out.queries.empty()) {
        out.note = "(query expansion produced no usable paraphrases — "
                   "searching original only)";
    }
    return out;
}

// One-shot tag extraction.  Asks the advisor for 2..max_tags concise
// tags describing the entry's title + content.  Returns lowercase,
// hyphenated tokens already de-duplicated against `existing_tags`.  On
// advisor failure or unparseable output, returns an empty list — the
// entry is created with the caller-supplied tags untouched.
//
// The output format (one tag per line, lowercase, hyphenated) is
// constrained tightly so the parser doesn't have to second-guess
// arbitrary punctuation; this keeps the runtime predictable across
// model versions.  Tags get the existing 8x BM25 weight on retrieval,
// so a clean tag set is a much stronger signal than a noisy one.
struct TagExtractionResult {
    std::vector<std::string> tags;
    std::string              note;
};
TagExtractionResult extract_tags_with_advisor(
    const AdvisorFn& advisor,
    const std::string& title,
    const std::string& content,
    const std::vector<std::string>& existing_tags,
    size_t max_tags = 4) {
    TagExtractionResult out;

    std::ostringstream prompt;
    prompt << "Extract " << max_tags
           << " concise tags describing this memory entry. Tags should "
              "be lowercase, hyphenated where multi-word, ≤32 characters, "
              "and capture the *topic* — not the speech act. Examples of "
              "good tags: 'sushi', 'machine-learning', 'tokyo-trip', "
              "'budget'. Examples of bad tags: 'the-user-said', 'a-fact', "
              "'general'. Output one tag per line, no numbering, no "
              "quotes, no commentary.\n\n"
           << "Title: " << title << "\n";
    if (!content.empty()) {
        std::string excerpt = content;
        if (excerpt.size() > 1500) excerpt.resize(1500);
        for (auto& c : excerpt) if (c == '\n') c = ' ';
        prompt << "Content: " << excerpt << "\n";
    }

    std::string resp = advisor(prompt.str());
    if (resp.size() >= 4 && resp.compare(0, 4, "ERR:") == 0) {
        out.note = "(auto-tag advisor unavailable:" + resp.substr(4) +
                   " — using caller tags only)";
        return out;
    }

    // Existing-tag set for dedupe — case-insensitive.
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::unordered_map<std::string, char> seen;
    for (auto& t : existing_tags) seen[lower(t)] = 1;

    auto trim = [](std::string s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };

    size_t i = 0;
    while (i < resp.size() && out.tags.size() < max_tags) {
        size_t nl = resp.find('\n', i);
        std::string line = trim(resp.substr(
            i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? resp.size() : nl + 1;
        if (line.empty()) continue;
        // Strip common list prefixes the model may add despite the
        // instruction (- foo, * foo, 1. foo).
        if (line.size() > 2 &&
            (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            line = trim(line.substr(2));
        } else if (line.size() > 3 &&
                   std::isdigit(static_cast<unsigned char>(line[0])) &&
                   (line[1] == '.' || line[1] == ')') && line[2] == ' ') {
            line = trim(line.substr(3));
        }

        // Lowercase + filter to alphanumeric/hyphen.  Anything else
        // (quotes, parentheses, punctuation) is dropped — keeps the
        // tag column clean even when the model ignores formatting.
        std::string tag;
        tag.reserve(line.size());
        for (char c : line) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc)) tag.push_back(static_cast<char>(std::tolower(uc)));
            else if (c == '-' || c == ' ') tag.push_back('-');
        }
        // Collapse runs of '-' and trim leading/trailing.
        std::string compact;
        compact.reserve(tag.size());
        char prev = 0;
        for (char c : tag) {
            if (c == '-' && prev == '-') continue;
            compact.push_back(c);
            prev = c;
        }
        while (!compact.empty() && compact.front() == '-') compact.erase(0, 1);
        while (!compact.empty() && compact.back()  == '-') compact.pop_back();

        if (compact.size() < 2 || compact.size() > 32) continue;
        if (seen.count(compact)) continue;
        seen[compact] = 1;
        out.tags.push_back(std::move(compact));
    }

    if (out.tags.empty() && out.note.empty()) {
        out.note = "(auto-tag produced no usable tags)";
    }
    return out;
}

// Detect supersession: when a new entry asserts a fact that *replaces*
// the truth of an older entry on the same subject, return the older
// entry's id so the caller can call invalidate_entry() on it.
//
// We do not auto-invalidate as a side-effect of search.  The mutation
// belongs to the entry-create code path because it ties cleanly to a
// user action ("the user just told me X is now true") and respects the
// same opt-in surface as auto-tagging.  A false positive here erases
// (well, soft-deletes) prior memory; the prompt is deliberately strict
// to bias toward "leave it alone" on ambiguous cases.
//
// `candidates` are passed in — caller is responsible for finding them
// (typically: FTS search on the new entry's title, top 5 by BM25,
// excluding the new entry itself).  Helper stays pure: it doesn't touch
// the store, it just decides which ids to flag.
struct SupersessionResult {
    std::vector<int64_t>     invalidated_ids;
    std::vector<int64_t>     candidate_ids;
    std::string              note;
};
SupersessionResult detect_supersession_with_advisor(
    const AdvisorFn& advisor,
    const std::string& new_title,
    const std::string& new_content,
    const std::vector<MemoryEntry>& candidates) {
    SupersessionResult out;
    for (auto& c : candidates) out.candidate_ids.push_back(c.id);
    if (candidates.empty()) return out;

    std::ostringstream prompt;
    prompt << "A new memory entry was just stored. Determine whether it "
              "directly contradicts any of the existing entries below — "
              "i.e., asserts a fact that REPLACES a previously-stored "
              "fact about the same subject. Examples of contradictions: "
              "'I prefer pasta' (new) vs 'I prefer sushi' (existing); "
              "'we use Postgres now' (new) vs 'we use MySQL' (existing). "
              "Mere relatedness or topical overlap does NOT count — the "
              "new entry must make the old one factually WRONG. When in "
              "doubt, prefer 'none'.\n\n"
           << "New entry:\n"
           << "  Title: " << new_title << "\n";
    if (!new_content.empty()) {
        std::string excerpt = new_content;
        if (excerpt.size() > 1500) excerpt.resize(1500);
        for (auto& c : excerpt) if (c == '\n') c = ' ';
        prompt << "  Content: " << excerpt << "\n";
    }
    prompt << "\nExisting entries:\n";
    for (auto& e : candidates) {
        prompt << "[id=" << e.id << "] " << e.title << "\n";
        if (!e.content.empty()) {
            std::string excerpt = e.content;
            if (excerpt.size() > 800) excerpt.resize(800);
            for (auto& c : excerpt) if (c == '\n') c = ' ';
            prompt << "  " << excerpt << "\n";
        }
    }
    prompt << "\nRespond with the ids of existing entries that are now "
              "factually superseded, comma-separated. If no entry is "
              "directly contradicted, respond with the single word "
              "'none'.";

    std::string resp = advisor(prompt.str());
    if (resp.size() >= 4 && resp.compare(0, 4, "ERR:") == 0) {
        out.note = "(supersession advisor unavailable:" + resp.substr(4) +
                   " — no auto-invalidation)";
        return out;
    }

    // Explicit "none" guard: model wrote no digits, meaning no
    // supersession.  We only consider the digit path otherwise; an
    // utterance like "none of these" is no-op for the digit parser
    // already, but check explicitly so the response shape is clear.
    bool has_digit = false;
    for (char c : resp) {
        if (c >= '0' && c <= '9') { has_digit = true; break; }
    }
    if (!has_digit) return out;

    // Build candidate id set for membership checks (only accept ids the
    // advisor was actually shown — no fabrication).
    std::unordered_map<int64_t, char> allowed;
    for (auto& c : candidates) allowed[c.id] = 1;

    int64_t accum = 0;
    bool in_num = false;
    auto flush = [&]() {
        if (in_num && allowed.count(accum)) {
            bool dup = false;
            for (auto x : out.invalidated_ids)
                if (x == accum) { dup = true; break; }
            if (!dup) out.invalidated_ids.push_back(accum);
        }
        accum = 0;
        in_num = false;
    };
    for (char c : resp) {
        if (c >= '0' && c <= '9') {
            accum = accum * 10 + (c - '0');
            in_num = true;
        } else {
            flush();
        }
    }
    flush();

    return out;
}

// Reciprocal-rank fusion across N rankings of MemoryEntry rows.  Each
// ranking is a 1-based ordered list (as returned by FTS5 or
// search_entries_graduated); each gets a per-ranking weight to bias the
// fusion toward more-trusted variants (e.g., the original query above
// reformulations).  k=60 is the canonical RRF constant — tunable, but
// the fusion is robust across k ∈ [40, 80].
std::vector<MemoryEntry> rrf_fuse_rankings(
    std::vector<std::vector<MemoryEntry>> rankings,
    const std::vector<double>& weights,
    int limit,
    double k = 60.0) {
    if (rankings.empty()) return {};
    if (rankings.size() == 1) {
        auto out = std::move(rankings[0]);
        if (limit > 0 && static_cast<int>(out.size()) > limit) {
            out.resize(static_cast<size_t>(limit));
        }
        return out;
    }

    // id → (entry pointer + accumulated score).  Pointer is into one of
    // the input vectors (which we own here), valid for the duration of
    // this function.  Don't move-from `rankings` until done.
    struct Row { const MemoryEntry* entry; double score; };
    std::unordered_map<int64_t, Row> fused;

    for (size_t r = 0; r < rankings.size(); ++r) {
        double w = (r < weights.size()) ? weights[r] : 1.0;
        for (size_t i = 0; i < rankings[r].size(); ++i) {
            const auto& e = rankings[r][i];
            double contrib = w / (k + static_cast<double>(i + 1));
            auto it = fused.find(e.id);
            if (it == fused.end()) fused.emplace(e.id, Row{&e, contrib});
            else                   it->second.score += contrib;
        }
    }

    std::vector<std::pair<int64_t, double>> sorted;
    sorted.reserve(fused.size());
    for (auto& [id, row] : fused) sorted.emplace_back(id, row.score);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    std::vector<MemoryEntry> out;
    out.reserve(limit > 0 ? static_cast<size_t>(limit) : sorted.size());
    for (auto& [id, _] : sorted) {
        if (limit > 0 && static_cast<int>(out.size()) >= limit) break;
        out.push_back(*fused[id].entry);
    }
    return out;
}

// Render an epoch-seconds timestamp as YYYY-MM-DD UTC, or "" if missing.
// Used in rerank prompt enrichment so the LLM can pick the most-recent
// non-superseded entry on temporal/knowledge-update questions.  We
// deliberately omit time-of-day to keep the prompt compact — day-level
// granularity is enough to break ties between memories on different
// dates, and most LongMemEval-style questions ground at day resolution.
std::string format_ts_yyyymmdd(int64_t epoch_s) {
    if (epoch_s <= 0) return {};
    std::time_t t = static_cast<std::time_t>(epoch_s);
    std::tm tm{};
    if (gmtime_r(&t, &tm) == nullptr) return {};
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm) == 0) return {};
    return std::string(buf);
}

RerankResult rerank_with_advisor(
    const std::function<std::string(const std::string&)>& advisor,
    const std::string& query,
    std::vector<MemoryEntry> candidates,
    size_t excerpt_bytes = 800) {
    if (candidates.size() <= 1) {
        return {std::move(candidates), {}, false};
    }

    // Build a structured prompt asking for comma-separated ids.
    //
    // `excerpt_bytes` controls how much of each candidate's content
    // is shown to the reranker.  Default 800 bytes — long enough that
    // the answer-bearing detail of a typical conversational turn is
    // visible (most turns are 300–800 chars), short enough that 25
    // candidates fit comfortably in any modern advisor context window
    // (~20KB total excerpt content).  Two-stage callers raise this
    // for the fine pass where the candidate set is small and the
    // model is expected to discriminate among close-scored matches.
    //
    // We ask for the *full* ranked list rather than a top-3.  The
    // parser only repositions ids it sees; ids the model omits keep
    // their original FTS position, which on a top-3 prompt meant
    // entries 4..N inherited the upstream ordering verbatim — pinning
    // R@10 to the candidate-generation R@10.  Asking for every id
    // lets the reorder reach the long tail.
    //
    // Each candidate row carries a short metadata header
    // `(type · YYYY-MM-DD · superseded)` when those fields are
    // available.  Without that, the reranker is reordering by title +
    // excerpt only — it can't pick the most-recent preference, prefer
    // a `type=preference` row over a `type=research` row that mentions
    // the same topic, or skip an explicitly invalidated entry.  Those
    // signals are exactly what knowledge-update and temporal-reasoning
    // questions hinge on.
    std::ostringstream prompt;
    prompt << "Rerank these search results by relevance to the query.\n"
           << "Each candidate header carries (type · authored-date · "
              "validity).  When ranking, prefer entries whose type "
              "matches what the query is asking for (preference, event, "
              "fact, etc.); on questions about *current* state, prefer "
              "the most recent non-superseded entry; entries marked "
              "'superseded' have been invalidated and should rank below "
              "live entries on the same topic.\n\n"
           << "Query: \"" << query << "\"\n\n"
           << "Candidates:\n";
    for (auto& e : candidates) {
        // Compose the metadata header.  Only emit fields we have so the
        // line stays terse for entries with sparse metadata.
        std::vector<std::string> meta;
        if (!e.type.empty()) meta.push_back(e.type);
        std::string ts = format_ts_yyyymmdd(e.created_at);
        if (!ts.empty()) meta.push_back(ts);
        if (e.valid_to > 0) {
            std::string inv = format_ts_yyyymmdd(e.valid_to);
            meta.push_back(inv.empty()
                ? std::string("superseded")
                : "superseded " + inv);
        }
        prompt << "[id=" << e.id << "]";
        if (!meta.empty()) {
            prompt << " (";
            for (size_t k = 0; k < meta.size(); ++k) {
                if (k) prompt << " · ";
                prompt << meta[k];
            }
            prompt << ")";
        }
        prompt << " " << e.title << "\n";
        if (!e.content.empty()) {
            std::string excerpt = e.content;
            if (excerpt.size() > excerpt_bytes) {
                excerpt.resize(excerpt_bytes);
                excerpt += "...";
            }
            for (auto& c : excerpt) if (c == '\n') c = ' ';
            prompt << "  " << excerpt << "\n";
        }
    }
    prompt << "\nReturn ALL candidate ids in order from most to least "
           << "relevant, comma-separated.  Include every id exactly "
           << "once.  Example for 4 candidates: 42,17,23,8";

    std::string resp = advisor(prompt.str());

    if (resp.size() >= 4 && resp.compare(0, 4, "ERR:") == 0) {
        // Advisor unavailable / errored — keep the FTS order, surface
        // the reason so the caller can adapt.
        return {
            std::move(candidates),
            "(rerank requested but advisor unavailable:" + resp.substr(4) +
            " — falling back to FTS order)",
            false,
        };
    }

    // Parse digit-runs out of the response.  Lenient by design: model
    // output is usually clean ("17,42,23") but may include quotes,
    // prefix ("Result:"), or trailing prose — extract the ids
    // regardless.  Only ids in the candidate set count; duplicates
    // dropped.
    std::vector<int64_t> picked;
    int64_t accum = 0;
    bool in_num = false;
    auto flush = [&]() {
        if (!in_num) return;
        for (auto& e : candidates) {
            if (e.id == accum) {
                bool seen = false;
                for (auto p2 : picked)
                    if (p2 == accum) { seen = true; break; }
                if (!seen) picked.push_back(accum);
                break;
            }
        }
        accum = 0;
        in_num = false;
    };
    for (char c : resp) {
        if (c >= '0' && c <= '9') {
            accum = accum * 10 + (c - '0');
            in_num = true;
        } else {
            flush();
        }
    }
    flush();

    if (picked.empty()) {
        return {
            std::move(candidates),
            "(rerank produced no parseable ids — falling back to FTS order)",
            false,
        };
    }

    // Reorder: picked ids first (in advisor order), then everything
    // else in original FTS order.
    std::vector<MemoryEntry> reordered;
    reordered.reserve(candidates.size());
    for (auto pid : picked) {
        for (auto& e : candidates) {
            if (e.id == pid) {
                reordered.push_back(e);
                break;
            }
        }
    }
    for (auto& e : candidates) {
        bool already = false;
        for (auto& r : reordered)
            if (r.id == e.id) { already = true; break; }
        if (!already) reordered.push_back(e);
    }
    return {std::move(reordered), "(reranked by advisor model)", true};
}

std::shared_ptr<JsonValue> memory_entry_to_json(const MemoryEntry& e) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]         = jnum(static_cast<double>(e.id));
    m["tenant_id"]  = jnum(static_cast<double>(e.tenant_id));
    m["type"]       = jstr(e.type);
    m["title"]      = jstr(e.title);
    m["content"]    = jstr(e.content);
    m["source"]     = jstr(e.source);
    // Re-parse so tags serialize as a nested array, not an escaped string.
    // Defensive fallback to [] if a malformed row ever slipped past
    // validation — never crash the read path.
    try {
        auto parsed = json_parse(e.tags_json);
        if (parsed && parsed->is_array()) m["tags"] = parsed;
        else                              m["tags"] = jarr();
    } catch (...) {
        m["tags"] = jarr();
    }
    // 0 ⇒ JSON null; positive ⇒ bare id.  List/graph endpoints surface
    // just the id so the frontend can decide whether to hydrate.
    if (e.artifact_id > 0) m["artifact_id"] = jnum(static_cast<double>(e.artifact_id));
    else                    m["artifact_id"] = jnull();
    m["created_at"] = jnum(static_cast<double>(e.created_at));
    m["updated_at"] = jnum(static_cast<double>(e.updated_at));
    // Temporal validity window.  `valid_from` is always set; `valid_to`
    // is null while the entry is active and an epoch when invalidated.
    m["valid_from"] = jnum(static_cast<double>(e.valid_from));
    if (e.valid_to > 0) m["valid_to"] = jnum(static_cast<double>(e.valid_to));
    else                m["valid_to"] = jnull();
    // Optional conversation scope.  null means "unscoped — visible from
    // any conversation"; a positive id means the entry was created in
    // that conversation's context and ranks higher under graduated
    // search there.
    if (e.conversation_id > 0)
        m["conversation_id"] = jnum(static_cast<double>(e.conversation_id));
    else
        m["conversation_id"] = jnull();
    return o;
}

// Single-entry hydration: attaches a nested `artifact` object with
// metadata when the entry has artifact_id set and the row resolves.
// Stale links (artifact deleted under us) leave `artifact_id` set but
// `artifact` omitted — caller can detect and surface "expired".
std::shared_ptr<JsonValue>
memory_entry_to_json_hydrated(const MemoryEntry& e, TenantStore& tenants) {
    auto o = memory_entry_to_json(e);
    if (e.artifact_id > 0) {
        if (auto art = tenants.get_artifact_meta(e.tenant_id, e.artifact_id)) {
            auto& m = o->as_object_mut();
            auto a = jobj();
            auto& am = a->as_object_mut();
            am["id"]              = jnum(static_cast<double>(art->id));
            am["conversation_id"] = jnum(static_cast<double>(art->conversation_id));
            am["path"]            = jstr(art->path);
            am["sha256"]          = jstr(art->sha256);
            am["mime_type"]       = jstr(art->mime_type);
            am["size"]            = jnum(static_cast<double>(art->size));
            am["created_at"]      = jnum(static_cast<double>(art->created_at));
            am["updated_at"]      = jnum(static_cast<double>(art->updated_at));
            m["artifact"] = a;
        }
    }
    return o;
}

std::shared_ptr<JsonValue> memory_relation_to_json(const MemoryRelation& r) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]         = jnum(static_cast<double>(r.id));
    m["tenant_id"]  = jnum(static_cast<double>(r.tenant_id));
    m["source_id"]  = jnum(static_cast<double>(r.source_id));
    m["target_id"]  = jnum(static_cast<double>(r.target_id));
    m["relation"]   = jstr(r.relation);
    m["created_at"] = jnum(static_cast<double>(r.created_at));
    return o;
}

void write_memory_error(int fd, int code, const std::string& msg) {
    auto e = jobj();
    e->as_object_mut()["error"] = jstr(msg);
    write_json_response(fd, code, e);
}

void handle_memory_entry_create(int fd, const HttpRequest& req,
                                 const ApiServerOptions& opts,
                                 TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        return write_memory_error(fd, 400, std::string("invalid JSON: ") + e.what());
    }
    if (!body || !body->is_object())
        return write_memory_error(fd, 400, "body must be a JSON object");

    const std::string type    = body->get_string("type");
    const std::string title   = body->get_string("title");
    const std::string content = body->get_string("content", "");
    const std::string source  = body->get_string("source", "");

    if (!memory_entry_type_is_valid(type))
        return write_memory_error(fd, 400, "invalid type");
    if (title.empty() || title.size() > 200)
        return write_memory_error(fd, 400, "title length must be 1..200 chars");
    if (content.size() > 64 * 1024)
        return write_memory_error(fd, 400, "content exceeds 64 KiB");
    if (source.size() > 200)
        return write_memory_error(fd, 400, "source exceeds 200 chars");

    std::string tags_err;
    auto tags = canonical_tags_json(body->get("tags"), tags_err);
    if (!tags) return write_memory_error(fd, 400, tags_err);

    // Optional auto-tagging.  When `auto_tag=<model>` is present in the
    // body, run an advisor pass that extracts 2-4 topical tags from the
    // title + content, dedupe-merge them with the caller-supplied tags,
    // and store the union.  Tags carry an 8x weight in the BM25 ranking
    // (memory_entries_fts), so a clean tag set is one of the strongest
    // retrieval signals available — and most agent-side ingest paths
    // currently leave it empty.  Failure is benign: the tag advisor's
    // error surfaces in the response's `auto_tag.note` field, the entry
    // is created with the caller-supplied tags only.
    std::string auto_tag_model;
    if (auto v = body->get("auto_tag"); v && v->is_string()) {
        auto_tag_model = v->as_string();
    }
    std::vector<std::string> auto_tags_added;
    std::string auto_tag_note;
    if (!auto_tag_model.empty()) {
        // Re-parse the canonicalized tags JSON to a vector<string> for
        // dedupe.  Cheap — caller tag arrays are bounded at 32 entries.
        std::vector<std::string> existing;
        try {
            if (auto parsed = json_parse(*tags); parsed && parsed->is_array()) {
                for (auto& t : parsed->as_array()) {
                    if (t && t->is_string()) existing.push_back(t->as_string());
                }
            }
        } catch (...) {}

        auto tag_advisor = build_memory_advisor(opts.api_keys,
                                                  auto_tag_model,
                                                  /*sys_prompt=*/
            "You are a tag extractor.  Read the entry and emit short, "
            "lowercase, hyphenated topical tags — one per line — that "
            "describe what the entry is about.  No commentary, no "
            "preamble, no quotes.");
        auto extr = extract_tags_with_advisor(tag_advisor, title, content,
                                                existing);
        auto_tags_added = std::move(extr.tags);
        auto_tag_note   = std::move(extr.note);

        if (!auto_tags_added.empty()) {
            // Merge: caller-supplied tags first (they own the entry's
            // canonical labels), advisor tags appended.  Re-serialize
            // through canonical_tags_json so length/count caps still
            // apply.  If the merge overflows the 32-tag cap, the
            // validator rejects with a clear error — no silent drop.
            auto merged = jarr();
            auto& ma = merged->as_array_mut();
            for (auto& t : existing) ma.push_back(jstr(t));
            for (auto& t : auto_tags_added) ma.push_back(jstr(t));
            std::string merge_err;
            auto canonical_merged = canonical_tags_json(merged, merge_err);
            if (canonical_merged) {
                tags = std::move(canonical_merged);
            } else {
                // Overflow / validation failure on merge — fall back to
                // caller tags, surface the reason as a note so the
                // caller can see what happened.
                auto_tags_added.clear();
                auto_tag_note = "(auto-tag merge rejected: " + merge_err +
                                " — used caller tags only)";
            }
        }
    }

    // Optional artifact link.  Validated against the tenant's artifact
    // catalogue here — passing a foreign tenant's id surfaces as 400
    // "artifact_id does not belong to this tenant" rather than a 500.
    int64_t artifact_id = 0;
    if (auto v = body->get("artifact_id"); v && v->is_number()) {
        artifact_id = static_cast<int64_t>(v->as_number());
        if (artifact_id < 0)
            return write_memory_error(fd, 400, "artifact_id must be ≥ 0");
        if (artifact_id > 0 &&
            !tenants.get_artifact_meta(tenant.id, artifact_id)) {
            return write_memory_error(fd, 400,
                "artifact_id does not exist for this tenant");
        }
    }

    // Optional conversation scope.  Validated tenant-side so a foreign
    // tenant's conversation id can't be smuggled in.  Pass 0 to leave
    // the entry unscoped (visible from every conversation).
    int64_t conversation_id = 0;
    if (auto v = body->get("conversation_id"); v && v->is_number()) {
        conversation_id = static_cast<int64_t>(v->as_number());
        if (conversation_id < 0)
            return write_memory_error(fd, 400, "conversation_id must be ≥ 0");
        if (conversation_id > 0 &&
            !tenants.get_conversation(tenant.id, conversation_id)) {
            return write_memory_error(fd, 400,
                "conversation_id does not exist for this tenant");
        }
    }

    // Optional `created_at` override.  Lets bench harnesses and
    // historical-import callers stamp entries with the time the fact
    // was originally observed rather than ingest time.  Without this,
    // every backfilled entry shares the ingest timestamp and temporal
    // queries ("when did the user say X?") collapse onto one moment.
    // Accepted as either epoch seconds (preferred) or epoch
    // milliseconds (auto-detected: anything > 10**12 is treated as ms).
    int64_t created_at_override = 0;
    if (auto v = body->get("created_at"); v && v->is_number()) {
        double raw = v->as_number();
        if (raw < 0)
            return write_memory_error(fd, 400, "created_at must be ≥ 0");
        int64_t cand = static_cast<int64_t>(raw);
        if (cand > 1'000'000'000'000LL) cand /= 1000;  // ms → s
        created_at_override = cand;
    }

    // Manual consolidation: an explicit list of ids the new entry
    // should supersede.  Each id gets a `supersedes` relation from
    // the new entry to the old one, and the old entry is invalidated.
    // Validated against the tenant before we create anything so a
    // bad id fails closed instead of leaving a dangling synthesis.
    std::vector<int64_t> manual_supersedes;
    if (auto v = body->get("supersedes_ids"); v && v->is_array()) {
        for (auto& item : v->as_array()) {
            if (!item || !item->is_number())
                return write_memory_error(fd, 400,
                    "supersedes_ids must be an array of positive ids");
            int64_t sid = static_cast<int64_t>(item->as_number());
            if (sid <= 0)
                return write_memory_error(fd, 400,
                    "supersedes_ids must contain only positive ids");
            if (!tenants.get_entry(tenant.id, sid))
                return write_memory_error(fd, 400,
                    "supersedes_ids: #" + std::to_string(sid) +
                    " does not exist for this tenant");
            manual_supersedes.push_back(sid);
        }
    }

    // Optional auto-supersession.  When `supersede=<model>` is in the
    // body, after creating the new entry we ask the advisor whether
    // the new entry contradicts any existing top-K FTS hits on the
    // same title; ids the advisor flags get invalidated (valid_to set
    // to now()).  This is the auto-cleanup loop for knowledge-update
    // cases where the user changes their mind without ever explicitly
    // invalidating the old entry — and it's where the
    // knowledge-update accuracy gap mostly lives.  Strictly opt-in;
    // the prompt biases toward "none" so false positives are rare.
    // Manual `supersedes_ids` wins: when the caller supplies an
    // explicit list, the auto pass is skipped (the caller already
    // knows which prior facts are stale).
    std::string supersede_model;
    if (auto v = body->get("supersede"); v && v->is_string()) {
        supersede_model = v->as_string();
    }

    auto e = tenants.create_entry(tenant.id, type, title, content, source,
                                    *tags, artifact_id, conversation_id,
                                    created_at_override);

    // Apply the manual consolidation immediately after create — any
    // invalidations land before the response goes out.
    std::vector<int64_t> manual_superseded_applied;
    for (auto sid : manual_supersedes) {
        (void)tenants.create_relation(tenant.id, e.id, sid, "supersedes");
        if (tenants.invalidate_entry(tenant.id, sid)) {
            manual_superseded_applied.push_back(sid);
        }
    }

    // Run supersession detection AFTER create.  We always want the new
    // entry to be persisted; the advisor pass is auxiliary.  When the
    // caller already supplied a manual `supersedes_ids` list, skip the
    // auto pass — explicit intent wins over inferred intent.
    std::vector<int64_t> superseded_ids;
    std::string supersede_note;
    std::vector<int64_t> supersede_candidates;
    if (!supersede_model.empty() && manual_supersedes.empty()) {
        // Find existing candidates: FTS on the new title, type-matched
        // (so 'preference' contradicts 'preference', not 'context').
        // Top 6 — keep the prompt bounded; -1 in the budget to leave
        // room for the new-entry-itself filter.
        TenantStore::EntryFilter fcand;
        fcand.q     = title;
        fcand.types = { type };
        fcand.limit = 6;
        auto cands = tenants.list_entries(tenant.id, fcand);
        // Drop the just-created row (FTS sees it because the AFTER
        // INSERT trigger wrote to memory_entries_fts before this
        // call returns).
        cands.erase(std::remove_if(cands.begin(), cands.end(),
            [&e](const MemoryEntry& x) { return x.id == e.id; }),
            cands.end());
        if (cands.size() > 5) cands.resize(5);

        if (!cands.empty()) {
            auto advisor = build_memory_advisor(opts.api_keys,
                                                  supersede_model,
                                                  /*sys_prompt=*/
                "You are a memory-supersession judge.  Be conservative — "
                "only flag an existing entry as superseded when the new "
                "entry directly replaces its factual content.  Output "
                "ids comma-separated, or the single word 'none'.");
            auto sup = detect_supersession_with_advisor(advisor, title,
                                                          content, cands);
            superseded_ids        = std::move(sup.invalidated_ids);
            supersede_candidates  = std::move(sup.candidate_ids);
            supersede_note        = std::move(sup.note);

            // Apply: invalidate each flagged entry.  Failures (already
            // invalidated, cross-tenant — shouldn't happen here, but
            // guarded in the storage call) are silently dropped.
            for (auto id : superseded_ids) {
                (void)tenants.invalidate_entry(tenant.id, id);
            }
        }
    }

    auto resp = memory_entry_to_json_hydrated(e, tenants);
    // Manual-supersession metadata block.  Echoes the ids the caller
    // requested so they can confirm which were applied (vs. silently
    // skipped because the row was already invalid by the time we
    // tried).  Always emitted when the caller supplied the field.
    if (!manual_supersedes.empty()) {
        auto ms = jobj();
        auto& mso = ms->as_object_mut();
        auto req_arr = jarr();
        for (auto sid : manual_supersedes)
            req_arr->as_array_mut().push_back(jnum(static_cast<double>(sid)));
        mso["requested"] = req_arr;
        auto inv_arr = jarr();
        for (auto sid : manual_superseded_applied)
            inv_arr->as_array_mut().push_back(jnum(static_cast<double>(sid)));
        mso["invalidated"] = inv_arr;
        resp->as_object_mut()["supersedes_manual"] = ms;
    }
    // Auto-supersession metadata block.  Always emitted when requested
    // so the caller can verify what the advisor decided, even on
    // empty / no-op runs.
    if (!supersede_model.empty()) {
        auto sm = jobj();
        auto& smo = sm->as_object_mut();
        smo["model"]   = jstr(supersede_model);
        smo["applied"] = jbool(!superseded_ids.empty());
        auto cands_arr = jarr();
        auto& ca = cands_arr->as_array_mut();
        for (auto cid : supersede_candidates)
            ca.push_back(jnum(static_cast<double>(cid)));
        smo["candidates"] = cands_arr;
        auto inv_arr = jarr();
        auto& ia = inv_arr->as_array_mut();
        for (auto iid : superseded_ids)
            ia.push_back(jnum(static_cast<double>(iid)));
        smo["invalidated"] = inv_arr;
        if (!supersede_note.empty()) smo["note"] = jstr(supersede_note);
        resp->as_object_mut()["supersede"] = sm;
    }
    // When auto-tagging was requested, surface what the advisor added
    // (or why it didn't) so the caller can audit the augmentation.
    if (!auto_tag_model.empty()) {
        auto am = jobj();
        auto& amo = am->as_object_mut();
        amo["model"]   = jstr(auto_tag_model);
        amo["applied"] = jbool(!auto_tags_added.empty());
        auto added = jarr();
        auto& aa = added->as_array_mut();
        for (auto& t : auto_tags_added) aa.push_back(jstr(t));
        amo["added"] = added;
        if (!auto_tag_note.empty()) amo["note"] = jstr(auto_tag_note);
        resp->as_object_mut()["auto_tag"] = am;
    }
    write_json_response(fd, 201, resp);
}

void handle_memory_entry_list(int fd, const HttpRequest& req,
                               const ApiServerOptions& opts,
                               TenantStore& tenants, const Tenant& tenant) {
    const auto qp = parse_query(req.path);
    auto get_str = [&](const std::string& k) -> std::string {
        auto it = qp.find(k);
        return it == qp.end() ? std::string{} : it->second;
    };
    auto get_int = [&](const std::string& k) -> int64_t {
        auto it = qp.find(k);
        if (it == qp.end()) return 0;
        try { return std::stoll(it->second); } catch (...) { return 0; }
    };

    TenantStore::EntryFilter f;
    // Comma-separated list of types; reject the whole request if any value
    // is unknown so the caller sees a typo immediately rather than silent
    // empty results.
    const std::string types_csv = get_str("type");
    if (!types_csv.empty()) {
        size_t start = 0;
        while (start <= types_csv.size()) {
            size_t comma = types_csv.find(',', start);
            std::string tok = types_csv.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!tok.empty()) {
                if (!memory_entry_type_is_valid(tok))
                    return write_memory_error(fd, 400, "invalid type filter: " + tok);
                f.types.push_back(tok);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    f.tag               = get_str("tag");
    f.q                 = get_str("q");
    f.since             = get_int("since");
    f.before_updated_at = get_int("before_updated_at");
    f.limit             = static_cast<int>(get_int("limit"));
    // `as_of=<epoch>` reconstructs the active set at a past timestamp:
    // includes invalidated rows whose validity window covers that
    // moment.  Default 0 ⇒ "now", which means "active rows only".
    f.as_of             = get_int("as_of");
    // `conversation_id=<id>` scopes results to one conversation, with
    // an OR-NULL fallback so unscoped entries stay reachable.  Stripped
    // here if it doesn't belong to this tenant — silent drop rather
    // than 400 because it's a hint, not a hard constraint.
    f.conversation_id   = get_int("conversation_id");
    if (f.conversation_id > 0 &&
        !tenants.get_conversation(tenant.id, f.conversation_id)) {
        f.conversation_id = 0;
    }
    // Question-intent routing.  When the caller hasn't supplied an
    // explicit `type=` filter, classify the query for cue words and
    // soft-boost matching memory types via the existing 1.3x BM25
    // multiplier.  Opt-out via `intent=off` for callers that want
    // pure lexical ranking (debugging, regression baselines).  No-op
    // when q is empty (browse path) — types are hard filters there.
    if (f.types.empty() && !f.q.empty() && get_str("intent") != "off") {
        f.types = classify_question_intent(f.q);
    }

    // Age decay.  Off by default on the HTTP path so callers that build
    // their own ranking on top get raw BM25.  Enable per-request with
    // `decay=true|on|1` (or `decay=<half_life_days>`).  `decay_floor` /
    // `decay_half_life_days` override the bucket and floor when set.
    {
        std::string dec = get_str("decay");
        bool decay_on = (dec == "1" || dec == "true" || dec == "on" ||
                         dec == "yes");
        int hl = 0;
        try { hl = std::stoi(dec); } catch (...) { hl = 0; }
        if (hl > 0) decay_on = true;
        if (decay_on && !f.q.empty()) {
            f.age_now_epoch = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count());
            int hlv = hl;
            try {
                int s = std::stoi(get_str("decay_half_life_days"));
                if (s > 0) hlv = s;
            } catch (...) {}
            if (hlv <= 0) hlv = 90;
            f.age_half_life_days = hlv;
            try {
                double fr = std::stod(get_str("decay_floor"));
                if (fr > 0.0 && fr < 1.0) f.age_floor = fr;
            } catch (...) {}
        }
    }

    // `graduated=true` (only meaningful with q + conversation_id) runs
    // search_entries_graduated: tries conversation-scoped first, fills
    // out from tenant-wide if fewer than `limit` hits.  Without it the
    // single-pass list_entries semantic applies.
    const std::string graduated = get_str("graduated");
    const bool use_graduated = !graduated.empty() &&
                               graduated != "0" &&
                               graduated != "false" &&
                               !f.q.empty() &&
                               f.conversation_id > 0;

    // `rerank=<model>` (only meaningful with `q`) runs the FTS top-N
    // through an LLM for a final reorder.  The model is passed
    // explicitly here because there's no calling-agent context on the
    // HTTP path — the agent-side `/mem search --rerank` resolves the
    // model via the agent's advisor_model field instead.  Failures
    // (unknown model, no API key for that provider, transport error)
    // fall back to the FTS order with a `reason` populated in the
    // response's `rerank` block.
    //
    // When rerank is on, fetch a wider candidate pool than the caller
    // asked for: the rerank's gain comes from promoting items at
    // positions limit+1..pool into the top `limit` — pointless if the
    // pool never reached past `limit` to begin with.  After rerank,
    // trim back to the caller's requested `limit` so the response
    // shape matches what they asked for.
    // `expand=<model>` (only meaningful with `q`) calls the model once
    // to generate paraphrased reformulations of the query, runs each
    // through the same FTS pipeline, then RRF-fuses the rankings.
    // No-embedding alternative to dense retrieval: catches queries
    // phrased differently from the answer text without adding any
    // new index, table, or external dependency.  Cost: one extra LLM
    // call per query (~150ms at Haiku speeds).
    //
    // Failure mode is benign: if the expansion model errors or returns
    // unparseable output, the search proceeds with the original query
    // alone and a `note` is surfaced in the `expansion` response block.
    std::string expand_model = get_str("expand");

    std::string rerank_model = get_str("rerank");
    // Optional second-stage reranker.  When set, pass 1 (the cheaper
    // `rerank` model) coarse-orders the wider candidate pool and the
    // top kFinePoolSize survive to pass 2 (`rerank_fine`), which sees
    // bigger excerpts and produces the final ordering.  Tradeoff: a
    // small fast model is good enough to ditch the obvious
    // non-matches; a stronger model is meaningfully better at picking
    // among the closely-scored top candidates that determine R@1.
    // Doubles the LLM cost per query but typically lifts R@1 by
    // several points.
    std::string rerank_fine_model = get_str("rerank_fine");
    const int caller_limit =
        (f.limit > 0 && f.limit <= 200) ? f.limit : 50;
    const bool widen_pool = !rerank_model.empty() && !f.q.empty();
    if (widen_pool) {
        // Pool floor of 25 lifts even small `limit=5` callers; cap at
        // 50 so the advisor prompt stays bounded (each candidate
        // contributes ~250 bytes of excerpt + framing).  No-op when
        // the caller already asked for >= 50.
        const int desired_pool = caller_limit < 25 ? 25 : caller_limit;
        f.limit = desired_pool > 50 ? caller_limit : desired_pool;
    }

    // Shared sys_prompt for advisor-driven memory operations.  The
    // advisor responds as a memory-search collaborator, not a chat
    // companion — so no preambles, no apologies, no pleasantries.
    const std::string advisor_sys_prompt =
        "You are an advisor consulted by another AI agent.  Answer "
        "the question directly and concisely.  No preamble.  No "
        "pleasantries.  No restating the question.  No offers to "
        "help further — the executor will re-engage if it needs "
        "more.";

    // ── Optional query expansion ─────────────────────────────────────
    // When `expand=<model>` is set, generate paraphrases and fan out
    // the FTS query across all variants.  Each variant gets its own
    // intent classification (so a paraphrase that changes the question
    // shape from "what is" to "where is" still picks up an appropriate
    // type boost).
    std::vector<std::string> expansion_queries;
    std::string expansion_note;
    if (!expand_model.empty() && !f.q.empty()) {
        auto expander = build_memory_advisor(opts.api_keys, expand_model,
                                              advisor_sys_prompt);
        auto exp = expand_query_with_advisor(expander, f.q);
        expansion_queries = std::move(exp.queries);
        expansion_note    = std::move(exp.note);
    }

    std::vector<MemoryEntry> entries;
    if (expansion_queries.empty()) {
        // Fast path: single search, no fusion needed.
        entries = use_graduated
            ? tenants.search_entries_graduated(tenant.id, f)
            : tenants.list_entries(tenant.id, f);
    } else {
        // Run the original query plus each paraphrase through the
        // same pipeline.  We weight the original at 1.0 and each
        // paraphrase at 0.7 — paraphrases add recall but the original
        // captured the user's intent best, so its rank should anchor
        // the fused order.
        std::vector<std::vector<MemoryEntry>> rankings;
        std::vector<double> weights;
        rankings.reserve(1 + expansion_queries.size());
        weights.reserve(1 + expansion_queries.size());

        rankings.push_back(use_graduated
            ? tenants.search_entries_graduated(tenant.id, f)
            : tenants.list_entries(tenant.id, f));
        weights.push_back(1.0);

        for (auto& q : expansion_queries) {
            TenantStore::EntryFilter ef = f;
            ef.q = q;
            // Reclassify intent for the paraphrase.  Skip when the
            // caller passed an explicit type filter (we mirror the
            // original-query behavior).
            if (ef.types.empty() && get_str("intent") != "off") {
                ef.types = classify_question_intent(q);
            }
            rankings.push_back(use_graduated
                ? tenants.search_entries_graduated(tenant.id, ef)
                : tenants.list_entries(tenant.id, ef));
            weights.push_back(0.7);
        }

        // Limit on RRF output: same widened pool the rerank logic
        // expects (so reranking a fused candidate set behaves the same
        // as reranking a single-query candidate set).
        const int fused_limit = (f.limit > 0 && f.limit <= 200) ? f.limit : 50;
        entries = rrf_fuse_rankings(std::move(rankings), weights,
                                     fused_limit);
    }

    std::shared_ptr<JsonValue> rerank_meta;
    if (!rerank_model.empty() && !f.q.empty() && entries.size() > 1) {
        auto coarse_advisor = build_memory_advisor(opts.api_keys,
                                                    rerank_model,
                                                    advisor_sys_prompt);
        auto rr = rerank_with_advisor(coarse_advisor, f.q,
                                       std::move(entries));
        entries = std::move(rr.entries);

        // Two-stage path: when `rerank_fine` is set, take the top
        // kFinePoolSize from pass 1 and rerank again with the
        // stronger model + larger excerpts.  Use a smaller fine pool
        // so the fine model can afford to see more of each candidate
        // (the kFineExcerptBytes value below is meaningfully larger
        // than the default 800).
        //
        // Critical: preserve pass-1 candidates beyond kFinePoolSize as
        // a *tail* and append them after the fine-pass result.  R@K
        // for K > kFinePoolSize must still see those candidates;
        // otherwise the two-stage path silently caps recall at 8 and
        // R@10 collapses below the single-stage baseline.  The fine
        // pass sharpens the top of the list (R@1, R@5); the tail
        // keeps recall on parity with single-stage.
        bool fine_applied = false;
        std::string fine_note;
        if (!rerank_fine_model.empty() && rr.applied &&
            entries.size() > 1) {
            constexpr size_t kFinePoolSize    = 8;
            constexpr size_t kFineExcerptBytes = 1500;
            std::vector<MemoryEntry> tail;
            if (entries.size() > kFinePoolSize) {
                tail.reserve(entries.size() - kFinePoolSize);
                std::move(entries.begin() +
                              static_cast<std::ptrdiff_t>(kFinePoolSize),
                          entries.end(), std::back_inserter(tail));
                entries.resize(kFinePoolSize);
            }
            auto fine_advisor = build_memory_advisor(opts.api_keys,
                                                       rerank_fine_model,
                                                       advisor_sys_prompt);
            auto rr2 = rerank_with_advisor(
                fine_advisor, f.q, std::move(entries),
                kFineExcerptBytes);
            entries = std::move(rr2.entries);
            fine_applied = rr2.applied;
            fine_note = rr2.note;
            for (auto& e : tail) entries.push_back(std::move(e));
        }

        // Trim wider pool back to what the caller asked for.  Pool
        // existed only to feed the reranker; final response shape
        // matches caller_limit either way.
        if (static_cast<int>(entries.size()) > caller_limit) {
            entries.resize(static_cast<size_t>(caller_limit));
        }

        rerank_meta = jobj();
        auto& rm = rerank_meta->as_object_mut();
        rm["applied"] = jbool(rr.applied);
        rm["model"]   = jstr(rerank_model);
        if (!rr.note.empty()) rm["note"] = jstr(rr.note);
        if (!rerank_fine_model.empty()) {
            rm["fine_model"]   = jstr(rerank_fine_model);
            rm["fine_applied"] = jbool(fine_applied);
            if (!fine_note.empty()) rm["fine_note"] = jstr(fine_note);
            rm["stages"] = jnum(fine_applied ? 2 : 1);
        }
    } else if (widen_pool &&
               static_cast<int>(entries.size()) > caller_limit) {
        // Rerank was requested but didn't run (1 or 0 candidates is
        // already trivially "ranked").  Honour caller_limit anyway so
        // the wider pool is invisible from the response side.
        entries.resize(static_cast<size_t>(caller_limit));
    }

    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& e : entries) a.push_back(memory_entry_to_json(e));
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["entries"] = arr;
    m["count"]   = jnum(static_cast<double>(entries.size()));
    if (rerank_meta) m["rerank"] = rerank_meta;
    // Surface query expansion metadata so callers can debug "why did I
    // get back this set" when expansion is on.  Always emitted when
    // expansion was requested, even on advisor failure (note explains).
    if (!expand_model.empty()) {
        auto em = jobj();
        auto& emo = em->as_object_mut();
        emo["model"]   = jstr(expand_model);
        emo["applied"] = jbool(!expansion_queries.empty());
        auto qarr = jarr();
        auto& qa = qarr->as_array_mut();
        for (auto& q : expansion_queries) qa.push_back(jstr(q));
        emo["queries"] = qarr;
        if (!expansion_note.empty()) emo["note"] = jstr(expansion_note);
        m["expansion"] = em;
    }
    write_json_response(fd, 200, body);
}

void handle_memory_entry_get(int fd, int64_t id,
                              TenantStore& tenants, const Tenant& tenant) {
    auto e = tenants.get_entry(tenant.id, id);
    if (!e)
        return write_memory_error(fd, 404, "entry not found");
    // Single-entry GET hydrates the artifact link so the frontend can
    // render the file metadata next to the entry without a second round
    // trip.  Lists deliberately stay lightweight.
    write_json_response(fd, 200, memory_entry_to_json_hydrated(*e, tenants));
}

void handle_memory_entry_patch(int fd, int64_t id, const HttpRequest& req,
                                TenantStore& tenants, const Tenant& tenant) {
    log_memory_event("entry.patch.enter", tenant.id,
                      "id=" + std::to_string(id) +
                      " body_bytes=" + std::to_string(req.body.size()));

    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        log_memory_event("entry.patch.parse_error", tenant.id, e.what());
        return write_memory_error(fd, 400, std::string("invalid JSON: ") + e.what());
    }
    if (!body || !body->is_object()) {
        log_memory_event("entry.patch.shape_error", tenant.id,
                          "body is not a JSON object");
        return write_memory_error(fd, 400, "body must be a JSON object");
    }

    // Confirm the entry exists and belongs to this tenant before doing
    // input validation work — reduces 404 vs 400 ambiguity for callers.
    auto existing = tenants.get_entry(tenant.id, id);
    if (!existing) {
        log_memory_event("entry.patch.not_found", tenant.id,
                          "id=" + std::to_string(id));
        return write_memory_error(fd, 404, "entry not found");
    }
    log_memory_event("entry.patch.found", tenant.id,
                      "id=" + std::to_string(id));

    std::optional<std::string> title, content, source, tags_json, type;

    if (auto v = body->get("title"); v && v->is_string()) {
        if (v->as_string().empty() || v->as_string().size() > 200)
            return write_memory_error(fd, 400, "title length must be 1..200 chars");
        title = v->as_string();
    }
    if (auto v = body->get("content"); v && v->is_string()) {
        if (v->as_string().size() > 64 * 1024)
            return write_memory_error(fd, 400, "content exceeds 64 KiB");
        content = v->as_string();
    }
    if (auto v = body->get("source"); v && v->is_string()) {
        if (v->as_string().size() > 200)
            return write_memory_error(fd, 400, "source exceeds 200 chars");
        source = v->as_string();
    }
    if (body->get("tags")) {
        std::string err;
        auto canonical = canonical_tags_json(body->get("tags"), err);
        if (!canonical) return write_memory_error(fd, 400, err);
        tags_json = canonical;
    }
    if (auto v = body->get("type"); v && v->is_string()) {
        if (!memory_entry_type_is_valid(v->as_string()))
            return write_memory_error(fd, 400, "invalid type");
        type = v->as_string();
    }

    // artifact_id PATCH semantics: explicit `null` clears the link,
    // a positive integer sets it (validated against the tenant's
    // catalogue), and absence leaves it untouched.
    std::optional<int64_t> artifact_id;
    if (auto v = body->get("artifact_id")) {
        if (v->is_null()) {
            artifact_id = 0;       // 0 in storage layer = clear
        } else if (v->is_number()) {
            const int64_t aid = static_cast<int64_t>(v->as_number());
            if (aid < 0)
                return write_memory_error(fd, 400, "artifact_id must be ≥ 0");
            if (aid > 0 && !tenants.get_artifact_meta(tenant.id, aid))
                return write_memory_error(fd, 400,
                    "artifact_id does not exist for this tenant");
            artifact_id = aid;
        } else {
            return write_memory_error(fd, 400,
                "artifact_id must be a number or null");
        }
    }

    log_memory_event("entry.patch.update", tenant.id,
                      "id=" + std::to_string(id) +
                      " field_changes=" +
                      std::to_string(int(title.has_value())   +
                                      int(content.has_value()) +
                                      int(source.has_value())  +
                                      int(tags_json.has_value()) +
                                      int(type.has_value())     +
                                      int(artifact_id.has_value())));

    if (!tenants.update_entry(tenant.id, id, title, content, source, tags_json,
                                type, artifact_id)) {
        log_memory_event("entry.patch.update_failed", tenant.id,
                          "id=" + std::to_string(id) + " (row vanished?)");
        return write_memory_error(fd, 404, "entry not found");
    }

    auto e = tenants.get_entry(tenant.id, id);
    if (!e) {
        // Vanished between update and re-fetch — should be vanishingly
        // rare (concurrent DELETE on the same row).  Log + 410 Gone.
        log_memory_event("entry.patch.refetch_missing", tenant.id,
                          "id=" + std::to_string(id) +
                          " — entry disappeared after update; possible concurrent DELETE");
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("entry vanished between update and re-fetch — refresh and retry");
        write_json_response(fd, 410, err);
        return;
    }
    log_memory_event("entry.patch.ok", tenant.id, "id=" + std::to_string(id));
    write_json_response(fd, 200, memory_entry_to_json_hydrated(*e, tenants));
}

void handle_memory_entry_delete(int fd, int64_t id,
                                 TenantStore& tenants, const Tenant& tenant) {
    if (!tenants.delete_entry(tenant.id, id))
        return write_memory_error(fd, 404, "entry not found");
    auto body = jobj();
    body->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, body);
}

// POST /v1/memory/entries/:id/invalidate — soft delete with a temporal
// window.  Distinct from DELETE: the row stays in the DB and is still
// reachable through `?as_of=<epoch>` for replay / audit, but disappears
// from the default active set.  The optional body `{"when": <epoch>}`
// pins the invalidation moment; without it we use the wall clock.
void handle_memory_entry_invalidate(int fd, int64_t id, const HttpRequest& req,
                                     TenantStore& tenants, const Tenant& tenant) {
    int64_t when = 0;
    if (!req.body.empty()) {
        std::shared_ptr<JsonValue> body;
        try { body = json_parse(req.body); }
        catch (const std::exception& e) {
            return write_memory_error(fd, 400,
                std::string("invalid JSON: ") + e.what());
        }
        if (body && body->is_object()) {
            // Treat null / missing as "now".  Negative values are
            // operator error and rejected up front.
            if (auto v = body->get("when"); v && v->is_number()) {
                when = static_cast<int64_t>(v->as_number());
                if (when < 0)
                    return write_memory_error(fd, 400,
                        "'when' must be a non-negative epoch");
            }
        }
    }

    if (!tenants.invalidate_entry(tenant.id, id, when)) {
        // The storage layer collapses three rejection cases into a
        // single false: missing row, cross-tenant, or already-invalidated.
        // Distinguish with a probe so the HTTP status is informative.
        // get_entry filters to active rows; if it returns the row, we
        // reached this branch via concurrent invalidate — race; report
        // 409.  If get_entry is empty but a raw read finds the row with
        // valid_to set, this is double-invalidate → 409.  Otherwise the
        // row genuinely doesn't exist → 404.
        auto active = tenants.get_entry(tenant.id, id);
        if (active) {
            return write_memory_error(fd, 409,
                "entry was already invalidated by a concurrent request");
        }
        // We don't currently expose a "fetch invalidated" point read in
        // the storage layer (intentionally — see get_entry's comment).
        // Treat the false here as either-not-found-or-already-invalid
        // and return 409 only when the caller's intent ("soft-delete
        // this") is satisfied by current state.  Without a way to tell,
        // 404 is the conservative default.
        return write_memory_error(fd, 404,
            "entry not found or already invalidated");
    }
    auto e = tenants.get_entry(tenant.id, id);   // returns None — entry is now inactive
    (void)e;
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["invalidated"] = jbool(true);
    m["id"]          = jnum(static_cast<double>(id));
    write_json_response(fd, 200, body);
}

void handle_memory_relation_create(int fd, const HttpRequest& req,
                                    TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        return write_memory_error(fd, 400, std::string("invalid JSON: ") + e.what());
    }
    if (!body || !body->is_object())
        return write_memory_error(fd, 400, "body must be a JSON object");

    const int64_t source_id = static_cast<int64_t>(body->get_number("source_id", 0));
    const int64_t target_id = static_cast<int64_t>(body->get_number("target_id", 0));
    const std::string relation = body->get_string("relation");

    if (source_id <= 0 || target_id <= 0)
        return write_memory_error(fd, 400, "source_id and target_id are required");
    if (source_id == target_id)
        return write_memory_error(fd, 400, "self-loops not allowed");
    if (!memory_relation_is_valid(relation))
        return write_memory_error(fd, 400, "invalid relation");

    // Both endpoints must belong to this tenant.  We surface either side
    // missing as 400 with "entries belong to different tenants" — caller
    // can't distinguish "doesn't exist" from "belongs to someone else"
    // without leaking cross-tenant ids, and 404 here would be ambiguous
    // (which entry is missing?).
    if (!tenants.get_entry(tenant.id, source_id) ||
        !tenants.get_entry(tenant.id, target_id))
        return write_memory_error(fd, 400, "entries belong to different tenants");

    auto created = tenants.create_relation(tenant.id, source_id, target_id, relation);
    if (!created) {
        auto existing = tenants.find_relation(tenant.id, source_id, target_id, relation);
        auto err = jobj();
        auto& m = err->as_object_mut();
        m["error"] = jstr("relation already exists");
        if (existing) m["existing_id"] = jnum(static_cast<double>(existing->id));
        write_json_response(fd, 409, err);
        return;
    }
    write_json_response(fd, 201, memory_relation_to_json(*created));
}

void handle_memory_relation_list(int fd, const HttpRequest& req,
                                  TenantStore& tenants, const Tenant& tenant) {
    const auto qp = parse_query(req.path);
    auto get_int = [&](const std::string& k) -> int64_t {
        auto it = qp.find(k);
        if (it == qp.end()) return 0;
        try { return std::stoll(it->second); } catch (...) { return 0; }
    };
    auto get_str = [&](const std::string& k) -> std::string {
        auto it = qp.find(k);
        return it == qp.end() ? std::string{} : it->second;
    };

    const int64_t source_id = get_int("source_id");
    const int64_t target_id = get_int("target_id");
    const std::string relation = get_str("relation");
    const int limit = static_cast<int>(get_int("limit"));

    if (!relation.empty() && !memory_relation_is_valid(relation))
        return write_memory_error(fd, 400, "invalid relation filter");

    auto rels = tenants.list_relations(tenant.id, source_id, target_id, relation,
                                        limit);
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& r : rels) a.push_back(memory_relation_to_json(r));
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["relations"] = arr;
    m["count"]     = jnum(static_cast<double>(rels.size()));
    write_json_response(fd, 200, body);
}

void handle_memory_relation_delete(int fd, int64_t id,
                                    TenantStore& tenants, const Tenant& tenant) {
    if (!tenants.delete_relation(tenant.id, id))
        return write_memory_error(fd, 404, "relation not found");
    auto body = jobj();
    body->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, body);
}

// (PATCH /v1/memory/relations/:id and GET /v1/memory/proposals were
// removed when the proposal-queue model was retired — agents now write
// directly into the curated graph.  Reject paths use DELETE on the
// underlying entry / relation row.)


void handle_memory_graph(int fd, const HttpRequest& req,
                          TenantStore& tenants, const Tenant& tenant) {
    // Optional `?type=` filter scopes the entry set; relations are then
    // pruned to those with both endpoints in that set so the snapshot is
    // self-consistent.  No pagination — the unfiltered result is expected
    // to fit in one response for v1.  When a tenant outgrows that, add
    // pagination here rather than guessing a cap up front.
    const auto qp = parse_query(req.path);
    auto get_str = [&](const std::string& k) -> std::string {
        auto it = qp.find(k);
        return it == qp.end() ? std::string{} : it->second;
    };

    TenantStore::EntryFilter f;
    f.limit  = 200;  // hit the per-call ceiling
    const std::string types_csv = get_str("type");
    if (!types_csv.empty()) {
        size_t start = 0;
        while (start <= types_csv.size()) {
            size_t comma = types_csv.find(',', start);
            std::string tok = types_csv.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!tok.empty()) {
                if (!memory_entry_type_is_valid(tok))
                    return write_memory_error(fd, 400, "invalid type filter: " + tok);
                f.types.push_back(tok);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    // Drain pages of entries until we've got every match.  list_entries is
    // updated_at DESC; cursor on the last row's updated_at to step backward.
    std::vector<MemoryEntry> entries;
    while (true) {
        auto page = tenants.list_entries(tenant.id, f);
        if (page.empty()) break;
        const int64_t last_updated = page.back().updated_at;
        // Bail on degenerate same-timestamp pages — without a strict
        // tie-break we'd loop forever.  Effectively caps the snapshot at
        // 200 entries when many share an updated_at, which is acceptable
        // for v1.
        if (f.before_updated_at > 0 && last_updated >= f.before_updated_at) {
            for (auto& e : page) entries.push_back(std::move(e));
            break;
        }
        for (auto& e : page) entries.push_back(std::move(e));
        if (static_cast<int>(page.size()) < f.limit) break;
        f.before_updated_at = last_updated;
    }

    // Index entry ids for the relation filter.
    std::unordered_map<int64_t, bool> entry_set;
    entry_set.reserve(entries.size() * 2);
    for (auto& e : entries) entry_set[e.id] = true;

    // All relations for this tenant; filter by the entry set in memory
    // (cheap relative to a join, and keeps the relation query trivial).
    auto rels = tenants.list_relations(tenant.id, 0, 0, std::string{}, 1000);

    auto entries_arr = jarr();
    auto& ea = entries_arr->as_array_mut();
    for (auto& e : entries) ea.push_back(memory_entry_to_json(e));

    auto rels_arr = jarr();
    auto& ra = rels_arr->as_array_mut();
    for (auto& r : rels) {
        if (entry_set.count(r.source_id) && entry_set.count(r.target_id)) {
            ra.push_back(memory_relation_to_json(r));
        }
    }

    auto body = jobj();
    auto& m = body->as_object_mut();
    m["tenant_id"] = jnum(static_cast<double>(tenant.id));
    m["entries"]   = entries_arr;
    m["relations"] = rels_arr;
    write_json_response(fd, 200, body);
}

// ─── Artifact store (HTTP surface) ──────────────────────────────────────
//
// Persistent per-(tenant, conversation) file blobs.  Two axes of access:
//   • /v1/conversations/:id/artifacts        — primary, conversation-scoped
//   • /v1/artifacts                          — secondary, tenant-wide discovery
// Both surface the same ArtifactRecord shape; metadata responses never
// include `content` (the raw blob ships only on /raw, with proper
// Content-Type + ETag).

std::shared_ptr<JsonValue> artifact_to_json(const ArtifactRecord& a) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]              = jnum(static_cast<double>(a.id));
    m["tenant_id"]       = jnum(static_cast<double>(a.tenant_id));
    m["conversation_id"] = jnum(static_cast<double>(a.conversation_id));
    m["path"]            = jstr(a.path);
    m["sha256"]          = jstr(a.sha256);
    m["mime_type"]       = jstr(a.mime_type);
    m["size"]            = jnum(static_cast<double>(a.size));
    m["created_at"]      = jnum(static_cast<double>(a.created_at));
    m["updated_at"]      = jnum(static_cast<double>(a.updated_at));
    return o;
}

void write_artifact_error(int fd, int code, const std::string& msg) {
    auto e = jobj();
    e->as_object_mut()["error"] = jstr(msg);
    write_json_response(fd, code, e);
}

// Validate a tenant-supplied media type before storage.  This value is
// later echoed verbatim into the Content-Type response header on
// /v1/artifacts/:id/raw — without this guard a tenant can inject
// CRLF + extra headers + body, splitting the response.  We accept any
// printable ASCII (0x20..0x7E) up to 127 chars containing at least one
// '/', and reject CR/LF/NUL/CTLs/non-ASCII outright.  Empty stays
// empty (the store applies its `application/octet-stream` default).
bool is_valid_mime_type(const std::string& s) {
    if (s.empty()) return true;          // store fills in a safe default
    if (s.size() > 127) return false;
    bool saw_slash = false;
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return false;   // CTLs + 8-bit
        if (c == '/') saw_slash = true;
    }
    return saw_slash;
}

// POST /v1/conversations/:id/artifacts
// Body: { "path": "...", "content": "...", "mime_type"?: "..." }
// Used by the frontend (or any non-agent caller) to drop a file into a
// conversation's working dir.  Same path validator + quota math as the
// agent /write --persist path.
void handle_artifact_create(int fd, int64_t conversation_id,
                              const HttpRequest& req,
                              TenantStore& tenants, const Tenant& tenant) {
    auto conv = tenants.get_conversation(tenant.id, conversation_id);
    if (!conv) return write_artifact_error(fd, 404, "conversation not found");

    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        return write_artifact_error(fd, 400, std::string("invalid JSON: ") + e.what());
    }
    if (!body || !body->is_object())
        return write_artifact_error(fd, 400, "body must be a JSON object");

    const std::string raw_path = body->get_string("path", "");
    const std::string content  = body->get_string("content", "");
    const std::string mime     = body->get_string("mime_type", "");

    if (!is_valid_mime_type(mime))
        return write_artifact_error(fd, 400,
            "invalid mime_type: must be printable ASCII, contain '/', "
            "and be ≤127 chars (no CR/LF/NUL)");

    std::string sanitize_err;
    auto canonical = sanitize_artifact_path(raw_path, sanitize_err);
    if (!canonical)
        return write_artifact_error(fd, 400, "invalid path: " + sanitize_err);

    auto put = tenants.put_artifact(tenant.id, conversation_id, *canonical,
                                     content, mime);
    switch (put.status) {
        case PutArtifactResult::Status::Created:
        case PutArtifactResult::Status::Updated: {
            auto resp = jobj();
            auto& m = resp->as_object_mut();
            m["artifact"] = artifact_to_json(*put.record);
            m["tenant_used_bytes"]       = jnum(static_cast<double>(put.tenant_used_bytes));
            m["conversation_used_bytes"] = jnum(static_cast<double>(put.conversation_used_bytes));
            m["created"] = jbool(put.status == PutArtifactResult::Status::Created);
            const int code = (put.status == PutArtifactResult::Status::Created) ? 201 : 200;
            write_json_response(fd, code, resp);
            return;
        }
        case PutArtifactResult::Status::QuotaExceeded:
            return write_artifact_error(fd, 413, put.error_msg);
        case PutArtifactResult::Status::PathRejected:
            return write_artifact_error(fd, 409, put.error_msg);
    }
}

// GET /v1/conversations/:id/artifacts
// Lists this conversation's artifacts, newest-updated first.
void handle_artifact_list_conversation(int fd, int64_t conversation_id,
                                        TenantStore& tenants, const Tenant& tenant) {
    auto conv = tenants.get_conversation(tenant.id, conversation_id);
    if (!conv) return write_artifact_error(fd, 404, "conversation not found");

    auto rows = tenants.list_artifacts_conversation(tenant.id, conversation_id, 200);
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& r : rows) a.push_back(artifact_to_json(r));

    auto body = jobj();
    auto& m = body->as_object_mut();
    m["conversation_id"]         = jnum(static_cast<double>(conversation_id));
    m["artifacts"]               = arr;
    m["count"]                   = jnum(static_cast<double>(rows.size()));
    m["bytes_used"]              = jnum(static_cast<double>(
        tenants.bytes_used_conversation(tenant.id, conversation_id)));
    m["tenant_bytes_used"]       = jnum(static_cast<double>(
        tenants.bytes_used_tenant(tenant.id)));
    write_json_response(fd, 200, body);
}

// GET /v1/artifacts — tenant-scoped cross-conversation discovery.
void handle_artifact_list_tenant(int fd, TenantStore& tenants, const Tenant& tenant) {
    auto rows = tenants.list_artifacts_tenant(tenant.id, 200);
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& r : rows) a.push_back(artifact_to_json(r));

    auto body = jobj();
    auto& m = body->as_object_mut();
    m["tenant_id"]   = jnum(static_cast<double>(tenant.id));
    m["artifacts"]   = arr;
    m["count"]       = jnum(static_cast<double>(rows.size()));
    m["bytes_used"]  = jnum(static_cast<double>(tenants.bytes_used_tenant(tenant.id)));
    write_json_response(fd, 200, body);
}

void handle_artifact_get_meta(int fd, int64_t artifact_id,
                                TenantStore& tenants, const Tenant& tenant) {
    auto rec = tenants.get_artifact_meta(tenant.id, artifact_id);
    if (!rec) return write_artifact_error(fd, 404, "artifact not found");
    write_json_response(fd, 200, artifact_to_json(*rec));
}

// GET /v1/artifacts/:id/raw — content body with proper Content-Type +
// ETag (= sha256) for conditional GETs.  Tenant-scoped lookup; cross-
// tenant id surfaces as 404.
void handle_artifact_get_raw(int fd, int64_t artifact_id,
                              const HttpRequest& req,
                              TenantStore& tenants, const Tenant& tenant) {
    auto rec = tenants.get_artifact_meta(tenant.id, artifact_id);
    if (!rec) return write_artifact_error(fd, 404, "artifact not found");

    // ETag honors the strong-validator semantics — sha256 of the bytes.
    // Quote per RFC 7232.  If-None-Match returns 304 cheaply.
    const std::string etag = "\"" + rec->sha256 + "\"";
    auto inm = req.headers.find("if-none-match");
    if (inm != req.headers.end() && inm->second == etag) {
        std::ostringstream ss;
        ss << "HTTP/1.1 304 Not Modified\r\n"
           << "ETag: " << etag << "\r\n"
           << kCorsHeaders
           << "Content-Length: 0\r\n"
           << "Connection: close\r\n\r\n";
        write_all(fd, ss.str());
        return;
    }

    auto blob = tenants.get_artifact_content(tenant.id, artifact_id);
    if (!blob) return write_artifact_error(fd, 404, "artifact content missing");

    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << rec->mime_type << "\r\n"
       << "Content-Length: " << blob->size() << "\r\n"
       << "ETag: " << etag << "\r\n"
       << kCorsHeaders
       << "Connection: close\r\n\r\n";
    write_all(fd, ss.str());
    write_all(fd, *blob);
}

void handle_artifact_delete(int fd, int64_t artifact_id,
                              TenantStore& tenants, const Tenant& tenant) {
    if (!tenants.delete_artifact(tenant.id, artifact_id))
        return write_artifact_error(fd, 404, "artifact not found");
    auto body = jobj();
    body->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, body);
}

// Short random hex id for correlating in-flight requests.  Not a UUID —
// just 16 hex chars of OpenSSL RAND_bytes.  Collisions in the in-flight
// map are effectively impossible for any realistic concurrency level.
#include <openssl/rand.h>
std::string new_request_id() {
    unsigned char buf[8];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        // Extremely unlikely; fall back to a monotonic counter so the
        // caller never sees empty.  The counter's not cryptographically
        // secure but request_ids don't need to be.
        static std::atomic<uint64_t> ctr{0};
        uint64_t n = ctr.fetch_add(1);
        std::memcpy(buf, &n, sizeof(buf));
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (unsigned char c : buf) { out += hex[c >> 4]; out += hex[c & 0xF]; }
    return out;
}

// RAII add/remove for the in-flight map.  Lifetime must match the
// Orchestrator's so a cancel arriving after completion misses cleanly.
class InFlightScope {
public:
    InFlightScope(InFlightRegistry& reg, std::string id,
                   Orchestrator* orch, int64_t tenant_id)
        : reg_(reg), id_(std::move(id)) {
        std::lock_guard<std::mutex> lk(reg_.mu);
        reg_.by_id[id_] = {orch, tenant_id};
    }
    ~InFlightScope() {
        std::lock_guard<std::mutex> lk(reg_.mu);
        reg_.by_id.erase(id_);
    }
    InFlightScope(const InFlightScope&) = delete;
    InFlightScope& operator=(const InFlightScope&) = delete;
private:
    InFlightRegistry& reg_;
    std::string       id_;
};

void handle_cancel(int fd, const HttpRequest& req,
                    InFlightRegistry& reg, const Tenant& tenant) {
    const auto segs = split_path(req.path);
    // /v1/requests/:id/cancel
    if (segs.size() != 4 || segs[3] != "cancel") {
        auto e = jobj();
        e->as_object_mut()["error"] = jstr("expected /v1/requests/:id/cancel");
        write_json_response(fd, 404, e);
        return;
    }
    const std::string request_id = segs[2];

    // Critical: we must call target->cancel() *while holding* reg.mu.
    // Releasing the lock and then dereferencing `target` outside the
    // critical section is a use-after-free: the owning request thread
    // can run ~InFlightScope (which acquires reg.mu and erases the
    // entry) and continue stack-unwinding, destroying the Orchestrator,
    // before this thread reaches the deref.  cancel() is short — sets
    // an atomic and shuts down sockets under a different mutex — so
    // holding reg.mu through it costs nothing and rules out the race.
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lk(reg.mu);
        auto it = reg.by_id.find(request_id);
        if (it != reg.by_id.end() && it->second.tenant_id == tenant.id) {
            // Tenant isolation: cross-tenant ids surface as 404, never
            // as a successful cancel of another tenant's stream.
            it->second.orch->cancel();
            cancelled = true;
        }
    }
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["request_id"] = jstr(request_id);
    if (cancelled) {
        m["cancelled"] = jbool(true);
        write_json_response(fd, 200, body);
    } else {
        m["cancelled"] = jbool(false);
        m["reason"]    = jstr("no in-flight request with that id");
        write_json_response(fd, 404, body);
    }
}

// ── Request log + resubscribe ──────────────────────────────────────────

std::shared_ptr<JsonValue>
request_status_to_json(const TenantStore::RequestStatus& s) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["request_id"]      = jstr(s.request_id);
    m["agent_id"]        = jstr(s.agent_id);
    m["conversation_id"] = jnum(s.conversation_id);
    m["state"]           = jstr(s.state);
    m["started_at"]      = jnum(s.started_at);
    m["completed_at"]    = jnum(s.completed_at);
    m["error_message"]   = jstr(s.error_message);
    m["last_seq"]        = jnum(s.last_seq);
    return o;
}

void handle_request_list(int fd, const HttpRequest& req,
                          TenantStore& tenants, const Tenant& tenant) {
    int limit = 100;
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = req.path.substr(qpos + 1);
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp - i);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if (k == "limit") try { limit = std::stoi(v); } catch (...) {}
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }
    auto rows = tenants.list_request_status(tenant.id, limit);
    auto arr = jarr();
    for (const auto& r : rows) arr->as_array_mut().push_back(request_status_to_json(r));
    auto out = jobj();
    out->as_object_mut()["requests"] = arr;
    write_json_response(fd, 200, out);
}

void handle_request_get(int fd, const std::string& request_id,
                         TenantStore& tenants, const Tenant& tenant) {
    auto row = tenants.get_request_status(tenant.id, request_id);
    if (!row) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("request not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["request"] = request_status_to_json(*row);
    write_json_response(fd, 200, out);
}

// SSE replay-and-tail for /v1/requests/:id/events.  Streams the backlog
// after `since_seq`, then — if the run is still in flight — subscribes
// to the per-request bus and continues emitting newly-persisted events
// until the bus delivers the terminal `done` envelope.  Reconnects
// after a disconnect re-sync via the same endpoint with the last seen
// seq; the bus does no buffering.
void handle_request_events(int fd, const std::string& request_id,
                             const HttpRequest& req,
                             TenantStore& tenants, const Tenant& tenant,
                             RequestEventBus* bus) {
    auto status = tenants.get_request_status(tenant.id, request_id);
    if (!status) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("request not found");
        write_json_response(fd, 404, err);
        return;
    }

    int64_t since_seq = 0;
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = req.path.substr(qpos + 1);
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp - i);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if (k == "since_seq") try { since_seq = std::stoll(v); } catch (...) {}
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }

    // Open the SSE response.  CORS headers + X-Accel-Buffering: no so
    // dev-mode SPAs on a different origin can subscribe.
    {
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/event-stream\r\n"
            << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            << "X-Accel-Buffering: no\r\n"
            << kCorsHeaders
            << "Connection: close\r\n"
            << "\r\n";
        write_all(fd, hdr.str());
    }

    auto write_envelope = [fd](const std::string& kind, int64_t seq,
                                  const std::string& payload_json) {
        // Each persisted event becomes one SSE frame.  The seq is
        // exposed via the SSE `id:` field so a reconnecting client can
        // pass it back as `since_seq` without parsing the payload.
        std::ostringstream f;
        f << "id: " << seq << "\n"
          << "event: " << kind << "\n"
          << "data: " << payload_json << "\n\n";
        write_all(fd, f.str());
    };

    // Replay backlog.  Page through in chunks of 1000 — for runs
    // longer than that we'd rather many small writes than one huge
    // SQL fetch holding the connection's CPU time.
    int64_t cursor = since_seq;
    while (true) {
        auto chunk = tenants.list_request_events(tenant.id, request_id,
                                                    cursor, /*limit=*/1000);
        if (chunk.empty()) break;
        for (const auto& e : chunk) {
            write_envelope(e.event_kind, e.seq, e.payload_json);
            cursor = e.seq;
        }
        if (chunk.size() < 1000) break;
    }

    // Re-fetch status.  If it's already terminal, we're done — close.
    auto post_replay = tenants.get_request_status(tenant.id, request_id);
    if (!post_replay || post_replay->state != "running") return;
    if (!bus) return;

    // Live tail.  Subscribe; mailbox-buffer events under a small mutex
    // so the publisher thread doesn't block on slow clients.  Heartbeat
    // every 30s so reverse proxies don't time out the idle connection.
    std::mutex                      mb_mu;
    std::condition_variable         mb_cv;
    std::deque<RequestEventEnvelope> mailbox;
    std::atomic<bool>               saw_terminal{false};

    int64_t sub_id = bus->subscribe(request_id,
        [&mb_mu, &mb_cv, &mailbox, &saw_terminal](const RequestEventEnvelope& env) {
            std::lock_guard<std::mutex> lk(mb_mu);
            mailbox.push_back(env);
            if (env.terminal) saw_terminal = true;
            mb_cv.notify_one();
        });

    // Drain — exit when we've delivered the terminal envelope.
    using clock = std::chrono::steady_clock;
    auto next_heartbeat = clock::now() + std::chrono::seconds(30);
    while (!saw_terminal.load() || true) {
        RequestEventEnvelope ev;
        bool have = false;
        bool was_terminal = false;
        {
            std::unique_lock<std::mutex> lk(mb_mu);
            mb_cv.wait_until(lk, next_heartbeat,
                [&]{ return !mailbox.empty(); });
            if (!mailbox.empty()) {
                ev = mailbox.front();
                mailbox.pop_front();
                have = true;
                was_terminal = ev.terminal;
            }
        }
        if (have) {
            // Skip events at or before the cursor — the publisher races
            // the backlog scan and may republish events we already wrote.
            if (ev.seq > cursor) {
                write_envelope(ev.event_kind, ev.seq, ev.payload_json);
                cursor = ev.seq;
            }
            if (was_terminal) break;
            continue;
        }
        // Heartbeat
        const char* hb = ": heartbeat\n\n";
        write_all(fd, hb, std::strlen(hb));
        next_heartbeat = clock::now() + std::chrono::seconds(30);
        // Re-poll status: if a recovery sweep flipped the row to
        // failed without publishing on the bus (e.g. another process
        // marked it), append a terminal frame and exit.
        auto fresh = tenants.get_request_status(tenant.id, request_id);
        if (fresh && fresh->state != "running") {
            auto term = jobj();
            term->as_object_mut()["ok"]     = jbool(fresh->state == "completed");
            term->as_object_mut()["reason"] = jstr(fresh->error_message);
            term->as_object_mut()["state"]  = jstr(fresh->state);
            write_envelope("done", fresh->last_seq + 1,
                            json_serialize(*term));
            break;
        }
    }
    bus->unsubscribe(sub_id);
}

// ── Lessons ────────────────────────────────────────────────────────────

std::shared_ptr<JsonValue> lesson_to_json(const TenantStore::Lesson& l) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]            = jnum(l.id);
    m["agent_id"]      = jstr(l.agent_id);
    m["signature"]     = jstr(l.signature);
    m["lesson_text"]   = jstr(l.lesson_text);
    m["hit_count"]     = jnum(l.hit_count);
    m["created_at"]    = jnum(l.created_at);
    m["updated_at"]    = jnum(l.updated_at);
    m["last_seen_at"]  = jnum(l.last_seen_at);
    return o;
}

void handle_lesson_create(int fd, const HttpRequest& req,
                           TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }
    auto get_str = [&](const char* k) -> std::string {
        auto v = body->get(k);
        return (v && v->is_string()) ? v->as_string() : std::string{};
    };
    std::string signature   = get_str("signature");
    std::string lesson_text = get_str("lesson_text");
    std::string agent_id    = get_str("agent_id");
    if (signature.empty() || lesson_text.empty()) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("required fields: signature, lesson_text");
        write_json_response(fd, 400, err);
        return;
    }
    if (signature.size() > 200) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("signature must be ≤ 200 chars");
        write_json_response(fd, 400, err);
        return;
    }
    if (lesson_text.size() > 4096) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("lesson_text must be ≤ 4096 chars");
        write_json_response(fd, 400, err);
        return;
    }
    if (agent_id.empty()) agent_id = "index";
    auto l = tenants.create_lesson(tenant.id, agent_id, signature, lesson_text);
    auto out = jobj();
    out->as_object_mut()["lesson"] = lesson_to_json(l);
    write_json_response(fd, 201, out);
}

void handle_lesson_list(int fd, const HttpRequest& req,
                         TenantStore& tenants, const Tenant& tenant) {
    std::string agent_id;
    std::string query;
    int limit = 100;
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = req.path.substr(qpos + 1);
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp - i);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if      (k == "agent_id") agent_id = v;
                else if (k == "q")        query    = v;
                else if (k == "limit") try { limit = std::stoi(v); } catch (...) {}
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }
    auto rows = query.empty()
        ? tenants.list_lessons(tenant.id, agent_id, limit)
        : tenants.search_lessons(tenant.id, agent_id, query, limit);
    auto arr = jarr();
    for (auto& r : rows) arr->as_array_mut().push_back(lesson_to_json(r));
    auto out = jobj();
    out->as_object_mut()["lessons"] = arr;
    write_json_response(fd, 200, out);
}

void handle_lesson_get(int fd, int64_t id,
                        TenantStore& tenants, const Tenant& tenant) {
    auto row = tenants.get_lesson(tenant.id, id);
    if (!row) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("lesson not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["lesson"] = lesson_to_json(*row);
    write_json_response(fd, 200, out);
}

void handle_lesson_patch(int fd, int64_t id, const HttpRequest& req,
                          TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }
    auto opt_str = [&](const char* k) -> std::optional<std::string> {
        auto v = body->get(k);
        if (v && v->is_string()) return v->as_string();
        return std::nullopt;
    };
    auto sig = opt_str("signature");
    auto txt = opt_str("lesson_text");
    if (sig && sig->size() > 200) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("signature must be ≤ 200 chars");
        write_json_response(fd, 400, err);
        return;
    }
    if (txt && txt->size() > 4096) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("lesson_text must be ≤ 4096 chars");
        write_json_response(fd, 400, err);
        return;
    }
    if (!tenants.update_lesson(tenant.id, id, sig, txt)) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("lesson not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto row = tenants.get_lesson(tenant.id, id);
    auto out = jobj();
    if (row) out->as_object_mut()["lesson"] = lesson_to_json(*row);
    write_json_response(fd, 200, out);
}

void handle_lesson_delete(int fd, int64_t id,
                           TenantStore& tenants, const Tenant& tenant) {
    if (!tenants.delete_lesson(tenant.id, id)) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("lesson not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, out);
}

// ── Todos ──────────────────────────────────────────────────────────────

std::shared_ptr<JsonValue> todo_to_json(const TenantStore::Todo& t) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]              = jnum(t.id);
    m["conversation_id"] = jnum(t.conversation_id);
    m["agent_id"]        = jstr(t.agent_id);
    m["subject"]         = jstr(t.subject);
    m["description"]     = jstr(t.description);
    m["status"]          = jstr(t.status);
    m["position"]        = jnum(t.position);
    m["created_at"]      = jnum(t.created_at);
    m["updated_at"]      = jnum(t.updated_at);
    m["completed_at"]    = jnum(t.completed_at);
    return o;
}

void handle_todo_create(int fd, const HttpRequest& req,
                         TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }
    auto get_str = [&](const char* k) -> std::string {
        auto v = body->get(k);
        return (v && v->is_string()) ? v->as_string() : std::string{};
    };
    auto get_int = [&](const char* k) -> int64_t {
        auto v = body->get(k);
        return (v && v->is_number()) ? static_cast<int64_t>(v->as_number()) : 0;
    };
    std::string subject     = get_str("subject");
    std::string description = get_str("description");
    std::string agent_id    = get_str("agent_id");
    int64_t conv_id         = get_int("conversation_id");
    if (subject.empty()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("required field: subject");
        write_json_response(fd, 400, err);
        return;
    }
    if (agent_id.empty()) agent_id = "index";
    auto row = tenants.create_todo(tenant.id, conv_id, agent_id, subject, description);
    auto out = jobj();
    out->as_object_mut()["todo"] = todo_to_json(row);
    write_json_response(fd, 201, out);
}

void handle_todo_list(int fd, const HttpRequest& req,
                       TenantStore& tenants, const Tenant& tenant) {
    TenantStore::TodoFilter f;
    f.limit = 200;
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = req.path.substr(qpos + 1);
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp - i);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if (k == "conversation_id")
                    try { f.conversation_id = std::stoll(v); } catch (...) {}
                else if (k == "status")   f.status_filter   = v;
                else if (k == "agent_id") f.agent_id_filter = v;
                else if (k == "limit")
                    try { f.limit = std::stoi(v); } catch (...) {}
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }
    auto rows = tenants.list_todos(tenant.id, f);
    auto arr = jarr();
    for (const auto& r : rows) arr->as_array_mut().push_back(todo_to_json(r));
    auto out = jobj();
    out->as_object_mut()["todos"] = arr;
    write_json_response(fd, 200, out);
}

void handle_todo_get(int fd, int64_t id,
                      TenantStore& tenants, const Tenant& tenant) {
    auto row = tenants.get_todo(tenant.id, id);
    if (!row) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("todo not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["todo"] = todo_to_json(*row);
    write_json_response(fd, 200, out);
}

void handle_todo_patch(int fd, int64_t id, const HttpRequest& req,
                        TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }
    auto opt_str = [&](const char* k) -> std::optional<std::string> {
        auto v = body->get(k);
        if (v && v->is_string()) return v->as_string();
        return std::nullopt;
    };
    auto opt_int = [&](const char* k) -> std::optional<int64_t> {
        auto v = body->get(k);
        if (v && v->is_number()) return static_cast<int64_t>(v->as_number());
        return std::nullopt;
    };
    auto subj_opt = opt_str("subject");
    auto desc_opt = opt_str("description");
    auto stat_opt = opt_str("status");
    auto pos_opt  = opt_int("position");
    if (stat_opt) {
        const std::string& s = *stat_opt;
        if (s != "pending" && s != "in_progress" &&
            s != "completed" && s != "canceled") {
            auto err = jobj();
            err->as_object_mut()["error"] = jstr(
                "status must be one of: pending, in_progress, completed, canceled");
            write_json_response(fd, 400, err);
            return;
        }
    }
    bool ok = tenants.update_todo(tenant.id, id,
        subj_opt, desc_opt, stat_opt, pos_opt, std::nullopt);
    if (!ok) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("todo not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto row = tenants.get_todo(tenant.id, id);
    auto out = jobj();
    if (row) out->as_object_mut()["todo"] = todo_to_json(*row);
    write_json_response(fd, 200, out);
}

void handle_todo_delete(int fd, int64_t id,
                         TenantStore& tenants, const Tenant& tenant) {
    if (!tenants.delete_todo(tenant.id, id)) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("todo not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, out);
}

// ── Schedules + runs + notifications ───────────────────────────────────

std::shared_ptr<JsonValue> scheduled_task_to_json(
        const TenantStore::ScheduledTask& t) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]               = jnum(t.id);
    m["agent_id"]         = jstr(t.agent_id);
    m["conversation_id"]  = jnum(t.conversation_id);
    m["message"]          = jstr(t.message);
    m["schedule_phrase"]  = jstr(t.schedule_phrase);
    m["schedule_kind"]    = jstr(t.schedule_kind);
    m["fire_at"]          = jnum(t.fire_at);
    m["recur_json"]       = jstr(t.recur_json);
    m["next_fire_at"]     = jnum(t.next_fire_at);
    m["status"]           = jstr(t.status);
    m["created_at"]       = jnum(t.created_at);
    m["updated_at"]       = jnum(t.updated_at);
    m["last_run_at"]      = jnum(t.last_run_at);
    m["last_run_id"]      = jnum(t.last_run_id);
    m["run_count"]        = jnum(t.run_count);
    return o;
}

std::shared_ptr<JsonValue> task_run_to_json(const TenantStore::TaskRun& r) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]              = jnum(r.id);
    m["task_id"]         = jnum(r.task_id);
    m["status"]          = jstr(r.status);
    m["started_at"]      = jnum(r.started_at);
    m["completed_at"]    = jnum(r.completed_at);
    m["request_id"]      = jstr(r.request_id);
    m["result_summary"]  = jstr(r.result_summary);
    m["error_message"]   = jstr(r.error_message);
    m["input_tokens"]    = jnum(r.input_tokens);
    m["output_tokens"]   = jnum(r.output_tokens);
    return o;
}

void handle_schedule_create(int fd, const HttpRequest& req,
                             TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }
    auto get_str = [&](const char* k) -> std::string {
        auto v = body->get(k);
        return (v && v->is_string()) ? v->as_string() : std::string{};
    };
    auto get_int = [&](const char* k) -> int64_t {
        auto v = body->get(k);
        return (v && v->is_number()) ? static_cast<int64_t>(v->as_number()) : 0;
    };
    std::string phrase  = get_str("schedule");
    std::string message = get_str("message");
    std::string agent   = get_str("agent");
    int64_t conv_id     = get_int("conversation_id");
    if (phrase.empty() || message.empty()) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("required fields: schedule (NL phrase), message");
        write_json_response(fd, 400, err);
        return;
    }
    if (agent.empty()) agent = "index";

    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    ParseResult parsed = parse_schedule_phrase(phrase, now);
    if (!parsed.ok) {
        auto err = jobj();
        err->as_object_mut()["error"]      = jstr(parsed.error.message);
        err->as_object_mut()["accepted"]   = jstr(schedule_parser_help());
        write_json_response(fd, 400, err);
        return;
    }
    if (agent != "index") {
        auto rec = tenants.get_agent_record(tenant.id, agent);
        if (!rec) {
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr("agent '" + agent + "' not found in tenant catalog");
            write_json_response(fd, 404, err);
            return;
        }
    }

    std::string kind_str = (parsed.spec.kind == ScheduleSpec::Kind::Once)
        ? "once" : "recurring";
    auto row = tenants.create_scheduled_task(tenant.id, agent, conv_id,
        message, phrase, kind_str,
        parsed.spec.fire_at, parsed.spec.recur_json, parsed.spec.next_fire_at);
    auto out = jobj();
    out->as_object_mut()["scheduled_task"] = scheduled_task_to_json(row);
    out->as_object_mut()["normalized"]     = jstr(parsed.spec.normalized);
    write_json_response(fd, 201, out);
}

void handle_schedule_list(int fd, const HttpRequest& req,
                           TenantStore& tenants, const Tenant& tenant) {
    std::string status_filter;
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = req.path.substr(qpos + 1);
        // Tiny query-string parser — only one key we look at.
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp - i);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if (k == "status") status_filter = v;
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }
    auto rows = tenants.list_scheduled_tasks(tenant.id, status_filter, /*limit=*/200);
    auto arr = jarr();
    for (const auto& r : rows) arr->as_array_mut().push_back(scheduled_task_to_json(r));
    auto out = jobj();
    out->as_object_mut()["schedules"] = arr;
    write_json_response(fd, 200, out);
}

void handle_schedule_get(int fd, int64_t id,
                          TenantStore& tenants, const Tenant& tenant) {
    auto row = tenants.get_scheduled_task(tenant.id, id);
    if (!row) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("schedule not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["scheduled_task"] = scheduled_task_to_json(*row);
    write_json_response(fd, 200, out);
}

void handle_schedule_patch(int fd, int64_t id, const HttpRequest& req,
                            TenantStore& tenants, const Tenant& tenant) {
    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }
    auto sv = body->get("status");
    std::optional<std::string> status_opt;
    if (sv && sv->is_string()) {
        std::string s = sv->as_string();
        if (s != "active" && s != "paused" && s != "canceled" &&
            s != "completed" && s != "failed") {
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr("status must be one of: active, paused, canceled, completed, failed");
            write_json_response(fd, 400, err);
            return;
        }
        status_opt = s;
    }
    bool ok = tenants.update_scheduled_task(tenant.id, id,
        status_opt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
    if (!ok) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("schedule not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto row = tenants.get_scheduled_task(tenant.id, id);
    auto out = jobj();
    if (row) out->as_object_mut()["scheduled_task"] = scheduled_task_to_json(*row);
    write_json_response(fd, 200, out);
}

void handle_schedule_delete(int fd, int64_t id,
                             TenantStore& tenants, const Tenant& tenant) {
    bool ok = tenants.delete_scheduled_task(tenant.id, id);
    if (!ok) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("schedule not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["deleted"] = jbool(true);
    write_json_response(fd, 200, out);
}

void handle_schedule_runs(int fd, int64_t task_id, const HttpRequest& /*req*/,
                           TenantStore& tenants, const Tenant& tenant) {
    auto row = tenants.get_scheduled_task(tenant.id, task_id);
    if (!row) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("schedule not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto runs = tenants.list_task_runs(tenant.id, task_id, /*since=*/0, /*limit=*/100);
    auto arr = jarr();
    for (const auto& r : runs) arr->as_array_mut().push_back(task_run_to_json(r));
    auto out = jobj();
    out->as_object_mut()["runs"] = arr;
    write_json_response(fd, 200, out);
}

void handle_runs_list(int fd, const HttpRequest& req,
                       TenantStore& tenants, const Tenant& tenant) {
    int64_t since = 0;
    int64_t task_id = 0;
    auto qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = req.path.substr(qpos + 1);
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            std::string pair = qs.substr(i, amp - i);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if (k == "since")   try { since   = std::stoll(v); } catch (...) {}
                if (k == "task_id") try { task_id = std::stoll(v); } catch (...) {}
            }
            if (amp == std::string::npos) break;
            i = amp + 1;
        }
    }
    auto runs = tenants.list_task_runs(tenant.id, task_id, since, /*limit=*/200);
    auto arr = jarr();
    for (const auto& r : runs) arr->as_array_mut().push_back(task_run_to_json(r));
    auto out = jobj();
    out->as_object_mut()["runs"] = arr;
    write_json_response(fd, 200, out);
}

void handle_run_get(int fd, int64_t id, TenantStore& tenants, const Tenant& tenant) {
    auto run = tenants.get_task_run(tenant.id, id);
    if (!run) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("run not found");
        write_json_response(fd, 404, err);
        return;
    }
    auto out = jobj();
    out->as_object_mut()["run"] = task_run_to_json(*run);
    write_json_response(fd, 200, out);
}

// SSE notifications stream.  Long-lived connection; emits one
// `event: notification` block per Notification published on the bus
// for this tenant.  The handler subscribes on entry and unsubscribes
// on exit (RAII via Subscription guard).  Events queue in a per-handler
// mailbox to keep the publisher thread from blocking on slow clients.
void handle_notifications_stream(int fd, const Tenant& tenant,
                                   NotificationBus& bus) {
    // Open the SSE response.  CORS headers match the rest of the API so
    // browser fetch() / EventSource clients on a different origin (e.g.
    // a Vite dev server on :5173 dialling :8080) can subscribe.
    {
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/event-stream\r\n"
            << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            << "X-Accel-Buffering: no\r\n"
            << kCorsHeaders
            << "Connection: close\r\n"
            << "\r\n";
        write_all(fd, hdr.str());
    }

    std::mutex                     mb_mu;
    std::condition_variable        mb_cv;
    std::deque<Notification>       mailbox;
    std::atomic<bool>              client_alive{true};

    int64_t sub_id = bus.subscribe(tenant.id,
        [&mb_mu, &mb_cv, &mailbox](const Notification& n) {
            std::lock_guard<std::mutex> lk(mb_mu);
            mailbox.push_back(n);
            mb_cv.notify_one();
        });

    // Helper: serialize a string as a JSON-quoted token.  We piggyback on
    // json_serialize via jstr so escaping matches the rest of the API.
    auto qstr = [](const std::string& s) -> std::string {
        return json_serialize(*jstr(s));
    };

    // Initial hello so clients know the stream is open.
    {
        std::string hello = "event: open\ndata: {\"ok\":true}\n\n";
        write_all(fd, hello);
    }

    auto write_event = [&](const Notification& n) {
        std::ostringstream payload;
        payload << "{\"kind\":\"" << notification_kind_str(n.kind) << "\","
                << "\"task_id\":" << n.task_id << ","
                << "\"run_id\":"  << n.run_id  << ","
                << "\"agent_id\":" << qstr(n.agent_id) << ","
                << "\"status\":"   << qstr(n.status)   << ","
                << "\"started_at\":" << n.started_at << ","
                << "\"completed_at\":" << n.completed_at;
        if (!n.result_summary.empty())
            payload << ",\"result_summary\":" << qstr(n.result_summary);
        if (!n.error_message.empty())
            payload << ",\"error_message\":"  << qstr(n.error_message);
        payload << "}";
        std::string frame = "event: notification\ndata: " + payload.str() + "\n\n";
        write_all(fd, frame);
    };

    // Heartbeat every 30s so reverse proxies don't time the connection out.
    using clock = std::chrono::steady_clock;
    auto next_heartbeat = clock::now() + std::chrono::seconds(30);

    while (client_alive.load()) {
        Notification ev;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(mb_mu);
            mb_cv.wait_until(lk, next_heartbeat, [&]{ return !mailbox.empty(); });
            if (!mailbox.empty()) {
                ev = mailbox.front();
                mailbox.pop_front();
                have = true;
            }
        }
        if (have) {
            write_event(ev);
            // Crude liveness check — if the peer hung up, write_all returns
            // silently but a follow-up zero-byte send will surface EPIPE.
            // We accept best-effort here; the heartbeat path catches dead
            // peers within 30s anyway.
            continue;
        }
        const char* hb = ": heartbeat\n\n";
        write_all(fd, hb, std::strlen(hb));
        next_heartbeat = clock::now() + std::chrono::seconds(30);
        // The only way we actually exit this loop is the server-stop
        // handshake (the connection thread is detached and the read side
        // returns when the listen socket closes).  That's fine: scheduler
        // events drain via the mailbox until the parent terminates.
    }

    bus.unsubscribe(sub_id);
}

// Map an upstream provider's error_type to a fixed taxonomy of safe
// codes for SSE consumers.  We never proxy the provider's free-form
// `error.message` through to the tenant — that field can quote the
// offending Authorization header or other request data depending on
// the provider, and a future provider change would silently leak the
// runtime's shared API key to every tenant who triggered it.
//
// Operator-side stderr keeps the raw message; only the safe code and
// a fixed user-facing string ship over the wire.
const char* sanitised_provider_error_code(const std::string& error_type) {
    if (error_type == "authentication_error") return "auth_failed";
    if (error_type == "permission_error")     return "auth_failed";
    if (error_type == "rate_limit_error")     return "rate_limited";
    if (error_type == "overloaded_error")     return "rate_limited";
    if (error_type == "invalid_request_error")return "invalid_request";
    if (error_type == "not_found_error")      return "not_found";
    if (error_type == "request_too_large")    return "request_too_large";
    return "provider_error";
}

const char* sanitised_provider_error_message(const char* code) {
    if (std::strcmp(code, "auth_failed") == 0)
        return "the provider rejected the runtime's credentials";
    if (std::strcmp(code, "rate_limited") == 0)
        return "the provider is rate-limiting or overloaded — retry with backoff";
    if (std::strcmp(code, "invalid_request") == 0)
        return "the provider rejected the request shape";
    if (std::strcmp(code, "not_found") == 0)
        return "the configured model or resource is unavailable";
    if (std::strcmp(code, "request_too_large") == 0)
        return "the request exceeded the provider's size limit";
    return "the upstream provider returned an error";
}

// ── A2A JSON-RPC dispatch (PR-2: message/send synchronous) ─────────────────
//
// Writes a single JSON-RPC response envelope to the wire.  All A2A errors
// are reported as JSON-RPC error objects (HTTP 200 with `error.code`); we
// only emit non-200 HTTP for malformed envelopes that can't be answered
// in JSON-RPC form.  Sits below InFlightScope (defined above) because
// handle_a2a_message_send constructs one to receive cancel signals.

void write_a2a_rpc(int fd, const a2a::RpcResponse& r) {
    write_json_response(fd, 200, a2a::to_json(r));
}

// Resolve the inbound contextId.  When the client supplied one we echo
// it; otherwise we mint a fresh id so they can thread future requests.
// PR-4 will tie this back to the conversations table; for now contextId
// is ephemeral and not persisted.
std::string resolve_a2a_context_id(const a2a::Message& msg) {
    if (msg.context_id && !msg.context_id->empty()) return *msg.context_id;
    return new_request_id();
}

// ── Tool-callback factories ────────────────────────────────────────────────
//
// Each request — whether routed through /v1/orchestrate or the A2A
// surface (/v1/a2a/agents/:id) — needs the same tenant-scoped tool
// callbacks installed on its per-request Orchestrator.  Keeping the
// closure bodies here in named factories means handle_orchestrate and
// build_a2a_orchestrator share one source of truth; without this, the
// two paths drift the moment a callback's contract changes.
//
// Factories are TU-local and live in the outer anonymous namespace.
// Helpers they reference (mem_parse_id, format_ts_yyyymmdd,
// memory_relation_is_valid, sanitize_artifact_path, brave_search,
// kArtifactPerConversationMaxBytes) are declared earlier in this TU.

MemoryScratchpadInvoker make_memory_scratchpad_callback(int64_t tenant_id,
                                                          TenantStore* store) {
    return [tenant_id, store](const std::string& op,
                                const std::string& agent_id,
                                const std::string& args) -> std::string {
        // Per-agent ops use the calling agent's id; shared-* use the
        // empty scope key.  Output strings match the legacy file-based
        // responses so the model sees the same [/mem write] OK: ...
        // framing it always has.
        if (op == "read") {
            return store->read_scratchpad(tenant_id, agent_id);
        }
        if (op == "shared-read") {
            return store->read_scratchpad(tenant_id, std::string{});
        }
        if (op == "write") {
            int64_t sz = store->append_scratchpad(tenant_id, agent_id, args);
            return "OK: memory written (" + std::to_string(sz) +
                   " bytes total in scratchpad)";
        }
        if (op == "shared-write") {
            int64_t sz = store->append_scratchpad(tenant_id, std::string{}, args);
            return "OK (" + std::to_string(sz) + " bytes total)";
        }
        if (op == "clear") {
            store->clear_scratchpad(tenant_id, agent_id);
            return "OK: memory cleared";
        }
        if (op == "shared-clear") {
            store->clear_scratchpad(tenant_id, std::string{});
            return "OK";
        }
        return "ERR: unknown scratchpad op '" + op + "'";
    };
}

// Construct a per-request MCP Manager from `opts.mcp_servers_path`.
// Always returns a non-null Manager so callers can assume the invoker
// has something to call into; an empty registry simply yields a Manager
// with zero servers and the dispatcher renders a clear "no MCP servers
// configured" message on /mcp tools.  Registry-load failures are logged
// via `log_error` (typically the request's SSE error sink) and become
// the empty-Manager case.
std::shared_ptr<mcp::Manager> make_mcp_manager(
    const ApiServerOptions& opts,
    const std::function<void(const std::string&)>& log_error) {

    std::shared_ptr<mcp::Manager> mgr;
    if (!opts.mcp_servers_path.empty()) {
        try {
            auto specs = mcp::load_server_registry(opts.mcp_servers_path);
            mgr = std::make_shared<mcp::Manager>(std::move(specs));
        } catch (const std::exception& e) {
            if (log_error) log_error(std::string("MCP registry load failed: ") + e.what());
        }
    }
    if (!mgr) mgr = std::make_shared<mcp::Manager>(std::vector<mcp::ServerSpec>{});
    return mgr;
}

// Build a SchedulerInvoker bound to the calling tenant + conversation.
// Captures `tenants` by reference (the TenantStore outlives every
// request) and the integer ids by value so the lambda is self-contained.
// Renders user-facing tool-result bodies — the dispatcher wraps them in
// [/schedule …] / [END SCHEDULE] framing.
SchedulerInvoker make_scheduler_invoker_callback(
        TenantStore& tenants, int64_t tenant_id, int64_t conversation_id) {
    return [&tenants, tenant_id, conversation_id](
            const std::string& kind,
            const std::string& args,
            const std::string& caller_agent_id) -> std::string {
        auto trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
            return s;
        };

        const int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        if (kind == "list") {
            auto rows = tenants.list_scheduled_tasks(tenant_id,
                /*status_filter=*/"", /*limit=*/50);
            if (rows.empty()) return "(no scheduled tasks)";
            std::ostringstream out;
            out << rows.size() << " schedule(s):\n";
            for (const auto& r : rows) {
                out << "- #" << r.id << "  [" << r.status << "]  "
                    << r.schedule_phrase
                    << "  → " << r.agent_id;
                if (r.next_fire_at > 0) {
                    time_t t = static_cast<time_t>(r.next_fire_at);
                    char buf[32];
                    std::tm tm{};
                    localtime_r(&t, &tm);
                    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
                    out << "  (next: " << buf << ")";
                }
                if (r.run_count > 0) out << "  (runs: " << r.run_count << ")";
                out << "\n";
            }
            return out.str();
        }

        if (kind == "cancel" || kind == "pause" || kind == "resume") {
            std::string id_s = trim(args);
            if (!id_s.empty() && id_s.front() == '#') id_s.erase(0, 1);
            int64_t id = 0;
            try { id = std::stoll(id_s); } catch (...) { id = 0; }
            if (id <= 0) return "ERR: usage: /schedule " + kind + " <id>";

            if (kind == "cancel") {
                if (tenants.delete_scheduled_task(tenant_id, id)) {
                    return "OK: canceled #" + std::to_string(id);
                }
                return "ERR: schedule #" + std::to_string(id) + " not found";
            }

            // Pause or resume: PATCH status.  Resume also recomputes
            // next_fire_at for a recurring task whose previous fire is
            // now in the past.
            std::string new_status = (kind == "pause") ? "paused" : "active";
            std::optional<int64_t> next;
            if (kind == "resume") {
                auto row = tenants.get_scheduled_task(tenant_id, id);
                if (!row) return "ERR: schedule #" + std::to_string(id) + " not found";
                if (row->next_fire_at <= now) {
                    if (row->schedule_kind == "recurring") {
                        int64_t n = next_fire_for_recur(row->recur_json, now);
                        if (n > 0) next = n;
                    } else {
                        // One-shot whose fire time passed during the pause
                        // — fire on the next tick.
                        next = now + 1;
                    }
                }
            }
            bool ok = tenants.update_scheduled_task(tenant_id, id,
                std::optional<std::string>(new_status),
                next, std::nullopt, std::nullopt, std::nullopt);
            if (!ok) return "ERR: schedule #" + std::to_string(id) + " not found";
            return "OK: " + new_status + " #" + std::to_string(id);
        }

        // kind == "create": parse "<phrase>: <message>"
        std::string raw = trim(args);
        auto colon = raw.find(':');
        if (colon == std::string::npos) {
            return "ERR: missing ':' separator. Usage: /schedule <phrase>: <message>\n"
                   + schedule_parser_help();
        }
        std::string phrase  = trim(raw.substr(0, colon));
        std::string message = trim(raw.substr(colon + 1));
        if (phrase.empty() || message.empty()) {
            return "ERR: empty phrase or message. Usage: /schedule <phrase>: <message>";
        }

        ParseResult parsed = parse_schedule_phrase(phrase, now);
        if (!parsed.ok) {
            return "ERR: " + parsed.error.message + "\n" + schedule_parser_help();
        }

        // Determine the target agent.  Default = the caller; "index" if
        // the caller is unknown (defensive).
        std::string target_agent = caller_agent_id.empty() ? "index" : caller_agent_id;

        std::string kind_str = (parsed.spec.kind == ScheduleSpec::Kind::Once)
            ? "once" : "recurring";

        auto row = tenants.create_scheduled_task(tenant_id, target_agent,
            conversation_id, message, phrase, kind_str,
            parsed.spec.fire_at, parsed.spec.recur_json, parsed.spec.next_fire_at);

        std::ostringstream out;
        out << "OK: scheduled #" << row.id << " — " << parsed.spec.normalized
            << " → " << target_agent;
        if (conversation_id > 0) out << " (conversation #" << conversation_id << ")";
        out << "\n";
        out << "  message: " << message << "\n";
        return out.str();
    };
}

// Build a LessonInvoker bound to the calling tenant.  Lessons are
// scoped to (tenant, agent_id) — the calling agent's id captured at
// dispatch time is the owner of any newly-created lesson and the
// filter on read.  Same render style as /todo and /schedule.
LessonInvoker make_lesson_invoker_callback(
        TenantStore& tenants, int64_t tenant_id) {
    return [&tenants, tenant_id](
            const std::string& kind,
            const std::string& args,
            const std::string& caller_agent_id) -> std::string {
        auto trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
            return s;
        };

        const std::string owner =
            caller_agent_id.empty() ? "index" : caller_agent_id;

        auto render_one = [](const TenantStore::Lesson& l) {
            std::ostringstream out;
            out << "#" << l.id << "  ";
            if (!l.signature.empty()) out << "[" << l.signature << "]  ";
            out << l.lesson_text;
            if (l.hit_count > 0) out << "  (hits: " << l.hit_count << ")";
            return out.str();
        };

        if (kind == "list") {
            auto rows = tenants.list_lessons(tenant_id, owner, 25);
            if (rows.empty()) return "(no lessons recorded)";
            std::ostringstream out;
            out << rows.size() << " lesson(s):\n";
            for (auto& l : rows) out << "  " << render_one(l) << "\n";
            return out.str();
        }

        if (kind == "search") {
            std::string q = trim(args);
            if (q.empty()) return "ERR: usage: /lesson search <query>";
            auto rows = tenants.search_lessons(tenant_id, owner, q, 10);
            if (rows.empty()) return "(no matches)";
            std::ostringstream out;
            out << rows.size() << " match(es):\n";
            for (auto& l : rows) out << "  " << render_one(l) << "\n";
            return out.str();
        }

        if (kind == "delete") {
            std::string id_s = trim(args);
            if (!id_s.empty() && id_s.front() == '#') id_s.erase(0, 1);
            int64_t id = 0;
            try { id = std::stoll(id_s); } catch (...) { id = 0; }
            if (id <= 0) return "ERR: usage: /lesson delete <id>";
            if (!tenants.delete_lesson(tenant_id, id))
                return "ERR: lesson #" + std::to_string(id) + " not found";
            return "OK: deleted #" + std::to_string(id);
        }

        // Internal: preamble injection.  Looks up lessons that match
        // the upcoming user message heuristically (substring search on
        // signature + lesson_text), bumps `last_seen_at` for surfaced
        // rows so frequently-applicable lessons rise to the top, and
        // returns a renderable block.  Returns "(no lessons recorded)"
        // when the agent has none — the orchestrator skips injection.
        if (kind == "preamble") {
            // Cheap keyword extraction: grab unique alpha words >= 4 chars
            // from the first 400 chars of the prompt.  Search each one
            // and union the results, deduped by id.
            std::string prompt = args;
            if (prompt.size() > 400) prompt.resize(400);
            std::vector<std::string> keywords;
            {
                std::string cur;
                auto flush = [&]() {
                    if (cur.size() >= 4) {
                        bool dup = false;
                        for (auto& k : keywords) if (k == cur) { dup = true; break; }
                        if (!dup) keywords.push_back(cur);
                    }
                    cur.clear();
                };
                for (char c : prompt) {
                    if (std::isalpha(static_cast<unsigned char>(c))) {
                        cur.push_back(static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c))));
                    } else {
                        flush();
                    }
                    if (keywords.size() >= 12) break;
                }
                flush();
            }
            std::map<int64_t, TenantStore::Lesson> hits;
            for (auto& kw : keywords) {
                auto rows = tenants.search_lessons(tenant_id, owner, kw, 5);
                for (auto& r : rows) hits.emplace(r.id, r);
                if (hits.size() >= 8) break;
            }
            if (hits.empty()) return "(no lessons recorded)";

            // Sort by hit_count desc, last_seen_at desc; cap at 3.
            std::vector<TenantStore::Lesson> ranked;
            for (auto& [_, l] : hits) ranked.push_back(l);
            std::sort(ranked.begin(), ranked.end(),
                [](const TenantStore::Lesson& a, const TenantStore::Lesson& b){
                    if (a.hit_count != b.hit_count) return a.hit_count > b.hit_count;
                    return a.last_seen_at > b.last_seen_at;
                });
            if (ranked.size() > 3) ranked.resize(3);

            std::ostringstream out;
            out << "[KNOWN PITFALLS — your prior lessons]\n";
            for (auto& l : ranked) {
                out << "  - [" << l.signature << "] " << l.lesson_text
                    << " (#" << l.id << ")\n";
                tenants.bump_lesson_hit(tenant_id, l.id);
            }
            out << "[END KNOWN PITFALLS]";
            return out.str();
        }

        if (kind == "create") {
            // args carries either:
            //   "<signature>: <text>"           (single-line)
            //   "<signature>\n\n<body>"         (block form)
            std::string signature, lesson_text;
            auto blockSep = args.find("\n\n");
            if (blockSep != std::string::npos) {
                signature = trim(args.substr(0, blockSep));
                lesson_text = args.substr(blockSep + 2);
                while (!lesson_text.empty() && lesson_text.back() == '\n')
                    lesson_text.pop_back();
            } else {
                auto colon = args.find(':');
                if (colon == std::string::npos) {
                    return "ERR: /lesson <signature>: <text>  OR  block "
                           "form ending with /endlesson";
                }
                signature   = trim(args.substr(0, colon));
                lesson_text = trim(args.substr(colon + 1));
            }
            if (signature.empty() || lesson_text.empty()) {
                return "ERR: signature and lesson text are both required";
            }
            if (signature.size() > 200) {
                return "ERR: signature must be ≤ 200 chars";
            }
            if (lesson_text.size() > 4096) {
                return "ERR: lesson text must be ≤ 4096 chars";
            }
            auto l = tenants.create_lesson(tenant_id, owner, signature,
                                            lesson_text);
            std::ostringstream out;
            out << "OK: recorded lesson #" << l.id << " — " << render_one(l);
            return out.str();
        }

        return "ERR: unknown /lesson subcommand '" + kind + "'";
    };
}

// Build a TodoInvoker bound to the calling tenant + (optional) conversation.
// Captures by value/reference like the scheduler factory.  Returns
// user-facing tool-result bodies that the dispatcher wraps in
// [/todo …] / [END TODO] framing.
//
// "add" args shape:
//   "<subject>"                       — single-line, no body
//   "<subject>\n\n<body>"             — block-form with description
// (the writ dispatcher packs the body in for us when /endtodo is seen.)
TodoInvoker make_todo_invoker_callback(
        TenantStore& tenants, int64_t tenant_id, int64_t conversation_id) {
    return [&tenants, tenant_id, conversation_id](
            const std::string& kind,
            const std::string& args,
            const std::string& caller_agent_id) -> std::string {
        auto trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
            return s;
        };

        auto parse_id = [&](const std::string& s, int64_t& out) -> bool {
            std::string t = trim(s);
            if (!t.empty() && t.front() == '#') t.erase(0, 1);
            try { out = std::stoll(t); } catch (...) { return false; }
            return out > 0;
        };

        // Format a list block.  Pending + in_progress sit at the top in
        // append order; terminal rows are suppressed (use the HTTP
        // surface to browse the archive).
        auto render_list = [&](const std::vector<TenantStore::Todo>& rows) {
            if (rows.empty()) return std::string("(no todos)");
            std::ostringstream out;
            // Counts for the header line — quick at-a-glance status.
            int pending = 0, in_prog = 0;
            for (const auto& r : rows) {
                if (r.status == "pending")     ++pending;
                if (r.status == "in_progress") ++in_prog;
            }
            out << pending + in_prog << " open ("
                << in_prog << " in progress, " << pending << " pending):\n";
            for (const auto& r : rows) {
                if (r.status == "completed" || r.status == "canceled") continue;
                const char* mark = (r.status == "in_progress") ? "▶ " : "  ";
                out << mark << "#" << r.id << "  [" << r.status << "]  "
                    << r.subject;
                if (r.conversation_id == 0) out << "  (tenant-wide)";
                out << "\n";
            }
            return out.str();
        };

        if (kind == "list") {
            TenantStore::TodoFilter f;
            f.conversation_id = conversation_id;
            f.limit = 100;
            auto rows = tenants.list_todos(tenant_id, f);
            return render_list(rows);
        }

        if (kind == "add") {
            // Split subject (first segment) from body (after \n\n).
            std::string subject, description;
            auto sep = args.find("\n\n");
            if (sep == std::string::npos) {
                subject = trim(args);
            } else {
                subject     = trim(args.substr(0, sep));
                description = args.substr(sep + 2);
                while (!description.empty() && description.back() == '\n')
                    description.pop_back();
            }
            if (subject.empty()) return "ERR: /todo add requires a subject";
            std::string owner = caller_agent_id.empty() ? "index" : caller_agent_id;
            auto row = tenants.create_todo(tenant_id, conversation_id,
                                            owner, subject, description);
            std::ostringstream out;
            out << "OK: added #" << row.id << " — " << row.subject;
            if (!description.empty()) out << "\n  description: " << description;
            return out.str();
        }

        if (kind == "start" || kind == "done" || kind == "cancel") {
            int64_t id = 0;
            if (!parse_id(args, id)) return "ERR: usage: /todo " + kind + " <id>";
            std::string new_status =
                kind == "start"  ? "in_progress" :
                kind == "done"   ? "completed"   :
                                   "canceled";
            bool ok = tenants.update_todo(tenant_id, id,
                std::nullopt, std::nullopt,
                std::optional<std::string>(new_status),
                std::nullopt, std::nullopt);
            if (!ok) return "ERR: todo #" + std::to_string(id) + " not found";
            return "OK: " + new_status + " #" + std::to_string(id);
        }

        if (kind == "delete") {
            int64_t id = 0;
            if (!parse_id(args, id)) return "ERR: usage: /todo delete <id>";
            if (!tenants.delete_todo(tenant_id, id))
                return "ERR: todo #" + std::to_string(id) + " not found";
            return "OK: deleted #" + std::to_string(id);
        }

        if (kind == "describe" || kind == "subject") {
            // Both share the shape "<id>: <text>".  Parse the colon
            // and dispatch to the matching column.
            auto colon = args.find(':');
            if (colon == std::string::npos)
                return "ERR: usage: /todo " + kind + " <id>: <text>";
            int64_t id = 0;
            if (!parse_id(args.substr(0, colon), id))
                return "ERR: bad todo id";
            std::string text = trim(args.substr(colon + 1));
            std::optional<std::string> subj_opt;
            std::optional<std::string> desc_opt;
            if (kind == "subject")  subj_opt = text;
            if (kind == "describe") desc_opt = text;
            bool ok = tenants.update_todo(tenant_id, id,
                subj_opt, desc_opt, std::nullopt, std::nullopt, std::nullopt);
            if (!ok) return "ERR: todo #" + std::to_string(id) + " not found";
            return "OK: updated #" + std::to_string(id);
        }

        return "ERR: unknown /todo subcommand '" + kind + "'";
    };
}

MCPInvoker make_mcp_invoker_callback(std::shared_ptr<mcp::Manager> mcp_mgr) {
    return [mcp_mgr](const std::string& kind, const std::string& args) -> std::string {
        // /mcp tools  [server]
        // /mcp call   <server> <tool> [json_args]
        //
        // The /mcp slash dispatcher in commands.cpp normalises `kind`
        // to "tools" or "call" and hands us the rest of the line as
        // `args`.  We're responsible for parsing args and rendering
        // the response body.
        auto trim = [](std::string s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
            return s;
        };

        if (kind == "tools") {
            std::string server = trim(args);
            auto names = mcp_mgr->server_names();
            if (names.empty()) {
                return "(no MCP servers configured for this deployment — "
                       "set mcp_servers_path in ApiServerOptions and add an entry "
                       "in the registry JSON.  See docs/api/concepts/mcp.md.)\n";
            }
            std::ostringstream out;
            std::vector<std::string> targets;
            if (server.empty()) {
                targets = names;
            } else if (mcp_mgr->has(server)) {
                targets.push_back(server);
            } else {
                out << "ERR: no MCP server '" << server << "' configured. "
                    << "Available: ";
                for (size_t i = 0; i < names.size(); ++i) {
                    if (i) out << ", ";
                    out << names[i];
                }
                out << "\n";
                return out.str();
            }
            for (auto& s : targets) {
                out << "[" << s << "]\n";
                try {
                    auto& cli = mcp_mgr->client(s);
                    auto& tools = cli.tools();
                    if (tools.empty()) {
                        out << "  (server returned no tools)\n";
                    }
                    for (auto& t : tools) {
                        out << "  " << t.name;
                        if (!t.description.empty()) {
                            // First line of description only — the full
                            // schema is verbose enough that an agent
                            // querying tools should follow up with a
                            // narrower /mcp tools <server> if it
                            // already knows the namespace.
                            std::string d = t.description;
                            auto nl = d.find('\n');
                            if (nl != std::string::npos) d.resize(nl);
                            if (d.size() > 120) { d.resize(117); d += "..."; }
                            out << " — " << d;
                        }
                        out << "\n";
                    }
                } catch (const std::exception& e) {
                    out << "  ERR: " << e.what() << "\n";
                }
            }
            return out.str();
        }

        if (kind == "call") {
            // Parse: <server> <tool> [json_args]
            std::istringstream iss(args);
            std::string server, tool;
            iss >> server >> tool;
            std::string json_args;
            std::getline(iss, json_args);
            json_args = trim(json_args);

            if (server.empty() || tool.empty()) {
                return "ERR: usage: /mcp call <server> <tool> [json_args]\n";
            }
            if (!mcp_mgr->has(server)) {
                auto names = mcp_mgr->server_names();
                std::ostringstream out;
                out << "ERR: no MCP server '" << server << "' configured.";
                if (!names.empty()) {
                    out << " Available: ";
                    for (size_t i = 0; i < names.size(); ++i) {
                        if (i) out << ", ";
                        out << names[i];
                    }
                }
                out << "\n";
                return out.str();
            }

            std::shared_ptr<JsonValue> arg_obj;
            if (!json_args.empty()) {
                try { arg_obj = json_parse(json_args); }
                catch (const std::exception& e) {
                    return std::string("ERR: invalid JSON args: ") + e.what() + "\n";
                }
                if (!arg_obj || !arg_obj->is_object()) {
                    return "ERR: tool args must be a JSON object (e.g. "
                           "{\"url\":\"https://example.com\"})\n";
                }
            }

            try {
                auto& cli = mcp_mgr->client(server);
                auto result = cli.call_tool(tool, arg_obj);
                return mcp::render_tool_result(result);
            } catch (const std::exception& e) {
                return std::string("ERR: ") + e.what() + "\n";
            }
        }

        return "ERR: unknown /mcp subcommand";
    };
}

// Returns nullptr when no SandboxManager is wired into opts.  The
// dispatcher then falls back to its existing exec_disabled gate, so
// /exec returns the same clean ERR the SaaS path used before sandbox
// support landed.  When wired, the closure binds the manager + the
// request's tenant_id so concurrent requests for two tenants land in
// two distinct per-tenant containers.
ExecInvoker make_exec_invoker_callback(const ApiServerOptions& opts,
                                        int64_t tenant_id) {
    SandboxManager* mgr = opts.sandbox;
    if (!mgr) return nullptr;
    return [mgr, tenant_id](const std::string& cmd) -> std::string {
        SandboxExecResult r = mgr->exec(tenant_id, cmd);
        return r.output;
    };
}

// Returns nullptr when the configured provider isn't supported or no
// API key is set — the dispatcher then surfaces its own "search
// unavailable" ERR with a more useful message than this layer can
// generate.
SearchInvoker make_search_invoker_callback(const ApiServerOptions& opts) {
    const std::string provider = opts.search_provider.empty()
                                    ? std::string("brave")
                                    : opts.search_provider;
    const std::string key      = opts.search_api_key;
    if (provider == "brave" && !key.empty()) {
        return [key](const std::string& query, int top_n) -> std::string {
            return brave_search(query, key, top_n);
        };
    }
    if (!provider.empty() && provider != "brave" && !key.empty()) {
        return [provider](const std::string&, int) -> std::string {
            return "ERR: search provider '" + provider +
                   "' is configured but not implemented in this "
                   "build.  Only 'brave' is supported in v1.";
        };
    }
    return nullptr;
}

// Artifact-store callbacks — only meaningful when conversation_id > 0.
// /v1/orchestrate without a thread (the legacy raw form) leaves these
// null on purpose: /write --persist falls back to ephemeral SSE-only
// capture and /read / /list return ERR with a clear "no conversation
// context" hint.

ArtifactWriter make_artifact_writer_callback(int64_t tenant_id,
                                              int64_t conversation_id,
                                              TenantStore* store) {
    return [tenant_id, conversation_id, store](const std::string& raw_path,
                                                 const std::string& content) -> std::string {
        std::string err;
        auto canonical = sanitize_artifact_path(raw_path, err);
        if (!canonical) {
            return std::string("ERR: invalid path: ") + err;
        }
        // mime_type stays default ('application/octet-stream') — the
        // agent doesn't know what to declare and we don't sniff in v1.
        // HTTP callers can set it explicitly via the POST endpoint.
        auto put = store->put_artifact(tenant_id, conversation_id, *canonical,
                                         content, std::string{});
        std::ostringstream out;
        switch (put.status) {
            case PutArtifactResult::Status::Created:
            case PutArtifactResult::Status::Updated:
                out << (put.status == PutArtifactResult::Status::Created
                        ? "OK: persisted "
                        : "OK: updated ")
                    << put.record->size << " bytes (artifact #"
                    << put.record->id << ", "
                    << put.conversation_used_bytes << " of "
                    << kArtifactPerConversationMaxBytes
                    << " bytes used in this conversation)";
                break;
            case PutArtifactResult::Status::QuotaExceeded:
                out << "ERR: " << put.error_msg;
                break;
            case PutArtifactResult::Status::PathRejected:
                out << "ERR: " << put.error_msg;
                break;
        }
        return out.str();
    };
}

ArtifactReader make_artifact_reader_callback(int64_t tenant_id,
                                              int64_t conversation_id,
                                              TenantStore* store) {
    auto err = [](std::string msg) {
        ArtifactReadResult r;
        r.body = std::move(msg);
        return r;
    };
    return [tenant_id, conversation_id, store, err](
               const std::string& raw_path,
               int64_t artifact_id,
               int64_t via_memory_id) -> ArtifactReadResult {
        // Path-form: same-conversation lookup.  Sanitiser gates bad
        // paths before we touch the DB.
        if (artifact_id == 0) {
            std::string perr;
            auto canonical = sanitize_artifact_path(raw_path, perr);
            if (!canonical) return err("ERR: invalid path: " + perr);

            auto meta = store->get_artifact_meta_by_path(tenant_id,
                                                          conversation_id, *canonical);
            if (!meta) {
                return err("ERR: '" + *canonical +
                           "' not found in this conversation's artifacts");
            }
            auto blob = store->get_artifact_content(tenant_id, meta->id);
            if (!blob) {
                return err("ERR: artifact #" + std::to_string(meta->id) +
                           " content missing");
            }
            ArtifactReadResult r;
            r.body       = std::move(*blob);
            r.media_type = meta->mime_type;
            return r;
        }

        // Id-form: tenant-scoped lookup.  Cross-conversation reads
        // require a `via=mem:<id>` capability that points at this
        // artifact_id from a memory entry the tenant owns.  Same-
        // conversation reads are allowed without citation — same trust
        // boundary as path-form.
        auto art = store->get_artifact_meta(tenant_id, artifact_id);
        if (!art) {
            return err("ERR: artifact #" + std::to_string(artifact_id) +
                       " not found for this tenant");
        }
        if (art->conversation_id != conversation_id) {
            if (via_memory_id == 0) {
                return err("ERR: artifact #" + std::to_string(artifact_id) +
                           " is in a different conversation; cite the "
                           "memory entry that links it: "
                           "/read #" + std::to_string(artifact_id) +
                           " via=mem:<entry_id>");
            }
            auto mem = store->get_entry(tenant_id, via_memory_id);
            if (!mem) {
                return err("ERR: via=mem:" + std::to_string(via_memory_id) +
                           " — memory entry not found for this tenant");
            }
            if (mem->artifact_id != artifact_id) {
                return err("ERR: memory entry #" +
                           std::to_string(via_memory_id) +
                           " does not reference artifact #" +
                           std::to_string(artifact_id) +
                           " (its artifact_id=" +
                           std::to_string(mem->artifact_id) + ")");
            }
        }
        auto blob = store->get_artifact_content(tenant_id, artifact_id);
        if (!blob) {
            return err("ERR: artifact #" + std::to_string(artifact_id) +
                       " content missing");
        }
        ArtifactReadResult r;
        r.body       = std::move(*blob);
        r.media_type = art->mime_type;
        return r;
    };
}

ArtifactLister make_artifact_lister_callback(int64_t tenant_id,
                                              int64_t conversation_id,
                                              TenantStore* store) {
    return [tenant_id, conversation_id, store]() -> std::string {
        auto rows = store->list_artifacts_conversation(tenant_id,
                                                         conversation_id, 200);
        if (rows.empty()) return std::string{};
        std::ostringstream out;
        for (auto& r : rows) {
            out << r.path << "  (" << r.size
                << " bytes, mime=" << r.mime_type
                << ", id=" << r.id << ")\n";
        }
        return out.str();
    };
}

// Structured-memory reader / writer.  These two callbacks are the
// largest in arbiter (the reader alone is ~640 lines of search /
// formatting / ranking).  Lifted into named factories so both
// /v1/orchestrate and the A2A handlers can install identical
// behavior without 900 lines of duplication.

StructuredMemoryReader make_structured_memory_reader_callback(
    TenantStore& tenants, int64_t reader_tenant_id,
    int64_t reader_conversation_id, Orchestrator* orch_ptr) {
    return
        [&tenants, reader_tenant_id, reader_conversation_id, orch_ptr]
        (const std::string& kind, const std::string& args,
         const std::string& caller_id) -> std::string {
            // Helper formatters reused across kinds.
            auto fmt_tags = [](const std::string& tags_json) -> std::string {
                try {
                    auto v = json_parse(tags_json);
                    if (!v || !v->is_array() || v->as_array().empty()) return "";
                    std::string out = " [";
                    bool first = true;
                    for (auto& t : v->as_array()) {
                        if (!t || !t->is_string()) continue;
                        if (!first) out += ", ";
                        out += t->as_string();
                        first = false;
                    }
                    out += "]";
                    return out;
                } catch (...) { return ""; }
            };
            auto fmt_entry_line = [&](const MemoryEntry& e) -> std::string {
                std::ostringstream l;
                l << "- #" << e.id << "  [" << e.type << "]";
                // Surface the entry's authored date when present.  Lets
                // the agent reason about recency and "what was true
                // when" without an extra /mem entry round trip.  Same
                // YYYY-MM-DD form the reranker sees in its prompt.
                std::string ts = format_ts_yyyymmdd(e.created_at);
                if (!ts.empty()) l << " (" << ts;
                if (e.valid_to > 0) {
                    if (ts.empty()) l << " (";
                    else            l << " · ";
                    l << "superseded";
                    std::string inv = format_ts_yyyymmdd(e.valid_to);
                    if (!inv.empty()) l << " " << inv;
                }
                if (!ts.empty() || e.valid_to > 0) l << ")";
                l << "  " << e.title << fmt_tags(e.tags_json);
                if (!e.source.empty()) l << "  (source: " << e.source << ")";
                return l.str();
            };

            // Helper: case-insensitive substring count for ranking.
            auto ci_count = [](const std::string& hay, const std::string& needle) -> int {
                if (needle.empty() || hay.empty() || needle.size() > hay.size()) return 0;
                int n = 0;
                for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                    bool match = true;
                    for (size_t j = 0; j < needle.size(); ++j) {
                        char a = hay[i + j], b = needle[j];
                        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                        if (a != b) { match = false; break; }
                    }
                    if (match) ++n;
                }
                return n;
            };
            // Helper: split a query into whitespace-separated terms; used by
            // /mem search so multi-word queries score by the sum of per-term
            // hits, giving "more terms matched" a higher rank than a single
            // long substring match.
            auto split_terms = [](const std::string& q) -> std::vector<std::string> {
                std::vector<std::string> out;
                size_t i = 0;
                while (i < q.size()) {
                    while (i < q.size() && (q[i] == ' ' || q[i] == '\t')) ++i;
                    size_t j = i;
                    while (j < q.size() && q[j] != ' ' && q[j] != '\t') ++j;
                    if (j > i) out.push_back(q.substr(i, j - i));
                    i = j;
                }
                return out;
            };

            if (kind == "pipeline-entries") {
                // Internal probe used by the orchestrator's delegation
                // path to seed sub-agents with a "what did siblings just
                // write?" snapshot.  Distinct from agent-facing /mem
                // entries (which is unscoped tenant-wide) because:
                //   • Tenant-wide bleed exposes residue from PRIOR runs
                //     of the same scenario, encouraging the sub-agent
                //     to paraphrase old entries as if they were fresh
                //     siblings' output.
                //   • Pipeline memory needs to be cheap (one DB call
                //     per /agent invocation), so the cap is small.
                // Conversation-scoped + recent-N keeps the snapshot
                // tight: only what *this* turn's siblings produced.
                if (reader_conversation_id <= 0) {
                    // No conversation context (raw /v1/orchestrate, CLI).
                    // Without a conversation we can't isolate "this run"
                    // entries, so return empty rather than dumping the
                    // tenant-wide history into delegation context.
                    return "(no entries)";
                }
                TenantStore::EntryFilter f;
                f.limit = 15;
                f.conversation_id = reader_conversation_id;
                auto entries = tenants.list_entries(reader_tenant_id, f);
                if (entries.empty()) return "(no entries)";
                std::ostringstream out;
                out << entries.size() << " entries (newest first):\n";
                for (auto& e : entries) out << fmt_entry_line(e) << "\n";
                return out.str();
            }

            if (kind == "entries") {
                // /mem entries [<type>[,<type>...]]
                // /mem entries tag=<tagname>
                // The bare-arg comma list is preserved for backward compat;
                // the `tag=` form lets agents pull a curated subset by the
                // facet they're most likely to organise around.
                TenantStore::EntryFilter f;
                f.limit  = 100;

                std::string a = args;
                while (!a.empty() && a.front() == ' ') a.erase(0, 1);
                while (!a.empty() && a.back()  == ' ') a.pop_back();

                if (a.size() > 4 && a.compare(0, 4, "tag=") == 0) {
                    f.tag = a.substr(4);
                    while (!f.tag.empty() && f.tag.front() == ' ') f.tag.erase(0, 1);
                    while (!f.tag.empty() && f.tag.back()  == ' ') f.tag.pop_back();
                    if (f.tag.empty()) return "ERR: usage: /mem entries tag=<tagname>";
                } else if (!a.empty()) {
                    // Comma-sep type filter.
                    size_t start = 0;
                    while (start <= a.size()) {
                        size_t comma = a.find(',', start);
                        std::string tok = a.substr(
                            start, comma == std::string::npos ? std::string::npos
                                                              : comma - start);
                        while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
                        while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
                        if (!tok.empty()) f.types.push_back(tok);
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
                auto entries = tenants.list_entries(reader_tenant_id, f);
                if (entries.empty()) {
                    if (!f.tag.empty())
                        return "(no entries tagged '" + f.tag + "')";
                    return "(no entries)";
                }
                std::ostringstream out;
                out << entries.size() << " entries (newest first):\n";
                for (auto& e : entries) out << fmt_entry_line(e) << "\n";
                return out.str();
            }

            if (kind == "entry") {
                int64_t id = mem_parse_id(args);
                if (id <= 0) return "ERR: usage: /mem entry <id>";
                auto e = tenants.get_entry(reader_tenant_id, id);
                if (!e)
                    return "ERR: entry " + std::to_string(id) + " not found";
                std::ostringstream out;
                out << fmt_entry_line(*e) << "\n";
                if (!e->content.empty()) out << "\n" << e->content << "\n";
                // Linked artifact: surface metadata and the literal
                // /read command that grants access.  Same-conversation
                // artifacts can be read by path or by id without a via=
                // clause; cross-conversation artifacts require the
                // memory citation, and the suggested command bakes it
                // in so the agent can copy verbatim.
                if (e->artifact_id > 0) {
                    auto art = tenants.get_artifact_meta(reader_tenant_id,
                                                          e->artifact_id);
                    if (art) {
                        out << "\nlinked artifact:\n";
                        out << "  #" << art->id << "  " << art->path
                            << "  (" << art->size << " bytes, mime="
                            << art->mime_type << ")\n";
                        if (art->conversation_id == reader_conversation_id) {
                            out << "  fetch with: /read " << art->path
                                << "   (or /read #" << art->id << ")\n";
                        } else {
                            out << "  cross-conversation — fetch with: "
                                << "/read #" << art->id
                                << " via=mem:" << e->id << "\n";
                        }
                    } else {
                        out << "\nlinked artifact:\n"
                            << "  (link expired — artifact #"
                            << e->artifact_id << " no longer exists)\n";
                    }
                }
                // Edges: relations where this entry is source OR target.
                // Resolve neighbour titles in a small ad-hoc cache so an
                // entry with N edges to the same neighbour doesn't
                // N-times-fetch the same row.
                auto out_edges = tenants.list_relations(reader_tenant_id, id, 0,
                                                        std::string{}, 200);
                auto in_edges  = tenants.list_relations(reader_tenant_id, 0, id,
                                                        std::string{}, 200);
                std::map<int64_t, std::pair<std::string, std::string>> title_cache;
                auto resolve = [&](int64_t nid) -> std::pair<std::string, std::string> {
                    auto it = title_cache.find(nid);
                    if (it != title_cache.end()) return it->second;
                    auto neighbour = tenants.get_entry(reader_tenant_id, nid);
                    if (!neighbour) {
                        title_cache[nid] = {"(unavailable)", ""};
                    } else {
                        title_cache[nid] = {neighbour->title, neighbour->type};
                    }
                    return title_cache[nid];
                };
                if (!out_edges.empty()) {
                    out << "\noutgoing:\n";
                    for (auto& r : out_edges) {
                        auto [title, type] = resolve(r.target_id);
                        out << "  --[" << r.relation << "]--> #" << r.target_id
                            << "  " << title;
                        if (!type.empty()) out << " (" << type << ")";
                        out << "\n";
                    }
                }
                if (!in_edges.empty()) {
                    out << "\nincoming:\n";
                    for (auto& r : in_edges) {
                        auto [title, type] = resolve(r.source_id);
                        out << "  #" << r.source_id << "  " << title;
                        if (!type.empty()) out << " (" << type << ")";
                        out << "  --[" << r.relation << "]-->\n";
                    }
                }
                return out.str();
            }

            if (kind == "search") {
                // FTS5 + Okapi-BM25 ranking via TenantStore.  When the
                // request is part of a conversation, search runs through
                // search_entries_graduated: conversation-scoped hits
                // come first (locality bias), tenant-wide hits fill out
                // the page if conversation-scoped didn't reach the cap.
                // Top 3 by rank get their content excerpted inline so a
                // single /mem search resolves into something the agent
                // can actually read without follow-up /mem entry calls.
                //
                // Optional `--rerank` flag routes the top-10 candidates
                // through the calling agent's advisor_model for a final
                // reorder.  Costs one LLM call; only worth it on
                // ambiguous queries where BM25 produces close-scored
                // candidates that need semantic disambiguation.

                // Strip --rerank from anywhere in the args; remaining
                // text is the query.  Multiple instances are tolerated
                // (rare, but someone'll do it).
                std::string q = args;
                bool rerank = false;
                {
                    const std::string flag = "--rerank";
                    size_t p;
                    while ((p = q.find(flag)) != std::string::npos) {
                        rerank = true;
                        size_t end = p + flag.size();
                        // Eat one trailing space so we don't leave a
                        // double-space in the middle.
                        if (end < q.size() && q[end] == ' ') ++end;
                        size_t begin = p;
                        if (begin > 0 && q[begin - 1] == ' ') --begin;
                        q.erase(begin, end - begin);
                    }
                }
                while (!q.empty() && q.front() == ' ') q.erase(0, 1);
                while (!q.empty() && q.back()  == ' ') q.pop_back();
                if (q.empty()) return "ERR: usage: /mem search <query> [--rerank]";

                // Rerank widens the candidate pool internally (25)
                // and trims to a smaller visible cap (10) after the
                // advisor reorders.  The pool gives the reranker
                // headroom to promote real matches from positions
                // 11..25; the visible cap keeps the agent's reply
                // tractable.  Non-rerank path stays at 50 (the
                // renderer's natural cap).
                static constexpr int kAgentRerankPool   = 25;
                static constexpr int kAgentRerankReturn = 10;

                // Pull the caller's Constitution so we can consult its
                // memory-enrichment toggles.  get_constitution throws
                // out_of_range for unknown ids; the master "index" id
                // is always loaded, and any caller that reached this
                // closure was registered via Orchestrator's run loop.
                // If somehow we get a stale id we fall through to the
                // pre-config behavior — defensive in_try block, no
                // user-visible error for what is at worst a missed
                // optimization.
                Constitution::MemoryConfig mc;
                std::string caller_advisor_model;
                try {
                    const auto& cc = orch_ptr->get_constitution(caller_id);
                    mc = cc.memory;
                    caller_advisor_model = cc.advisor.model;
                    if (caller_advisor_model.empty())
                        caller_advisor_model = cc.advisor_model;
                } catch (...) {
                    // mc keeps defaults — search still runs, just
                    // without the per-agent enrichment.
                }

                TenantStore::EntryFilter f;
                f.q               = q;
                f.conversation_id = reader_conversation_id;
                f.limit           = rerank ? kAgentRerankPool : 50;

                // Age-decay ranking factor.  When the agent has
                // `memory.age_decay` on (default), pass a `now` epoch
                // to the storage layer so the search SQL multiplies
                // BM25 scores by a piecewise factor of (now -
                // valid_from).  Half-life and floor come from
                // MemoryConfig.  Disabling decay means the agent gets
                // pre-decay ranking (older entries score the same as
                // fresh ones on identical query matches).
                if (mc.age_decay) {
                    f.age_now_epoch = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        ).count());
                    f.age_half_life_days = mc.age_half_life_days;
                    f.age_floor          = mc.age_floor;
                }

                // Question-intent routing — heuristic, no LLM cost.
                // Soft-boost types matching the question's intent (the
                // existing 1.3x BM25 multiplier) so a "preference"
                // query surfaces user/feedback rows above context
                // rows.  Caller-supplied f.types would override, but
                // the agent path doesn't accept type filters today —
                // f.types is always empty here.
                if (mc.intent_routing) {
                    f.types = classify_question_intent(q);
                }

                // Closure that runs one query through the same FTS
                // path, used both for the original and (when
                // search_expand is on) for paraphrases.  Preserves the
                // intent-routing per variant — paraphrases that shift
                // the question shape get appropriate type boosts.
                auto run_one = [&](const std::string& query_str)
                    -> std::vector<MemoryEntry> {
                    TenantStore::EntryFilter ef = f;
                    ef.q = query_str;
                    if (mc.intent_routing && ef.types.empty()) {
                        ef.types = classify_question_intent(query_str);
                    }
                    return (reader_conversation_id > 0)
                        ? tenants.search_entries_graduated(reader_tenant_id, ef)
                        : tenants.list_entries(reader_tenant_id, ef);
                };

                std::vector<MemoryEntry> entries;
                std::string expand_note;
                std::vector<std::string> expansion_used;
                if (mc.search_expand && !caller_advisor_model.empty()) {
                    // Query reformulation.  Agent's advisor model
                    // generates 2 paraphrases; we run all 3 variants
                    // through FTS and RRF-fuse the rankings.
                    // No-embedding alternative to dense retrieval —
                    // closes the gap on paraphrased queries while
                    // staying inside the lean-binary constraint.
                    auto expander = orch_ptr->make_advisor_invoker(caller_id);
                    auto exp = expand_query_with_advisor(expander, q);
                    expansion_used = exp.queries;
                    if (!exp.note.empty()) expand_note = exp.note;

                    if (!expansion_used.empty()) {
                        std::vector<std::vector<MemoryEntry>> rankings;
                        std::vector<double> weights;
                        rankings.push_back(run_one(q));
                        weights.push_back(1.0);
                        for (auto& pq : expansion_used) {
                            rankings.push_back(run_one(pq));
                            weights.push_back(0.7);
                        }
                        const int fused_limit = f.limit;
                        entries = rrf_fuse_rankings(std::move(rankings),
                                                     weights, fused_limit);
                    } else {
                        // Expansion failed — single-query fallback.
                        entries = run_one(q);
                    }
                } else {
                    entries = run_one(q);
                }

                if (entries.empty()) {
                    return "(no entries match '" + q + "')";
                }

                std::string rerank_note;
                if (rerank && entries.size() > 1) {
                    auto advisor = orch_ptr->make_advisor_invoker(caller_id);
                    auto rr = rerank_with_advisor(advisor, q, std::move(entries));
                    entries = std::move(rr.entries);
                    if (!rr.note.empty()) rerank_note = rr.note + "\n";
                }
                // Trim the wider pool back to the visible cap, whether
                // rerank applied or not — caller asked for the rerank
                // path, the pool was internal to that.
                if (rerank && entries.size() >
                              static_cast<size_t>(kAgentRerankReturn)) {
                    entries.resize(
                        static_cast<size_t>(kAgentRerankReturn));
                }

                std::ostringstream out;
                if (!rerank_note.empty()) out << rerank_note;
                if (!expand_note.empty()) out << expand_note << "\n";
                if (!expansion_used.empty()) {
                    // Tell the agent which paraphrases the orchestrator
                    // also searched.  Useful for debugging "why did I
                    // get back this set?" — and lets the agent see the
                    // recall surface its memory config opted into.
                    out << "(also searched: ";
                    for (size_t i = 0; i < expansion_used.size(); ++i) {
                        if (i) out << " | ";
                        out << "'" << expansion_used[i] << "'";
                    }
                    out << ")\n";
                }
                out << entries.size() << " match"
                    << (entries.size() == 1 ? "" : "es")
                    << " for '" << q << "' (top by relevance):\n";

                static constexpr size_t kInlineTopN  = 3;
                static constexpr size_t kExcerptBytes = 480;
                for (size_t i = 0; i < entries.size(); ++i) {
                    const auto& e = entries[i];
                    out << fmt_entry_line(e);
                    // Mark conversation-scoped hits so the agent can tell
                    // local context from broader tenant memory at a glance.
                    if (reader_conversation_id > 0 &&
                        e.conversation_id == reader_conversation_id) {
                        out << "  [conversation]";
                    }
                    out << "\n";
                    if (i < kInlineTopN && !e.content.empty()) {
                        std::string excerpt = e.content;
                        if (excerpt.size() > kExcerptBytes) {
                            excerpt.resize(kExcerptBytes);
                            excerpt += " ...";
                        }
                        std::ostringstream indented;
                        size_t start = 0;
                        while (start < excerpt.size()) {
                            size_t nl = excerpt.find('\n', start);
                            indented << "    | "
                                     << excerpt.substr(start,
                                          nl == std::string::npos
                                              ? std::string::npos
                                              : nl - start)
                                     << "\n";
                            if (nl == std::string::npos) break;
                            start = nl + 1;
                        }
                        out << indented.str();
                    }
                }
                return out.str();
            }

            if (kind == "expand") {
                // /mem expand <id> [depth=N]
                // BFS the subgraph around <id> up to depth N (max 2,
                // default 1), capped at 50 nodes total.  One round trip
                // for what would otherwise be N+1 sequential /mem entry
                // calls.  Renders a tree-ish structure: seed → 1-hop →
                // 2-hop, with the relation labels on each edge.
                std::string a = args;
                while (!a.empty() && a.front() == ' ') a.erase(0, 1);
                while (!a.empty() && a.back()  == ' ') a.pop_back();
                if (a.empty()) {
                    return "ERR: usage: /mem expand <id> [depth=N]";
                }
                int64_t seed_id = 0;
                int depth = 1;
                {
                    // Split on whitespace so the first token can be
                    // run through mem_parse_id (which tolerates a leading '#').
                    // Any subsequent tokens can carry depth=N.
                    std::istringstream iss(a);
                    std::string id_tok;
                    iss >> id_tok;
                    seed_id = mem_parse_id(id_tok);
                    std::string flag;
                    if (iss >> flag) {
                        const std::string p = "depth=";
                        if (flag.compare(0, p.size(), p) == 0) {
                            try { depth = std::stoi(flag.substr(p.size())); }
                            catch (...) { depth = 1; }
                        }
                    }
                }
                if (seed_id <= 0) return "ERR: bad seed id";
                if (depth < 1) depth = 1;
                if (depth > 2) depth = 2;

                auto seed = tenants.get_entry(reader_tenant_id, seed_id);
                if (!seed)
                    return "ERR: entry " + std::to_string(seed_id) + " not found";

                // BFS with a 50-node cap; node order tracks discovery so
                // deduped neighbours render under the closer hop.
                static constexpr size_t kMaxNodes = 50;
                std::map<int64_t, MemoryEntry> nodes;       // id → entry
                std::map<int64_t, int> hop_of;              // id → 0/1/2
                std::vector<MemoryRelation> all_edges;
                std::vector<int64_t> frontier{seed_id};
                nodes[seed_id] = *seed;
                hop_of[seed_id] = 0;

                for (int d = 0; d < depth && nodes.size() < kMaxNodes; ++d) {
                    std::vector<int64_t> next_frontier;
                    for (int64_t nid : frontier) {
                        if (nodes.size() >= kMaxNodes) break;
                        auto outs = tenants.list_relations(reader_tenant_id,
                                                           nid, 0, std::string{},
                                                           50);
                        auto ins  = tenants.list_relations(reader_tenant_id,
                                                           0, nid, std::string{},
                                                           50);
                        auto add_edge_target = [&](int64_t target) {
                            if (nodes.size() >= kMaxNodes) return;
                            if (nodes.count(target)) return;
                            auto neighbour = tenants.get_entry(reader_tenant_id, target);
                            if (!neighbour) return;
                            nodes[target] = *neighbour;
                            hop_of[target] = d + 1;
                            next_frontier.push_back(target);
                        };
                        for (auto& r : outs) {
                            add_edge_target(r.target_id);
                            all_edges.push_back(r);
                        }
                        for (auto& r : ins) {
                            add_edge_target(r.source_id);
                            all_edges.push_back(r);
                        }
                    }
                    frontier = std::move(next_frontier);
                }

                // Render: by hop, with nodes and edges grouped per hop.
                // Each edge appears once even if both endpoints are in
                // the subgraph; we pick the lower-hop endpoint as the
                // anchor for display.
                std::ostringstream out;
                out << "Subgraph around #" << seed_id << " (depth=" << depth
                    << ", " << nodes.size() << " nodes):\n";
                out << "  hop 0: " << fmt_entry_line(*seed).substr(2) << "\n";

                for (int d = 1; d <= depth; ++d) {
                    bool first = true;
                    for (auto& [id, e] : nodes) {
                        if (hop_of[id] != d) continue;
                        if (first) {
                            out << "\n  hop " << d << ":\n";
                            first = false;
                        }
                        out << "    " << fmt_entry_line(e).substr(2) << "\n";
                    }
                }
                if (!all_edges.empty()) {
                    out << "\n  edges:\n";
                    // Dedupe by id (list_relations may return overlaps when
                    // a node is queried as both source and target on
                    // different hops).
                    std::set<int64_t> seen_edges;
                    for (auto& r : all_edges) {
                        if (!seen_edges.insert(r.id).second) continue;
                        if (!nodes.count(r.source_id) || !nodes.count(r.target_id)) continue;
                        out << "    #" << r.source_id << " --["
                            << r.relation << "]--> #" << r.target_id << "\n";
                    }
                }
                if (nodes.size() >= kMaxNodes) {
                    out << "\n  (subgraph capped at " << kMaxNodes
                        << " nodes — narrow with /mem entry <id> to dig further)\n";
                }
                return out.str();
            }

            if (kind == "density") {
                // /mem density <id>
                // Quick "is this part of the graph dense or sparse?"
                // probe — out-degree, in-degree, distinct relation kinds,
                // and 2-hop reach.  Cheap follow-up before doing redundant
                // research on a topic the graph already covers.
                int64_t id = mem_parse_id(args);
                if (id <= 0) return "ERR: usage: /mem density <id>";
                auto e = tenants.get_entry(reader_tenant_id, id);
                if (!e)
                    return "ERR: entry " + std::to_string(id) + " not found";

                auto outs = tenants.list_relations(reader_tenant_id, id, 0,
                                                    std::string{}, 200);
                auto ins  = tenants.list_relations(reader_tenant_id, 0, id,
                                                    std::string{}, 200);

                std::set<std::string> relation_kinds;
                std::set<int64_t> hop1_nodes;
                for (auto& r : outs) {
                    relation_kinds.insert(r.relation);
                    hop1_nodes.insert(r.target_id);
                }
                for (auto& r : ins) {
                    relation_kinds.insert(r.relation);
                    hop1_nodes.insert(r.source_id);
                }

                // 2-hop reach: count unique nodes (not equal to seed) that
                // any 1-hop neighbour edges touch.  Caps walk at 50
                // 1-hop nodes to keep the probe cheap.
                std::set<int64_t> hop2_nodes;
                int probed = 0;
                for (int64_t n : hop1_nodes) {
                    if (++probed > 50) break;
                    auto o = tenants.list_relations(reader_tenant_id, n, 0,
                                                     std::string{}, 50);
                    auto i = tenants.list_relations(reader_tenant_id, 0, n,
                                                     std::string{}, 50);
                    for (auto& r : o) if (r.target_id != id) hop2_nodes.insert(r.target_id);
                    for (auto& r : i) if (r.source_id != id) hop2_nodes.insert(r.source_id);
                }
                // Don't double-count 1-hop nodes in the 2-hop set.
                for (int64_t n : hop1_nodes) hop2_nodes.erase(n);

                std::ostringstream out;
                out << fmt_entry_line(*e) << "\n";
                out << "  out-edges:    " << outs.size() << "\n";
                out << "  in-edges:     " << ins.size()  << "\n";
                out << "  distinct relations: ";
                {
                    bool first = true;
                    for (auto& r : relation_kinds) {
                        if (!first) out << ", ";
                        out << r;
                        first = false;
                    }
                    if (relation_kinds.empty()) out << "(none)";
                }
                out << "\n";
                out << "  1-hop nodes:  " << hop1_nodes.size() << "\n";
                out << "  2-hop reach:  " << hop2_nodes.size()
                    << " new nodes (beyond direct neighbours)\n";
                if (outs.empty() && ins.empty()) {
                    out << "  → isolated node — no relations yet.  "
                           "Consider /mem add link to connect it.\n";
                } else if (hop1_nodes.size() + hop2_nodes.size() < 4) {
                    out << "  → sparse neighbourhood.  Likely worth research / linking.\n";
                } else {
                    out << "  → dense neighbourhood.  Existing graph "
                           "structure may already cover the topic.\n";
                }
                return out.str();
            }

            return "ERR: unknown structured-memory subcommand";
        };
}
StructuredMemoryWriter make_structured_memory_writer_callback(
    TenantStore& tenants, int64_t reader_tenant_id,
    int64_t reader_conversation_id, Orchestrator* orch_ptr) {
    return
        [&tenants, reader_tenant_id, reader_conversation_id, orch_ptr]
        (const std::string& kind,
         const std::string& args,
         const std::string& body,
         const std::string& caller_id) -> std::string {
            // Trim leading/trailing whitespace from a token.
            auto trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
                while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
                return s;
            };

            if (kind == "add-entry") {
                // /mem add entry <type> <title> [--artifact #<id>]
                // Trailing --artifact links a /write --persist'd file
                // straight into the new entry.
                std::istringstream iss(args);
                std::string type;
                iss >> type;
                std::string title;
                std::getline(iss, title);
                title = trim(title);
                if (type.empty() || title.empty()) {
                    return "ERR: usage: /mem add entry <type> <title> "
                           "[--artifact #<id>]";
                }

                // Manual consolidation: --supersedes #1,#2,#3 — strip
                // first because we want it before the --artifact strip
                // (so the artifact pos-search doesn't get confused by a
                // preceding --supersedes) and validate the ids exist
                // for the tenant before we commit anything.
                std::vector<int64_t> supersedes_ids;
                {
                    const std::string flag = "--supersedes";
                    auto pos = title.rfind(flag);
                    if (pos != std::string::npos &&
                        (pos == 0 || title[pos - 1] == ' ' || title[pos - 1] == '\t')) {
                        std::string tail = title.substr(pos + flag.size());
                        while (!tail.empty() && (tail.front() == ' ' || tail.front() == '\t'))
                            tail.erase(0, 1);
                        // Tail runs to the next " --" flag boundary or EOL.
                        std::string token;
                        size_t cut = std::string::npos;
                        size_t scan = 0;
                        while (scan + 2 < tail.size()) {
                            if (tail[scan] == ' ' && tail[scan + 1] == '-' && tail[scan + 2] == '-') {
                                cut = scan;
                                break;
                            }
                            ++scan;
                        }
                        if (cut == std::string::npos) {
                            token = tail;
                            tail.clear();
                        } else {
                            token = tail.substr(0, cut);
                            tail = tail.substr(cut + 1);   // keep " --next-flag …" for re-attach
                        }
                        // token = "#1,#2,#3" — split on comma.
                        size_t i = 0;
                        while (i < token.size()) {
                            size_t comma = token.find(',', i);
                            std::string piece = token.substr(i,
                                comma == std::string::npos ? std::string::npos : comma - i);
                            // trim
                            while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t'))
                                piece.erase(0, 1);
                            while (!piece.empty() && (piece.back()  == ' ' || piece.back()  == '\t'))
                                piece.pop_back();
                            if (!piece.empty() && piece.front() == '#') piece.erase(0, 1);
                            if (!piece.empty()) {
                                int64_t id = 0;
                                try { id = std::stoll(piece); } catch (...) { id = 0; }
                                if (id <= 0) {
                                    return "ERR: --supersedes accepts a "
                                           "comma-separated list of ids, "
                                           "e.g. --supersedes #41,#42";
                                }
                                supersedes_ids.push_back(id);
                            }
                            if (comma == std::string::npos) break;
                            i = comma + 1;
                        }
                        if (supersedes_ids.empty()) {
                            return "ERR: --supersedes accepts a "
                                   "comma-separated list of ids, "
                                   "e.g. --supersedes #41,#42";
                        }
                        // Reattach any flags that came after --supersedes
                        // back onto the title chunk for downstream parsing.
                        if (pos > 0 && (title[pos - 1] == ' ' || title[pos - 1] == '\t'))
                            --pos;
                        title.resize(pos);
                        if (!tail.empty()) title += " " + tail;
                        title = trim(title);
                    }
                }

                int64_t artifact_id = 0;
                {
                    // Strip trailing `--artifact #<id>` off the title
                    // before we length-check it.
                    const std::string flag = "--artifact";
                    auto pos = title.rfind(flag);
                    if (pos != std::string::npos &&
                        (pos == 0 || title[pos - 1] == ' ' || title[pos - 1] == '\t')) {
                        std::string tail = title.substr(pos + flag.size());
                        while (!tail.empty() && (tail.front() == ' ' || tail.front() == '\t'))
                            tail.erase(0, 1);
                        if (!tail.empty() && tail.front() == '#') tail.erase(0, 1);
                        try { artifact_id = std::stoll(tail); }
                        catch (...) { artifact_id = 0; }
                        if (artifact_id <= 0) {
                            return "ERR: --artifact requires a positive id, "
                                   "e.g. --artifact #42";
                        }
                        if (pos > 0 && (title[pos - 1] == ' ' || title[pos - 1] == '\t'))
                            --pos;
                        title.resize(pos);
                        title = trim(title);
                    }
                }

                // Validate every manual supersedes id belongs to this
                // tenant BEFORE we create the synthesis entry.  Catching
                // a bad id here means we don't leave a dangling entry
                // partway through commit — fail closed instead.
                for (auto sid : supersedes_ids) {
                    if (!tenants.get_entry(reader_tenant_id, sid)) {
                        return "ERR: --supersedes #" + std::to_string(sid) +
                               " does not exist for this tenant (or is "
                               "already invalidated; pass active ids only)";
                    }
                }

                if (!memory_entry_type_is_valid(type)) {
                    return "ERR: invalid type '" + type + "' — must be one of: "
                           "user, feedback, project, reference, learning, context";
                }
                if (title.empty()) {
                    return "ERR: title is required (got only the --artifact flag)";
                }
                if (title.size() > 200) {
                    return "ERR: title length must be 1..200 chars (got " +
                           std::to_string(title.size()) + ")";
                }
                if (artifact_id > 0 &&
                    !tenants.get_artifact_meta(reader_tenant_id, artifact_id)) {
                    return "ERR: artifact #" + std::to_string(artifact_id) +
                           " does not exist for this tenant";
                }

                // Defense in depth: the dispatcher already rejects empty
                // bodies, but if one slips through (older caller path or
                // future regression), refuse the write here too — a
                // title-only entry has no value to /mem search.
                std::string content = trim(body);
                if (content.empty()) {
                    return "ERR: /mem add entry requires a content body "
                           "(synthesised retrievable text between the "
                           "header line and /endmem)";
                }
                if (content.size() > 32 * 1024) {
                    return "ERR: content body too large (limit 32KB; got " +
                           std::to_string(content.size()) + " bytes).  "
                           "Trim to the load-bearing facts; the artifact "
                           "store holds long-form output via /write --persist";
                }

                // Read caller's memory config for auto-enrichment
                // toggles.  Defensive try: an unknown caller id keeps
                // pre-config behavior (no enrichment) without erroring
                // the write itself.
                Constitution::MemoryConfig mc_add;
                std::string caller_advisor_model;
                try {
                    const auto& cc = orch_ptr->get_constitution(caller_id);
                    mc_add = cc.memory;
                    caller_advisor_model = cc.advisor.model;
                    if (caller_advisor_model.empty())
                        caller_advisor_model = cc.advisor_model;
                } catch (...) {}

                // Auto-tagging.  When enabled (and the agent has an
                // advisor model), an advisor extracts 2-4 short
                // topical tags from title+content.  Tags get the 8x
                // BM25 weight on retrieval — one of the strongest
                // ranking signals available — and most agent ingest
                // paths leave them empty.  A failed advisor call
                // surfaces a note in the OK output and the entry is
                // written with empty tags as before.
                std::string tags_json = "[]";
                std::vector<std::string> auto_tags_added;
                std::string auto_tag_note;
                if (mc_add.auto_tag && !caller_advisor_model.empty()) {
                    auto tag_advisor = orch_ptr->make_advisor_invoker(caller_id);
                    auto extr = extract_tags_with_advisor(tag_advisor,
                                                            title, content,
                                                            /*existing=*/{});
                    auto_tags_added = std::move(extr.tags);
                    auto_tag_note   = std::move(extr.note);
                    if (!auto_tags_added.empty()) {
                        // Hand-build the JSON array — we know the
                        // tags are already validated (lowercase,
                        // hyphen, ≤32 chars) by the helper.  The
                        // canonical_tags_json validator would also
                        // accept this but the round trip is
                        // unnecessary for known-good input.
                        std::ostringstream tj;
                        tj << "[";
                        for (size_t i = 0; i < auto_tags_added.size(); ++i) {
                            if (i) tj << ",";
                            tj << "\"";
                            for (char c : auto_tags_added[i]) {
                                if (c == '"' || c == '\\') tj << '\\';
                                tj << c;
                            }
                            tj << "\"";
                        }
                        tj << "]";
                        tags_json = tj.str();
                    }
                }

                // Pin the entry to the active conversation when one is
                // present.  Without that link, /mem add entry inside a
                // conversation produces tenant-wide entries that don't
                // bias the conversation-scoped /mem search ranking.
                auto e = tenants.create_entry(reader_tenant_id, type, title,
                                               content, /*source=*/"agent",
                                               tags_json,
                                               artifact_id,
                                               reader_conversation_id);

                // Manual consolidation: every --supersedes id gets a
                // `supersedes` relation FROM the new entry TO the old
                // one, and the old entry is invalidated.  A relation
                // collision (the link already exists from a prior
                // attempt) is a no-op — caller's intent still satisfied.
                std::vector<int64_t> superseded_ids;
                std::string supersede_note;
                bool manual_supersede = !supersedes_ids.empty();
                if (manual_supersede) {
                    for (auto sid : supersedes_ids) {
                        (void)tenants.create_relation(reader_tenant_id,
                            e.id, sid, "supersedes");
                        if (tenants.invalidate_entry(reader_tenant_id, sid)) {
                            superseded_ids.push_back(sid);
                        }
                    }
                }

                // Auto-supersession.  After the new entry is written,
                // if the agent opted in (and didn't already supply a
                // manual --supersedes list), ask the advisor whether
                // any existing entries on the same title+type are now
                // factually contradicted; invalidate those.  Bias is
                // toward "leave alone" — false positives erase
                // legitimate prior memory.  Manual --supersedes wins:
                // an explicit list signals the agent already knows
                // which prior facts are stale.
                if (!manual_supersede &&
                    mc_add.auto_supersede && !caller_advisor_model.empty()) {
                    TenantStore::EntryFilter fcand;
                    fcand.q     = title;
                    fcand.types = { type };
                    fcand.limit = 6;
                    auto cands = tenants.list_entries(reader_tenant_id, fcand);
                    cands.erase(std::remove_if(cands.begin(), cands.end(),
                        [&e](const MemoryEntry& x) { return x.id == e.id; }),
                        cands.end());
                    if (cands.size() > 5) cands.resize(5);

                    if (!cands.empty()) {
                        auto sup_advisor = orch_ptr->make_advisor_invoker(caller_id);
                        auto sup = detect_supersession_with_advisor(
                            sup_advisor, title, content, cands);
                        superseded_ids = std::move(sup.invalidated_ids);
                        supersede_note = std::move(sup.note);
                        for (auto sid : superseded_ids) {
                            (void)tenants.invalidate_entry(reader_tenant_id, sid);
                        }
                    }
                }

                std::ostringstream out;
                out << "OK: added entry #" << e.id << " [" << e.type << "] "
                    << e.title;
                if (artifact_id > 0) {
                    out << " (linked to artifact #" << artifact_id << ")";
                }
                if (!auto_tags_added.empty()) {
                    out << "\n  auto-tagged: ";
                    for (size_t i = 0; i < auto_tags_added.size(); ++i) {
                        if (i) out << ", ";
                        out << auto_tags_added[i];
                    }
                }
                if (!auto_tag_note.empty()) out << "\n  " << auto_tag_note;
                if (!superseded_ids.empty()) {
                    out << "\n  superseded";
                    out << (manual_supersede ? " (manual): " : ": ");
                    for (size_t i = 0; i < superseded_ids.size(); ++i) {
                        if (i) out << ", ";
                        out << "#" << superseded_ids[i];
                    }
                }
                if (!supersede_note.empty()) out << "\n  " << supersede_note;
                out << ".  Use this id in subsequent /mem add link calls to "
                       "reference it.\n";
                return out.str();
            }

            if (kind == "invalidate") {
                // /mem invalidate <id>
                // Args is just the id token.  Anything past the first
                // whitespace is ignored — keeps the grammar tight.
                // Tolerate a leading '#' so the displayed and accepted
                // id forms agree (entries-list output uses #<n>).
                std::istringstream iss(args);
                std::string id_tok;
                iss >> id_tok;
                int64_t id = mem_parse_id(id_tok);
                if (id <= 0) {
                    return "ERR: usage: /mem invalidate <id>";
                }
                if (!tenants.invalidate_entry(reader_tenant_id, id)) {
                    // Same false-collapse the HTTP handler navigates: the
                    // row is missing, cross-tenant, or already invalid.
                    // From the agent's perspective the after-state is the
                    // same ("the row is no longer active"), so the wording
                    // here merges those cases.
                    std::ostringstream out;
                    out << "ERR: entry #" << id
                        << " not found or already invalidated";
                    return out.str();
                }
                std::ostringstream out;
                out << "OK: invalidated entry #" << id
                    << " (still reachable through historical reads, "
                    << "hidden from default queries).\n";
                return out.str();
            }

            if (kind == "add-link") {
                // /mem add link <src_id> <relation> <dst_id>
                // Both ids tolerate a leading '#' so an agent can copy
                // the displayed `#<n>` form straight from /mem entries
                // or pipeline-memory output without manual stripping.
                std::istringstream iss(args);
                std::string src_tok, dst_tok;
                std::string relation;
                iss >> src_tok >> relation >> dst_tok;
                int64_t src = mem_parse_id(src_tok);
                int64_t dst = mem_parse_id(dst_tok);
                if (src <= 0 || dst <= 0 || relation.empty()) {
                    return "ERR: usage: /mem add link <src_id> <relation> <dst_id>";
                }
                if (src == dst) {
                    return "ERR: self-loops not allowed";
                }
                if (!memory_relation_is_valid(relation)) {
                    return "ERR: invalid relation '" + relation + "' — must be "
                           "one of: relates_to, refines, contradicts, "
                           "supersedes, supports";
                }
                auto src_entry = tenants.get_entry(reader_tenant_id, src);
                auto dst_entry = tenants.get_entry(reader_tenant_id, dst);
                if (!src_entry || !dst_entry) {
                    return "ERR: one or both endpoint ids do not exist for "
                           "this tenant";
                }
                auto created = tenants.create_relation(reader_tenant_id,
                                                        src, dst, relation);
                if (!created) {
                    auto existing = tenants.find_relation(reader_tenant_id,
                                                           src, dst, relation);
                    std::ostringstream out;
                    out << "ERR: a " << relation << " relation from #" << src
                        << " to #" << dst << " already exists";
                    if (existing) out << " (id=" << existing->id << ")";
                    out << "\n";
                    return out.str();
                }
                std::ostringstream out;
                out << "OK: added relation #" << created->id << ": #" << src
                    << " --[" << relation << "]--> #" << dst << ".\n";
                return out.str();
            }

            return "ERR: unknown structured-memory write subcommand";
        };
}

// Format an a2a::Manager's configured remotes as a roster line block
// suitable for splicing into the master orchestrator's turn preamble.
// Empty string when no remotes are configured or no cards resolve.
std::string format_a2a_remote_roster(a2a::Manager& mgr) {
    auto names = mgr.agent_names();
    if (names.empty()) return "";
    std::ostringstream ss;
    ss << "REMOTE A2A AGENTS — delegate with /a2a call <name> <message> "
       << "(distinct trust boundary; no shared memory):\n";
    bool any_resolved = false;
    for (auto& name : names) {
        ss << "  " << name;
        try {
            auto& card = mgr.client(name).card();
            if (!card.description.empty()) ss << " — " << card.description;
            // Tag with a few skill ids so the master can match by
            // capability cheaply.  Cap to keep the block short.
            if (!card.skills.empty()) {
                ss << " (skills:";
                int shown = 0;
                for (auto& s : card.skills) {
                    if (shown++ >= 5) { ss << ", …"; break; }
                    ss << (shown == 1 ? " " : ", ") << s.id;
                }
                ss << ")";
            }
            any_resolved = true;
        } catch (const std::exception&) {
            ss << "  (card unavailable)";
        }
        ss << "\n";
    }
    return any_resolved ? ss.str() : "";
}

// Build the A2AInvoker callback for a request.  The lambda owns the
// per-request a2a::Manager via shared_ptr so its lifetime is tied to
// the orchestrator (which owns the lambda in turn).  Returns nullptr
// when no registry is configured — the dispatcher then surfaces a
// clean ERR explaining /a2a is unavailable.
//
// `roster_out` (when non-null) receives a function that formats the
// same Manager's remote-agent roster.  Lets the caller wire both the
// invoker and the system-prompt injection from one Manager instance,
// which keeps card caches warm across both surfaces.
A2AInvoker make_a2a_invoker(const ApiServerOptions& opts,
                             Orchestrator::RemoteRosterProvider* roster_out = nullptr) {
    if (opts.a2a_agents_path.empty()) return nullptr;
    std::vector<a2a::RemoteAgentConfig> configs;
    try {
        configs = a2a::load_registry(opts.a2a_agents_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[a2a] registry load failed at %s: %s\n",
            opts.a2a_agents_path.c_str(), e.what());
        return nullptr;
    }
    if (configs.empty()) return nullptr;

    auto mgr = std::make_shared<a2a::Manager>(std::move(configs));

    if (roster_out) {
        *roster_out = [mgr]() -> std::string {
            return format_a2a_remote_roster(*mgr);
        };
    }

    return [mgr](const std::string& kind, const std::string& args) -> std::string {
        if (kind == "list") {
            auto names = mgr->agent_names();
            if (names.empty()) return "no remote agents configured";
            // Best-effort card fetch to surface descriptions; failures
            // appear as `(card unavailable)` so the user knows the
            // agent is registered but unreachable rather than missing.
            std::ostringstream os;
            os << "configured remote agents (" << names.size() << "):\n";
            for (auto& name : names) {
                os << "  " << name;
                try {
                    auto& card = mgr->client(name).card();
                    if (!card.description.empty()) {
                        os << " — " << card.description;
                    }
                    if (!card.skills.empty()) {
                        os << "\n    skills:";
                        for (auto& s : card.skills) {
                            os << " " << s.id;
                        }
                    }
                } catch (const std::exception& e) {
                    os << "  (card unavailable: " << e.what() << ")";
                }
                os << "\n";
            }
            return os.str();
        }
        if (kind == "card") {
            const std::string name = args;
            if (name.empty()) return "ERR: usage: /a2a card <name>";
            if (!mgr->has(name)) {
                return "ERR: no remote agent named '" + name + "' (try /a2a list)";
            }
            try {
                auto& card = mgr->client(name).card();
                std::ostringstream os;
                os << "remote agent '" << card.name << "' (" << card.url << ")\n";
                os << "  protocol: A2A " << card.protocol_version
                   << ", version: " << card.version << "\n";
                os << "  description: " << card.description << "\n";
                if (!card.skills.empty()) {
                    os << "  skills:\n";
                    for (auto& s : card.skills) {
                        os << "    - " << s.id;
                        if (!s.name.empty() && s.name != s.id) os << " (" << s.name << ")";
                        os << ": " << s.description << "\n";
                    }
                }
                return os.str();
            } catch (const std::exception& e) {
                return std::string("ERR: card fetch failed: ") + e.what();
            }
        }
        if (kind == "call") {
            // Parse: <name> <message...>
            auto sp = args.find(' ');
            if (sp == std::string::npos) {
                return "ERR: usage: /a2a call <name> <message>";
            }
            const std::string name    = args.substr(0, sp);
            const std::string message = args.substr(sp + 1);
            if (name.empty() || message.empty()) {
                return "ERR: usage: /a2a call <name> <message>";
            }
            if (!mgr->has(name)) {
                return "ERR: no remote agent named '" + name + "' (try /a2a list)";
            }
            a2a::Message msg;
            msg.role       = "user";
            msg.message_id = new_request_id();
            a2a::Part p;
            p.kind = "text";
            p.text = message;
            msg.parts.push_back(std::move(p));
            try {
                std::string err;
                auto task = mgr->client(name).send_message(msg, err);
                if (!task) {
                    return "ERR: " + err;
                }
                if (task->status.message) {
                    // Concatenate text parts of the assistant's reply.
                    std::ostringstream os;
                    for (auto& part : task->status.message->parts) {
                        if (part.kind == "text") os << part.text;
                    }
                    std::string body = os.str();
                    if (body.empty()) {
                        // Spec-legal but unhelpful — surface the state
                        // so callers can debug.
                        return "(remote agent returned no text content; state="
                               + a2a::task_state_to_string(task->status.state) + ")";
                    }
                    return body;
                }
                return "(remote agent returned no message; state="
                       + a2a::task_state_to_string(task->status.state) + ")";
            } catch (const std::exception& e) {
                return std::string("ERR: ") + e.what();
            }
        }
        return "ERR: unknown /a2a subcommand '" + kind + "'";
    };
}

// Construct an orchestrator for a one-shot A2A turn.  Loads the tenant's
// stored agent catalog (so /agent + /parallel resolve sibling ids during
// the turn).  Memory bridge, MCP manager, search invoker, file
// interceptor, and structured-memory reader are deliberately NOT wired
// here — those depend on the SSE event sink which lands in PR-3.  The
// agent can chat and delegate; tool slash commands degrade to ERR for
// the v1.0 message/send synchronous path, and PR-3 lifts both surfaces
// to full parity.
std::unique_ptr<Orchestrator>
build_a2a_orchestrator(const ApiServerOptions& opts,
                        TenantStore& tenants, const Tenant& tenant,
                        std::string& err_out) {
    std::unique_ptr<Orchestrator> orch;
    try {
        orch = std::make_unique<Orchestrator>(opts.api_keys);
    } catch (const std::exception& e) {
        err_out = std::string("orchestrator init failed: ") + e.what();
        return nullptr;
    }
    if (!opts.memory_root.empty()) {
        orch->set_memory_dir(opts.memory_root + "/t" +
                              std::to_string(tenant.id));
    }
    orch->set_exec_disabled(opts.exec_disabled);
    if (auto exec_inv = make_exec_invoker_callback(opts, tenant.id)) {
        orch->set_exec_invoker(std::move(exec_inv));
    }
    orch->client().set_circuit_breaker(opts.circuit_breaker);
    orch->client().set_metrics(opts.metrics);

    const auto records = tenants.list_agent_records(tenant.id, /*limit=*/200);
    for (const auto& rec : records) {
        try {
            auto cfg = Constitution::from_json(rec.agent_def_json);
            if (orch->has_agent(rec.agent_id)) orch->remove_agent(rec.agent_id);
            orch->create_agent(rec.agent_id, std::move(cfg));
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[a2a] skipping malformed stored agent '%s' for tenant %lld: %s\n",
                rec.agent_id.c_str(), (long long)tenant.id, e.what());
        }
    }

    // Tool-callback parity with /v1/orchestrate.  These factories are
    // shared with handle_orchestrate so /mem read|write|search|entries,
    // /mcp tools|call, /search, and /a2a all work identically inside an
    // A2A turn.  Artifact callbacks (/write --persist, /read, /list) are
    // intentionally NOT wired here because A2A's contextId is opaque
    // and not foreign-keyed against a conversation; agents using /write
    // see captured-but-not-persisted bytes flowing as A2A artifacts in
    // the streaming path.  Logging during MCP registry load goes to
    // stderr because there's no SSE error sink at orchestrator-build
    // time.
    orch->set_memory_scratchpad(
        make_memory_scratchpad_callback(tenant.id, &tenants));
    orch->set_structured_memory_reader(
        make_structured_memory_reader_callback(
            tenants, tenant.id, /*conversation_id=*/0, orch.get()));
    orch->set_structured_memory_writer(
        make_structured_memory_writer_callback(
            tenants, tenant.id, /*conversation_id=*/0, orch.get()));
    orch->set_mcp_invoker(make_mcp_invoker_callback(make_mcp_manager(opts,
        [](const std::string& m) {
            std::fprintf(stderr, "[a2a] %s\n", m.c_str());
        })));
    if (auto search_inv = make_search_invoker_callback(opts)) {
        orch->set_search_invoker(std::move(search_inv));
    }

    // Wire the /a2a slash command so a server-side master agent can
    // delegate to remote A2A agents — symmetric to the /v1/orchestrate
    // wiring.  Auto-routing into the master's catalog is on by default
    // (the user's locked-in PR-8 decision).
    Orchestrator::RemoteRosterProvider roster_cb;
    if (auto inv = make_a2a_invoker(opts, &roster_cb)) {
        orch->set_a2a_invoker(std::move(inv));
        if (roster_cb) orch->set_remote_roster_provider(std::move(roster_cb));
    }
    orch->set_scheduler_invoker(
        make_scheduler_invoker_callback(tenants, tenant.id, /*conversation_id=*/0));
    orch->set_todo_invoker(
        make_todo_invoker_callback(tenants, tenant.id, /*conversation_id=*/0));
    orch->set_lesson_invoker(
        make_lesson_invoker_callback(tenants, tenant.id));
    return orch;
}

void handle_a2a_message_send(int fd,
                              const std::shared_ptr<JsonValue>& rpc_id,
                              const std::string& agent_id,
                              const a2a::RpcRequest& rpc,
                              const ApiServerOptions& opts,
                              TenantStore& tenants,
                              InFlightRegistry& in_flight,
                              const Tenant& tenant) {
    a2a::Message user_msg;
    if (!rpc.params) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params required"));
        return;
    }
    try {
        user_msg = a2a::extract_send_message(*rpc.params);
    } catch (const std::exception& e) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, e.what()));
        return;
    }

    std::string prompt;
    try {
        prompt = a2a::concatenate_text_parts(user_msg);
    } catch (const std::exception& e) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_CONTENT_TYPE_INVALID, e.what()));
        return;
    }
    if (prompt.empty()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS,
            "message has no text parts to send"));
        return;
    }

    const std::string task_id    = new_request_id();
    const std::string context_id = resolve_a2a_context_id(user_msg);

    std::string init_err;
    auto orch = build_a2a_orchestrator(opts, tenants, tenant, init_err);
    if (!orch) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INTERNAL_ERROR, init_err));
        return;
    }
    if (agent_id != "index" && !orch->has_agent(agent_id)) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_TASK_NOT_FOUND,
            "no agent '" + agent_id + "' for this tenant; POST it to /v1/agents first"));
        return;
    }

    // Persistence: rows go in at submitted, transition to working, then
    // to a terminal state.  A failure to insert is non-fatal — we log
    // and continue (the call still completes; tasks/get just won't
    // surface a record).
    try {
        tenants.create_a2a_task(tenant.id, task_id, agent_id, context_id,
                                 a2a::task_state_to_string(a2a::TaskState::submitted));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[a2a] create_a2a_task failed: %s\n", e.what());
    }

    InFlightScope in_flight_scope(in_flight, task_id, orch.get(), tenant.id);

    tenants.update_a2a_task(tenant.id, task_id,
                             a2a::task_state_to_string(a2a::TaskState::working),
                             /*final_message_json=*/"",
                             /*error_message=*/"");

    ApiResponse resp;
    try {
        resp = orch->send(agent_id, prompt);
    } catch (const std::exception& e) {
        tenants.update_a2a_task(tenant.id, task_id,
                                 a2a::task_state_to_string(a2a::TaskState::failed),
                                 "", e.what());
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_INVALID_AGENT_RESPONSE,
            std::string("agent threw: ") + e.what()));
        return;
    }

    a2a::Task task = a2a::build_terminal_task(task_id, context_id, agent_id,
                                               user_msg, resp);

    // Persist the terminal state.  We snapshot only the assistant
    // Message — history/artifacts can be reconstructed by combining
    // the user's input (which the client already has) with the
    // assistant's reply.  Smaller column, simpler reads.
    std::string final_msg_json;
    if (task.status.message) {
        final_msg_json = json_serialize(*a2a::to_json(*task.status.message));
    }
    tenants.update_a2a_task(tenant.id, task_id,
                             a2a::task_state_to_string(task.status.state),
                             final_msg_json,
                             resp.ok ? "" : resp.error);

    write_a2a_rpc(fd, a2a::make_result_response(rpc_id, a2a::to_json(task)));
}

// PR-3: streaming variant of message/send.  Opens an SSE response and
// emits one JSON-RPC chunk per arbiter event, all wrapped in TaskStatus
// or TaskArtifact updates.  Closes with a final TaskStatusUpdateEvent
// (final=true).  Tool-side callbacks (memory, MCP, search, structured
// memory) are NOT yet wired here — they degrade to the same ERR
// behavior as PR-2's synchronous send_message; PR-4 lifts both paths
// to /v1/orchestrate parity.
void handle_a2a_message_stream(int fd,
                                const std::shared_ptr<JsonValue>& rpc_id,
                                const std::string& agent_id,
                                const a2a::RpcRequest& rpc,
                                const ApiServerOptions& opts,
                                TenantStore& tenants,
                                InFlightRegistry& in_flight,
                                const Tenant& tenant) {
    // Same up-front parsing as message/send; we send any error as a
    // single non-streaming JSON-RPC error response (HTTP 200) BEFORE
    // opening the SSE stream.  Once the stream is open, errors
    // surface as final-status TaskStatusUpdateEvents.
    a2a::Message user_msg;
    if (!rpc.params) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params required"));
        return;
    }
    try {
        user_msg = a2a::extract_send_message(*rpc.params);
    } catch (const std::exception& e) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, e.what()));
        return;
    }
    std::string prompt;
    try {
        prompt = a2a::concatenate_text_parts(user_msg);
    } catch (const std::exception& e) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_CONTENT_TYPE_INVALID, e.what()));
        return;
    }
    if (prompt.empty()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS,
            "message has no text parts to send"));
        return;
    }

    const std::string task_id    = new_request_id();
    const std::string context_id = resolve_a2a_context_id(user_msg);

    std::string init_err;
    auto orch = build_a2a_orchestrator(opts, tenants, tenant, init_err);
    if (!orch) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INTERNAL_ERROR, init_err));
        return;
    }
    if (agent_id != "index" && !orch->has_agent(agent_id)) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_TASK_NOT_FOUND,
            "no agent '" + agent_id + "' for this tenant; POST it to /v1/agents first"));
        return;
    }

    // Persist before opening the stream so a tasks/get racing the start
    // sees at least the submitted row.
    try {
        tenants.create_a2a_task(tenant.id, task_id, agent_id, context_id,
                                 a2a::task_state_to_string(a2a::TaskState::submitted));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[a2a] create_a2a_task failed: %s\n", e.what());
    }

    // Begin the SSE response.  After this point every error must round-
    // trip as an in-stream TaskStatusUpdateEvent — we can't switch back
    // to a JSON body because the client already saw the SSE headers.
    SseStream sse(fd);
    sse.write_headers();

    auto sink = [&sse](const std::string& ev,
                        std::shared_ptr<JsonValue> payload) {
        sse.emit(ev, std::move(payload));
    };
    a2a::A2aStreamWriter writer(sink, rpc_id, task_id, context_id, agent_id);

    // Track depth-by-stream-id so callbacks that don't carry depth
    // (agent_stream, stream_end) can decide whether to emit text into
    // the master artifact or leave it for the sub-agent summary path.
    std::map<int, int> depth_by_stream;
    std::map<int, std::string> agent_by_stream;
    std::mutex stream_meta_mu;

    std::atomic<bool> working_persisted{false};
    orch->set_stream_start_callback(
        [&](const std::string& a, int stream_id, int depth) {
            {
                std::lock_guard<std::mutex> lk(stream_meta_mu);
                depth_by_stream[stream_id] = depth;
                agent_by_stream[stream_id] = a;
            }
            if (depth == 0) {
                writer.emit_status(a2a::TaskState::working, /*final=*/false);
                if (!working_persisted.exchange(true)) {
                    tenants.update_a2a_task(
                        tenant.id, task_id,
                        a2a::task_state_to_string(a2a::TaskState::working),
                        "", "");
                }
            }
        });

    orch->set_agent_stream_callback(
        [&](const std::string& /*a*/, int stream_id, const std::string& delta) {
            int depth = 0;
            {
                std::lock_guard<std::mutex> lk(stream_meta_mu);
                auto it = depth_by_stream.find(stream_id);
                if (it != depth_by_stream.end()) depth = it->second;
            }
            // Only the master agent's stream feeds the primary text
            // artifact.  Sub-agent text is summarised once at end-of-
            // turn via the progress_callback path below — streaming
            // it interleaved would confuse clients trying to render a
            // single coherent response.
            if (depth == 0) {
                writer.emit_text_chunk(delta, /*last_chunk=*/false);
            }
        });

    orch->set_stream_end_callback(
        [&](const std::string& /*a*/, int stream_id, bool /*ok*/) {
            std::lock_guard<std::mutex> lk(stream_meta_mu);
            depth_by_stream.erase(stream_id);
            agent_by_stream.erase(stream_id);
            // Note: the *final* TaskStatusUpdateEvent fires after the
            // entire send_streaming returns (below) — not here.  Per-
            // turn ends at depth>0 are part of normal sub-agent flow
            // and don't terminate the A2A stream.
        });

    // Per-tool observation events.  arbiter fires this for /fetch,
    // /search, /mem, /mcp, /agent, /parallel, /write, etc.
    orch->set_tool_status_callback(
        [&](const std::string& tool, bool ok) {
            writer.emit_tool_call(tool, ok);
        });

    // Sub-agent completion: emit the full text the sub-agent produced
    // as a side artifact.  current_stream_depth() reads the depth at
    // the time the callback fires, which is the sub-agent's turn
    // depth (>0).
    Orchestrator* orch_ptr = orch.get();
    orch->set_progress_callback(
        [&writer, orch_ptr](const std::string& a, const std::string& content) {
            writer.emit_sub_agent(a, orch_ptr->current_stream_depth(), content);
        });

    // /write capture so the agent can emit files mid-turn and the
    // remote client receives them as artifact-update events rather
    // than the server filesystem absorbing them.  Mirrors handle_orchestrate's
    // bytes-cap semantics so a runaway agent can't blow up the response.
    std::atomic<size_t> bytes_captured{0};
    const size_t cap = opts.file_max_bytes;
    orch->set_write_interceptor(
        [&writer, &bytes_captured, cap](const std::string& path,
                                         const std::string& content) -> std::string {
            const size_t size = content.size();
            const size_t prev = bytes_captured.load();
            if (prev + size > cap) {
                return "ERR: per-response file-size cap (" +
                       std::to_string(cap) + " bytes) reached — this file "
                       "was NOT included in the response.";
            }
            bytes_captured.fetch_add(size);
            writer.emit_file(path, content, "text/plain");
            return "OK: captured " + std::to_string(size) +
                   " bytes for '" + path + "' (streamed to client)";
        });

    // Cancellation hook so /v1/requests/:task_id/cancel can interrupt
    // an in-flight stream.  Same RAII shape as /v1/orchestrate.
    InFlightScope in_flight_scope(in_flight, task_id, orch_ptr, tenant.id);

    // Drive the agentic loop.  send_streaming returns the final
    // ApiResponse once the dispatch loop terminates (success or fail).
    // The StreamCallback we pass is intentionally a no-op — text deltas
    // already flow through agent_stream_callback after StreamFilter
    // strips slash commands, so feeding the raw chunks here would
    // double-emit and surface unfiltered command text.
    ApiResponse resp;
    try {
        resp = orch->send_streaming(agent_id, prompt,
                                     [](const std::string&) {});
    } catch (const std::exception& e) {
        // Catastrophic failure during the loop.  Surface as a final
        // failed-status update so the client knows the stream is over.
        a2a::Message err_msg;
        err_msg.role        = "agent";
        err_msg.message_id  = task_id + "-err";
        err_msg.task_id     = task_id;
        err_msg.context_id  = context_id;
        a2a::Part p_err;
        p_err.kind = "text";
        p_err.text = std::string("agent threw: ") + e.what();
        err_msg.parts.push_back(std::move(p_err));
        writer.emit_status(a2a::TaskState::failed, /*final=*/true,
                           std::move(err_msg));
        tenants.update_a2a_task(tenant.id, task_id,
                                 a2a::task_state_to_string(a2a::TaskState::failed),
                                 "", e.what());
        sse.close();
        return;
    }

    // Compose the assistant's terminal Message and ride it on the
    // final TaskStatusUpdateEvent so clients have the full text in
    // one place even if they missed earlier text deltas.
    a2a::Message reply;
    reply.role        = "agent";
    reply.message_id  = task_id + "-r";
    reply.task_id     = task_id;
    reply.context_id  = context_id;
    a2a::Part p;
    p.kind = "text";
    p.text = resp.content;
    reply.parts.push_back(std::move(p));

    // Signal end-of-stream on the running text artifact so a client
    // accumulating chunks can finalise its rendering.
    writer.emit_text_chunk("", /*last_chunk=*/true);

    const a2a::TaskState terminal = resp.ok ? a2a::TaskState::completed
                                             : a2a::TaskState::failed;
    writer.emit_status(terminal, /*final=*/true, reply);

    std::string final_msg_json = json_serialize(*a2a::to_json(reply));
    tenants.update_a2a_task(tenant.id, task_id,
                             a2a::task_state_to_string(terminal),
                             final_msg_json,
                             resp.ok ? "" : resp.error);
    sse.close();
}

// Build a Task object from a persisted record.  The history isn't
// reconstructed (we don't snapshot the user's input on disk), so the
// returned Task has only the assistant's reply on status.message when
// the task reached a terminal state.  Clients combining tasks/get with
// their own request log can stitch the full history back together.
a2a::Task task_from_record(const TenantStore::A2aTaskRecord& rec) {
    a2a::Task t;
    t.id         = rec.task_id;
    t.context_id = rec.context_id;
    t.status.state = a2a::task_state_from_string(rec.state);
    if (!rec.final_message_json.empty()) {
        try {
            auto v = json_parse(rec.final_message_json);
            if (v && v->is_object()) {
                t.status.message = a2a::message_from_json(*v);
                t.history.push_back(*t.status.message);
            }
        } catch (...) { /* malformed snapshot — surface bare state */ }
    }
    auto md = jobj();
    auto& mm = md->as_object_mut();
    mm["x-arbiter.agent_id"]   = jstr(rec.agent_id);
    mm["x-arbiter.created_at"] = jnum(static_cast<double>(rec.created_at));
    mm["x-arbiter.updated_at"] = jnum(static_cast<double>(rec.updated_at));
    if (!rec.error_message.empty()) {
        mm["x-arbiter.error"] = jstr(rec.error_message);
    }
    t.metadata = md;
    return t;
}

void handle_a2a_tasks_get(int fd,
                           const std::shared_ptr<JsonValue>& rpc_id,
                           const a2a::RpcRequest& rpc,
                           TenantStore& tenants,
                           const Tenant& tenant) {
    // Params shape: { "id": "<task_id>" }.  Spec also allows
    // historyLength to bound the returned history; we don't currently
    // store full history so the param is accepted-and-ignored.
    if (!rpc.params || !rpc.params->is_object()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params must be an object"));
        return;
    }
    const std::string task_id = rpc.params->get_string("id", "");
    if (task_id.empty()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params.id required"));
        return;
    }
    auto rec = tenants.get_a2a_task(tenant.id, task_id);
    if (!rec) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_TASK_NOT_FOUND,
            "no task with id '" + task_id + "' for this tenant"));
        return;
    }
    write_a2a_rpc(fd, a2a::make_result_response(
        rpc_id, a2a::to_json(task_from_record(*rec))));
}

void handle_a2a_tasks_cancel(int fd,
                              const std::shared_ptr<JsonValue>& rpc_id,
                              const a2a::RpcRequest& rpc,
                              InFlightRegistry& in_flight,
                              TenantStore& tenants,
                              const Tenant& tenant) {
    if (!rpc.params || !rpc.params->is_object()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params must be an object"));
        return;
    }
    const std::string task_id = rpc.params->get_string("id", "");
    if (task_id.empty()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params.id required"));
        return;
    }

    // Same lock-while-cancel discipline as handle_cancel above so a
    // racing ~InFlightScope can't free the orchestrator under us.
    bool cancelled_in_flight = false;
    {
        std::lock_guard<std::mutex> lk(in_flight.mu);
        auto it = in_flight.by_id.find(task_id);
        if (it != in_flight.by_id.end() && it->second.tenant_id == tenant.id) {
            it->second.orch->cancel();
            cancelled_in_flight = true;
        }
    }

    if (cancelled_in_flight) {
        // Persist canceled state so a follow-up tasks/get reflects the
        // outcome.  The orchestrator's send may still be unwinding —
        // its terminal-state update will run, but we want canceled to
        // win over completed/failed for clarity.  Re-update.
        tenants.update_a2a_task(tenant.id, task_id,
                                 a2a::task_state_to_string(a2a::TaskState::canceled),
                                 "", "canceled by tasks/cancel");
        auto rec = tenants.get_a2a_task(tenant.id, task_id);
        if (!rec) {
            // Edge: task was never persisted (very fast race).  Synth a
            // minimal Task for the response.
            a2a::Task t;
            t.id           = task_id;
            t.status.state = a2a::TaskState::canceled;
            write_a2a_rpc(fd, a2a::make_result_response(rpc_id, a2a::to_json(t)));
            return;
        }
        write_a2a_rpc(fd, a2a::make_result_response(
            rpc_id, a2a::to_json(task_from_record(*rec))));
        return;
    }

    // Not in-flight.  If the task exists in the DB and is already
    // terminal we surface NotCancelable; otherwise NotFound.
    auto rec = tenants.get_a2a_task(tenant.id, task_id);
    if (!rec) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_TASK_NOT_FOUND,
            "no task with id '" + task_id + "' for this tenant"));
        return;
    }
    write_a2a_rpc(fd, a2a::make_error_response(
        rpc_id, a2a::ERR_TASK_NOT_CANCELABLE,
        "task is in terminal state '" + rec->state +
        "' and is not in-flight"));
}

// A2A `tasks/resubscribe` — replay missed task state and live-tail
// while the task is running.  We map task_id 1:1 to arbiter's request_id
// (the A2A handler set it that way at submit time), so the underlying
// log + bus is the same one /v1/requests/:id/events uses; we just wrap
// each event in an A2A envelope.
void handle_a2a_tasks_resubscribe(int fd,
                                    const std::shared_ptr<JsonValue>& rpc_id,
                                    const std::string& /*agent_id_param*/,
                                    const a2a::RpcRequest& rpc,
                                    TenantStore& tenants,
                                    const Tenant& tenant,
                                    RequestEventBus* bus) {
    if (!rpc.params || !rpc.params->is_object()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "params required"));
        return;
    }
    auto id_v = rpc.params->get("id");
    if (!id_v || !id_v->is_string()) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::RPC_INVALID_PARAMS, "id (string) required"));
        return;
    }
    const std::string task_id = id_v->as_string();

    // Validate ownership via request_status (same row underpinning the
    // arbiter-native resubscribe surface) AND the legacy a2a_tasks
    // table (the source of truth for non-orchestrate A2A submissions).
    auto rs = tenants.get_request_status(tenant.id, task_id);
    auto a2a_task = tenants.get_a2a_task(tenant.id, task_id);
    if (!rs && !a2a_task) {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc_id, a2a::ERR_TASK_NOT_FOUND,
            "no task with id '" + task_id + "' for this tenant"));
        return;
    }

    const std::string context_id = a2a_task ? a2a_task->context_id : "";
    const std::string agent_id   = a2a_task ? a2a_task->agent_id
                                            : (rs ? rs->agent_id : "index");

    // Open the SSE response.  CORS + no-buffering so dev-mode clients
    // and reverse proxies behave.
    {
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/event-stream\r\n"
            << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            << "X-Accel-Buffering: no\r\n"
            << kCorsHeaders
            << "Connection: close\r\n"
            << "\r\n";
        write_all(fd, hdr.str());
    }

    SseStream sse(fd);  // no headers re-write; we already wrote them
    auto sink = [&sse](const std::string& ev,
                        std::shared_ptr<JsonValue> payload) {
        sse.emit(ev, std::move(payload));
    };
    a2a::A2aStreamWriter writer(sink, rpc_id, task_id, context_id, agent_id);

    // If terminal already, emit one final TaskStatusUpdateEvent and close.
    if (rs && rs->state != "running") {
        a2a::TaskState ts =
            (rs->state == "completed") ? a2a::TaskState::completed :
            (rs->state == "canceled")  ? a2a::TaskState::canceled  :
                                          a2a::TaskState::failed;
        writer.emit_status(ts, /*final=*/true);
        return;
    }
    if (!rs && a2a_task) {
        // No request_status row but there's a legacy a2a_tasks row —
        // use that for state.  Translate the wire-string back to enum.
        const std::string& s = a2a_task->state;
        if (s == "completed" || s == "failed" || s == "canceled" ||
            s == "rejected") {
            a2a::TaskState ts =
                (s == "completed") ? a2a::TaskState::completed :
                (s == "canceled")  ? a2a::TaskState::canceled  :
                (s == "rejected")  ? a2a::TaskState::rejected  :
                                      a2a::TaskState::failed;
            writer.emit_status(ts, /*final=*/true);
            return;
        }
    }

    // Running: emit a `working` status frame, replay the persisted
    // text/tool/file events translated into A2A envelopes, then live-
    // tail until terminal.
    writer.emit_status(a2a::TaskState::working, /*final=*/false);

    auto translate_and_emit =
        [&writer](const TenantStore::RequestEvent& e) {
            // We translate the canonical events: text deltas, tool
            // calls, files, sub-agent completions, and `done`.  Anything
            // else lands as x-arbiter.<kind> metadata so spec-aware
            // clients can pass it through if they want it.
            if (e.event_kind == "text") {
                std::shared_ptr<JsonValue> p;
                try { p = json_parse(e.payload_json); } catch (...) {}
                if (p && p->is_object()) {
                    auto d = p->get("delta");
                    if (d && d->is_string()) {
                        writer.emit_text_chunk(d->as_string(), false);
                        return;
                    }
                }
            } else if (e.event_kind == "tool_call") {
                std::shared_ptr<JsonValue> p;
                try { p = json_parse(e.payload_json); } catch (...) {}
                if (p && p->is_object()) {
                    auto k  = p->get("kind");
                    auto ok = p->get("ok");
                    writer.emit_tool_call(
                        (k && k->is_string()) ? k->as_string() : "?",
                        (ok && ok->is_bool()) ? ok->as_bool() : false);
                    return;
                }
            } else if (e.event_kind == "file") {
                std::shared_ptr<JsonValue> p;
                try { p = json_parse(e.payload_json); } catch (...) {}
                if (p && p->is_object()) {
                    auto pa = p->get("path");
                    auto co = p->get("content");
                    auto mt = p->get("mime_type");
                    writer.emit_file(
                        (pa && pa->is_string()) ? pa->as_string() : "",
                        (co && co->is_string()) ? co->as_string() : "",
                        (mt && mt->is_string()) ? mt->as_string() : "text/plain");
                    return;
                }
            } else if (e.event_kind == "sub_agent_response") {
                std::shared_ptr<JsonValue> p;
                try { p = json_parse(e.payload_json); } catch (...) {}
                if (p && p->is_object()) {
                    auto a = p->get("agent");
                    auto d = p->get("depth");
                    auto c = p->get("content");
                    writer.emit_sub_agent(
                        (a && a->is_string()) ? a->as_string() : "?",
                        (d && d->is_number()) ? static_cast<int>(d->as_number()) : 1,
                        (c && c->is_string()) ? c->as_string() : "");
                    return;
                }
            }
            // Pass-through metadata for anything else (token_usage,
            // stream_start, stream_end, etc.).  Spec-aware clients
            // ignore unknown metadata kinds.
            std::shared_ptr<JsonValue> p;
            try { p = json_parse(e.payload_json); } catch (...) {}
            writer.emit_metadata(e.event_kind, p ? p : jobj());
        };

    int64_t cursor = 0;
    while (true) {
        auto chunk = tenants.list_request_events(tenant.id, task_id,
                                                    cursor, 1000);
        if (chunk.empty()) break;
        for (const auto& e : chunk) {
            translate_and_emit(e);
            cursor = e.seq;
        }
        if (chunk.size() < 1000) break;
    }

    auto post_replay = tenants.get_request_status(tenant.id, task_id);
    if (!post_replay || post_replay->state != "running") {
        a2a::TaskState ts = !post_replay ? a2a::TaskState::failed :
            (post_replay->state == "completed") ? a2a::TaskState::completed :
            (post_replay->state == "canceled")  ? a2a::TaskState::canceled  :
                                                   a2a::TaskState::failed;
        writer.emit_status(ts, /*final=*/true);
        return;
    }
    if (!bus) {
        // No live-tail wire — close with the current state as terminal
        // would be wrong (the run is still in flight); just exit and
        // let the client reconnect later with the new since_seq.
        return;
    }

    // Live tail.
    std::mutex                       mb_mu;
    std::condition_variable          mb_cv;
    std::deque<RequestEventEnvelope> mailbox;
    std::atomic<bool>                saw_terminal{false};

    int64_t sub_id = bus->subscribe(task_id,
        [&mb_mu, &mb_cv, &mailbox, &saw_terminal](const RequestEventEnvelope& env) {
            std::lock_guard<std::mutex> lk(mb_mu);
            mailbox.push_back(env);
            if (env.terminal) saw_terminal = true;
            mb_cv.notify_one();
        });

    using clock = std::chrono::steady_clock;
    auto next_heartbeat = clock::now() + std::chrono::seconds(30);
    while (true) {
        RequestEventEnvelope ev;
        bool have = false;
        bool was_terminal = false;
        {
            std::unique_lock<std::mutex> lk(mb_mu);
            mb_cv.wait_until(lk, next_heartbeat,
                [&]{ return !mailbox.empty(); });
            if (!mailbox.empty()) {
                ev = mailbox.front();
                mailbox.pop_front();
                have = true;
                was_terminal = ev.terminal;
            }
        }
        if (have) {
            if (ev.seq > cursor) {
                TenantStore::RequestEvent re;
                re.event_kind   = ev.event_kind;
                re.payload_json = ev.payload_json;
                translate_and_emit(re);
                cursor = ev.seq;
            }
            if (was_terminal) {
                // Final status from the persisted row.
                auto fresh = tenants.get_request_status(tenant.id, task_id);
                a2a::TaskState ts = !fresh ? a2a::TaskState::failed :
                    (fresh->state == "completed") ? a2a::TaskState::completed :
                    (fresh->state == "canceled")  ? a2a::TaskState::canceled  :
                                                     a2a::TaskState::failed;
                writer.emit_status(ts, /*final=*/true);
                break;
            }
            continue;
        }
        // Heartbeat — same shape as the native resubscribe, the SSE
        // comment line keeps the connection alive without committing
        // a payload that A2A clients would have to ignore.
        const char* hb = ": heartbeat\n\n";
        write_all(fd, hb, std::strlen(hb));
        next_heartbeat = clock::now() + std::chrono::seconds(30);
    }

    bus->unsubscribe(sub_id);
}

void handle_a2a_rpc(int fd, const std::string& agent_id,
                     const HttpRequest& req,
                     const ApiServerOptions& opts,
                     TenantStore& tenants,
                     InFlightRegistry& in_flight,
                     const Tenant& tenant,
                     RequestEventBus* request_event_bus = nullptr) {
    // Version negotiation per spec section 3.6.  Empty header is
    // tolerated as 1.0 since every implementation in the wild today
    // omits it; explicit 1.0 + 1 (loose major) are accepted; anything
    // else fails with VersionNotSupportedError.
    if (auto it = req.headers.find("a2a-version"); it != req.headers.end()) {
        const std::string v = it->second;
        if (!v.empty() && v != "1.0" && v != "1") {
            write_a2a_rpc(fd, a2a::make_error_response(
                nullptr, a2a::ERR_VERSION_NOT_SUPPORTED,
                "arbiter speaks A2A 1.0 only; got A2A-Version: " + v));
            return;
        }
    }

    std::shared_ptr<JsonValue> body;
    try { body = json_parse(req.body); }
    catch (const std::exception& e) {
        write_a2a_rpc(fd, a2a::make_error_response(
            nullptr, a2a::RPC_PARSE_ERROR,
            std::string("malformed JSON: ") + e.what()));
        return;
    }
    if (!body) {
        write_a2a_rpc(fd, a2a::make_error_response(
            nullptr, a2a::RPC_PARSE_ERROR, "empty body"));
        return;
    }

    a2a::RpcRequest rpc;
    try { rpc = a2a::rpc_request_from_json(*body); }
    catch (const std::exception& e) {
        std::shared_ptr<JsonValue> id;
        if (body->is_object()) id = body->get("id");
        write_a2a_rpc(fd, a2a::make_error_response(
            id, a2a::RPC_INVALID_REQUEST, e.what()));
        return;
    }

    if (rpc.method == "message/send") {
        handle_a2a_message_send(fd, rpc.id, agent_id, rpc,
                                 opts, tenants, in_flight, tenant);
        return;
    }
    if (rpc.method == "message/stream") {
        handle_a2a_message_stream(fd, rpc.id, agent_id, rpc,
                                   opts, tenants, in_flight, tenant);
        return;
    }
    if (rpc.method == "tasks/get") {
        handle_a2a_tasks_get(fd, rpc.id, rpc, tenants, tenant);
        return;
    }
    if (rpc.method == "tasks/cancel") {
        handle_a2a_tasks_cancel(fd, rpc.id, rpc, in_flight, tenants, tenant);
        return;
    }
    if (rpc.method == "tasks/resubscribe") {
        handle_a2a_tasks_resubscribe(fd, rpc.id, agent_id, rpc,
                                       tenants, tenant, request_event_bus);
        return;
    }
    if (rpc.method == "tasks/pushNotificationConfig/set"
        || rpc.method == "tasks/pushNotificationConfig/get"
        || rpc.method == "tasks/pushNotificationConfig/list"
        || rpc.method == "tasks/pushNotificationConfig/delete") {
        write_a2a_rpc(fd, a2a::make_error_response(
            rpc.id, a2a::ERR_UNSUPPORTED_OPERATION,
            "method not implemented"));
        return;
    }
    write_a2a_rpc(fd, a2a::make_error_response(
        rpc.id, a2a::RPC_METHOD_NOT_FOUND,
        "unknown method: " + rpc.method));
}

// Parse the inbound `message` field into a parts vector + text view.
// Accepts either:
//   • a plain string  → single TEXT part, message_text == the string;
//   • an array of part objects (mirroring Anthropic's content shape):
//       [{"type":"text","text":"..."},
//        {"type":"image","source":{"type":"base64","media_type":"image/png",
//                                  "data":"<base64 bytes>"}},
//        {"type":"image","source":{"type":"url","url":"https://..."}} ]
//
// URL-form image parts are resolved server-side against `cmd_fetch_bytes`
// so the bytes flow inline into the provider call alongside any base64 the
// caller supplied directly.  Caps each fetched image at `kImageMaxBytes`.
//
// `out_text` carries the flattened text view used everywhere the runtime
// still expects a string (history persistence, billing pre-flight,
// invoker context).  Image parts contribute zero bytes to it.
//
// On success, returns true and populates out_parts + out_text.
// On failure, returns false and writes a clear, caller-safe message into
// `error_msg`.  The caller is responsible for the HTTP response.
namespace {
constexpr int64_t kImageMaxBytes = 20LL * 1024 * 1024;  // 20 MB

bool parse_message_field(const std::shared_ptr<JsonValue>& msg_val,
                          std::vector<ContentPart>& out_parts,
                          std::string& out_text,
                          std::string& error_msg) {
    out_parts.clear();
    out_text.clear();
    error_msg.clear();

    if (!msg_val) {
        error_msg = "missing required field: 'message'";
        return false;
    }

    // Legacy: bare string.
    if (msg_val->is_string()) {
        const std::string s = msg_val->as_string();
        if (s.empty()) {
            error_msg = "missing required field: 'message'";
            return false;
        }
        ContentPart p;
        p.kind = ContentPart::TEXT;
        p.text = s;
        out_parts.push_back(std::move(p));
        out_text = s;
        return true;
    }

    if (!msg_val->is_array()) {
        error_msg = "'message' must be a string or an array of parts";
        return false;
    }

    for (auto& entry : msg_val->as_array()) {
        if (!entry || !entry->is_object()) {
            error_msg = "'message' parts must be objects";
            return false;
        }
        const std::string type = entry->get_string("type");
        if (type == "text") {
            ContentPart p;
            p.kind = ContentPart::TEXT;
            p.text = entry->get_string("text");
            if (p.text.empty()) {
                error_msg = "text part has empty 'text'";
                return false;
            }
            if (!out_text.empty() && out_text.back() != '\n') out_text += '\n';
            out_text += p.text;
            out_parts.push_back(std::move(p));
        } else if (type == "image") {
            auto src = entry->get("source");
            if (!src || !src->is_object()) {
                error_msg = "image part missing 'source' object";
                return false;
            }
            const std::string src_type = src->get_string("type");
            ContentPart p;
            p.kind = ContentPart::IMAGE;
            if (src_type == "base64") {
                p.media_type = src->get_string("media_type");
                p.image_data = src->get_string("data");
                if (p.media_type.empty() || p.image_data.empty()) {
                    error_msg = "base64 image source needs both 'media_type' "
                                "and 'data'";
                    return false;
                }
                if (p.media_type.compare(0, 6, "image/") != 0) {
                    error_msg = "image media_type must start with 'image/'";
                    return false;
                }
            } else if (src_type == "url") {
                const std::string url = src->get_string("url");
                if (url.empty()) {
                    error_msg = "url image source needs 'url'";
                    return false;
                }
                // Server-side resolution.  Validates the response is
                // image/* and bounded — clients can't smuggle non-image
                // payloads through this path.
                FetchedResource fr = cmd_fetch_bytes(url, kImageMaxBytes);
                if (!fr.ok) {
                    error_msg = "image url fetch failed: " + fr.error;
                    return false;
                }
                if (fr.content_type.compare(0, 6, "image/") != 0) {
                    error_msg = "image url returned non-image content-type: " +
                                fr.content_type;
                    return false;
                }
                p.media_type = fr.content_type;
                p.image_data = base64_encode(fr.body);
            } else {
                error_msg = "image source.type must be 'base64' or 'url'";
                return false;
            }
            out_parts.push_back(std::move(p));
        } else {
            error_msg = "unknown part type: '" + type + "'";
            return false;
        }
    }

    if (out_parts.empty()) {
        error_msg = "'message' array is empty";
        return false;
    }
    if (out_text.empty()) {
        // Image-only messages get a synthetic text view so legacy paths
        // (history persistence, advisor original_task) have something to
        // hold.  The model still sees the full parts vector.
        out_text = "(image input)";
    }
    return true;
}
}  // namespace

// Read the `Idempotency-Key` header, trim whitespace, length-cap.
// Returns empty string when absent or invalid — caller proceeds normally
// without dedup.  The cap (256 chars) keeps a malicious client from
// bloating the in-memory cache with arbitrarily long keys.
std::string read_idempotency_key(const HttpRequest& req) {
    auto it = req.headers.find("idempotency-key");
    if (it == req.headers.end()) return {};
    std::string k = it->second;
    while (!k.empty() && (k.back()  == ' ' || k.back()  == '\t' ||
                          k.back()  == '\r' || k.back()  == '\n')) k.pop_back();
    while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(0, 1);
    if (k.size() > 256) return {};
    return k;
}

// Look up the request_id this idempotency key already maps to.  Empty
// optional ⇒ key absent or cache miss; caller proceeds with a fresh
// execution.  Non-empty ⇒ a previous request claimed this key; caller
// should call handle_request_events with the returned id and return.
std::optional<std::string>
check_idempotency_replay(const ApiServerOptions& opts,
                          const HttpRequest& req, int64_t tenant_id) {
    if (!opts.idempotency) return std::nullopt;
    std::string k = read_idempotency_key(req);
    if (k.empty()) return std::nullopt;
    auto entry = opts.idempotency->get(tenant_id, k);
    if (!entry) return std::nullopt;
    return entry->request_id;
}

// Register a new (tenant_id, idempotency-key) → request_id mapping.
// No-op when the header is absent or the cache isn't wired.  Called
// after the request_status row is created so a concurrent retry
// finds a valid id to replay against.
void record_idempotency_key(const ApiServerOptions& opts,
                              const HttpRequest& req, int64_t tenant_id,
                              const std::string& request_id) {
    if (!opts.idempotency) return;
    std::string k = read_idempotency_key(req);
    if (k.empty()) return;
    opts.idempotency->put(tenant_id, k, request_id);
}

// Two entry points funnel here: /v1/orchestrate (agent_override == "", read
// from body) and /v1/agents/:id/chat (agent_override == path :id).  Body
// parsing + dispatch is otherwise identical.
void handle_orchestrate(int fd, const HttpRequest& req,
                        const ApiServerOptions& opts,
                        TenantStore& tenants,
                        InFlightRegistry& in_flight,
                        // Nullable.  When non-null, drives pre-flight quota
                        // checks and post-turn usage records — the runtime
                        // becomes a thin gateway and the billing service owns
                        // billing.  When null, neither call fires; the
                        // turn runs straight through to the provider keys
                        // configured in `opts.api_keys`.
                        BillingClient* billing,
                        // The billing service's workspace_id the bearer maps to
                        // (returned from /v1/runtime/auth/validate).  Empty
                        // when `billing` is null.
                        const std::string& workspace_id,
                        const Tenant& tenant_in,
                        // Optional per-request event bus.  Non-null ⇒ the
                        // SSE writer mirrors every event to request_events
                        // and broadcasts to live-tail subscribers (the
                        // resubscribe handler).  Null ⇒ the request runs
                        // without durable persistence; useful for tests
                        // and for the legacy in-memory-only path during
                        // gradual rollout.
                        RequestEventBus* request_event_bus = nullptr,
                        const std::string& agent_override = "",
                        int64_t conversation_id = 0,
                        // Snapshotted agent_def from the conversation row.  When
                        // this is non-empty and the request body doesn't supply
                        // its own `agent_def`, we install the snapshot so
                        // follow-up messages don't have to re-send it on every
                        // turn.  Body-supplied agent_def still wins if both
                        // are present (lets callers update an agent mid-thread).
                        const std::string& conversation_agent_def_json = "") {
    Tenant tenant = tenant_in;   // mutable snapshot — MTD refreshes mid-request

    // Idempotency-Key short-circuit.  When the client supplies a key we
    // already have a request_id for, redirect into the resubscribe path
    // so the retry receives the same SSE stream (live tail or
    // backlog replay, depending on the original's state) instead of
    // triggering a second execution.  In-memory cache; a process
    // restart resets it.  Body is intentionally not part of the dedup
    // contract for v1 — clients retrying after a network blip send the
    // same body.
    if (auto replay_id = check_idempotency_replay(opts, req, tenant.id)) {
        if (opts.metrics) opts.metrics->inc_idempotency_replay();
        handle_request_events(fd, *replay_id, req, tenants, tenant,
                              request_event_bus);
        return;
    }
    // No cached hit, but the client still sent an Idempotency-Key —
    // count it for ops visibility into "how many writes use the
    // header" versus "how many actually dedup'd."
    if (opts.metrics && !read_idempotency_key(req).empty()) {
        opts.metrics->inc_idempotency_miss();
    }

    // Parse the JSON body.
    std::shared_ptr<JsonValue> body;
    try {
        body = json_parse(req.body);
    } catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr(std::string("invalid JSON: ") + e.what());
        write_json_response(fd, 400, err);
        return;
    }
    if (!body || !body->is_object()) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("request body must be a JSON object");
        write_json_response(fd, 400, err);
        return;
    }

    std::vector<ContentPart> message_parts;
    std::string              message;
    {
        std::string parse_err;
        if (!parse_message_field(body->get("message"),
                                  message_parts, message, parse_err)) {
            auto err = jobj();
            err->as_object_mut()["error"] = jstr(parse_err);
            write_json_response(fd, 400, err);
            return;
        }
    }

    // ── Resolve the agent identity ────────────────────────────────────────
    //
    // For inline `agent_def`, the caller supplies a stable `id` (typically a
    // UUID from their own DB).  Memory for this agent is persisted at
    // `<tenant_memory>/<id>.md`, so repeated requests carrying the same
    // `agent_def.id` share memory across turns regardless of what other
    // config fields change between calls.  Precedence:
    //   1. agent_def.id                 (strongest — the memory-persistence
    //                                     identity for dynamic agents)
    //   2. path :id                     (/v1/agents/:id/chat)
    //   3. body.agent                   (/v1/orchestrate fallback)
    //   4. fallback "index"             (no agent_def, nothing else specified)
    //
    // When multiple of 1–3 are set, they MUST agree.  Conflicts fail fast
    // with 400 so the caller doesn't silently write memory to the wrong key.
    std::shared_ptr<JsonValue> agent_def;
    if (auto v = body->get("agent_def"); v && v->is_object()) agent_def = v;

    // Fall back to the conversation's snapshotted agent_def when the request
    // body didn't supply its own.  The snapshot is the source of truth for
    // resumed threads — without this, a follow-up turn that omits agent_def
    // would have no way to find the agent (the API does not read disk-side
    // .json definitions).
    if (!agent_def && !conversation_agent_def_json.empty()) {
        try {
            auto parsed = json_parse(conversation_agent_def_json);
            if (parsed && parsed->is_object()) agent_def = parsed;
        } catch (...) {
            // A corrupted snapshot shouldn't 500 the call — surface a clear
            // 400 instead so the caller knows to re-send agent_def or
            // recreate the conversation.
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr("conversation has a corrupted agent_def snapshot — "
                     "send a fresh agent_def in the request body, or "
                     "recreate the conversation.");
            write_json_response(fd, 400, err);
            return;
        }
    }

    const std::string path_id         = agent_override;
    const std::string body_agent      = body->get_string("agent", "");
    const std::string agent_def_id    = agent_def ? agent_def->get_string("id", "")
                                                  : std::string{};

    auto conflict = [&](const std::string& kind_a, const std::string& a,
                        const std::string& kind_b, const std::string& b) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr(kind_a + " ('" + a + "') and " + kind_b + " ('" + b +
                 "') disagree.  When both are set they must be identical — "
                 "otherwise memory would silently persist under the wrong id.");
        write_json_response(fd, 400, err);
    };

    std::string agent_id;
    if (!agent_def_id.empty()) {
        agent_id = agent_def_id;
        if (!path_id.empty() && path_id != agent_id) {
            conflict("path :id", path_id, "agent_def.id", agent_id);
            return;
        }
        if (!body_agent.empty() && body_agent != agent_id) {
            conflict("body.agent", body_agent, "agent_def.id", agent_id);
            return;
        }
    } else if (!path_id.empty()) {
        agent_id = path_id;
    } else if (!body_agent.empty()) {
        agent_id = body_agent;
    } else {
        agent_id = "index";
    }

    // Guardrail: the resolved id becomes a filesystem path component for
    // the memory file — reject traversal / shell metacharacters / absurd
    // lengths up front.  Allows alphanum, underscore, dash (covers UUIDs).
    if (!agent_id_is_safe(agent_id)) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("invalid agent id '" + agent_id +
                 "'.  Allowed: 1-64 chars, [a-zA-Z0-9_-] only, "
                 "first char not '.' or '/'.");
        write_json_response(fd, 400, err);
        return;
    }

    // Pre-validate agent_def early (before opening the SSE stream).  Also
    // catches the "can't override master" case up front so the error is
    // a clean JSON 400, not an SSE frame wedged into a half-written stream.
    std::optional<Constitution> parsed_cfg;
    if (agent_def) {
        if (agent_id == "index") {
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr("inline agent_def cannot override 'index' — pick a "
                     "different id.");
            write_json_response(fd, 400, err);
            return;
        }
        try {
            parsed_cfg = Constitution::from_json(json_serialize(*agent_def));
        } catch (const std::exception& e) {
            auto err = jobj();
            err->as_object_mut()["error"] =
                jstr(std::string("invalid agent_def: ") + e.what());
            write_json_response(fd, 400, err);
            return;
        }
    }

    // Begin the SSE response.
    SseStream sse(fd);
    sse.write_headers();

    const auto start_time = std::chrono::steady_clock::now();

    // Allocate request_id and the server-side event logger up front so
    // every error path below — including orchestrator-init failure —
    // logs to stderr alongside its SSE error frame.
    const std::string request_id = new_request_id();
    EventLogger logger(opts.log_verbose, request_id, tenant.name);

    // Metrics: classify the route from the entry-point arguments
    // (handle_orchestrate is the shared backend for three URL paths).
    const std::string metrics_route =
        conversation_id > 0 ? "messages" :
        !agent_override.empty() ? "agent_chat" : "orchestrate";
    struct RequestMetricScope {
        Metrics*    m;
        int64_t     tid;
        std::string route;
        std::chrono::steady_clock::time_point start;
        bool        ok = false;
        ~RequestMetricScope() {
            if (!m) return;
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count();
            m->add_request_duration_ms(tid, route, ms);
            m->inc_request_completed(tid, route, ok);
            m->dec_in_flight(tid);
        }
    } metric_scope{opts.metrics, tenant.id, metrics_route, start_time};
    if (opts.metrics) {
        opts.metrics->inc_request_started(tenant.id, metrics_route);
        opts.metrics->inc_in_flight(tenant.id);
    }

    // Wire durable SSE persistence + live-tail broadcast.  Insert the
    // request_status row first so a reconnecting client can find the
    // run by id even if no events have been persisted yet (a slow
    // start-up where the orchestrator init takes seconds, for example).
    // The append_request_event statement enforces FK to request_status,
    // so the create_request_status call MUST land before any emit().
    const int64_t now_s_for_request = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const bool persist_events = (request_event_bus != nullptr);
    bool       request_status_created = false;
    if (persist_events) {
        try {
            tenants.create_request_status(tenant.id, request_id,
                /*agent_id=*/agent_override.empty() ? "" : agent_override,
                conversation_id, now_s_for_request);
            request_status_created = true;
            sse.set_persistence(&tenants, request_event_bus,
                                tenant.id, request_id);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[orchestrate] persist init failed for %s: %s\n",
                request_id.c_str(), e.what());
            // Carry on without persistence rather than 500ing the call;
            // durable replay degrades, the wire stream still works.
        }
    }

    // Register the idempotency key now that we have a request_id a
    // replay can target.  Racing retries from the same client will
    // either land here first (winner inserts; loser's check_idempotency
    // hit replays the winner) or land here second (their put() returns
    // false; both executions run, but subsequent retries dedup).
    record_idempotency_key(opts, req, tenant.id, request_id);

    auto emit = [&sse, &logger](const std::string& ev,
                                 const std::shared_ptr<JsonValue>& p) {
        sse.emit(ev, p);
        logger.log(ev, p);
    };
    auto log_error = [&emit](const std::string& msg) {
        auto o = jobj();
        o->as_object_mut()["message"] = jstr(msg);
        emit("error", o);
    };

    // Per-request Orchestrator.  Concurrent API calls don't share agent
    // history or callback state — each request is a fresh universe.  The
    // The API path does NOT load agent .json definitions from disk.  It
    // does install the tenant's stored agent catalog (`POST /v1/agents`)
    // onto every per-request orchestrator so /agent and /parallel can
    // reference siblings by id without re-sending their constitutions.
    // Inline `agent_def` from the request body still wins on a colliding
    // id — useful for one-off ephemeral agents and mid-thread overrides.
    std::unique_ptr<Orchestrator> orch;
    try {
        orch = std::make_unique<Orchestrator>(opts.api_keys);
    } catch (const std::exception& e) {
        log_error(std::string("orchestrator init failed: ") + e.what());
        return;
    }
    // Memory is tenant-scoped so /mem commands can never leak between
    // accounts.  set_memory_dir is kept as a no-op fallback path for
    // any code that still expects a filesystem location, but the
    // canonical scratchpad storage is now the agent_scratchpad table
    // wired below.
    if (!opts.memory_root.empty()) {
        orch->set_memory_dir(opts.memory_root + "/t" +
                              std::to_string(tenant.id));
    }

    // DB-backed file-scratchpad bridge.  /mem read|write|clear and
    // /mem shared read|write|clear all flow through here, scoped to
    // this request's tenant.  Without this callback the dispatcher
    // would fall back to the filesystem path — fine for the CLI/REPL
    // (which doesn't have a tenant), but the API path always wires it.
    orch->set_memory_scratchpad(
        make_memory_scratchpad_callback(tenant.id, &tenants));

    // Install the tenant's stored catalog first so /agent and /parallel
    // can resolve sibling ids during this turn.  A blob whose JSON has
    // gone bad (schema drift after an upgrade, manual DB poke) gets
    // skipped with a log line — the rest of the catalog still loads.
    {
        const auto records = tenants.list_agent_records(tenant.id, /*limit=*/200);
        for (const auto& rec : records) {
            try {
                auto cfg = Constitution::from_json(rec.agent_def_json);
                if (orch->has_agent(rec.agent_id)) orch->remove_agent(rec.agent_id);
                orch->create_agent(rec.agent_id, std::move(cfg));
            } catch (const std::exception& e) {
                log_error("skipping stored agent '" + rec.agent_id + "' for tenant "
                           + std::to_string(tenant.id) + ": " + e.what());
            }
        }
    }

    // Install the inline agent definition (pre-validated above, so this
    // can't throw the user-visible errors — any failure now is internal
    // and surfaces as an SSE `error` event).  Inline wins over the stored
    // catalog when ids collide.
    if (parsed_cfg) {
        if (orch->has_agent(agent_id)) orch->remove_agent(agent_id);
        orch->create_agent(agent_id, std::move(*parsed_cfg));
    } else if (agent_id != "index" && !orch->has_agent(agent_id)) {
        // No inline agent_def, no snapshot, no stored catalog row, and
        // the caller didn't ask for the master.  Surface a clean SSE
        // error so the caller knows what to send next time.
        log_error("agent_def required for agent '" + agent_id + "' — no "
                  "stored agent with this id for the tenant.  Send `agent_def` "
                  "in the request body, POST it once to /v1/agents, or address "
                  "'index' (the master orchestrator) instead.");
        sse.close();
        return;
    }

    auto* orch_ptr = orch.get();

    // Register this orchestration so `POST /v1/requests/:id/cancel` can
    // reach it.  Lifetime matches the orchestrator; scope unwinds on every
    // exit path (including exceptions), so cancels arriving after
    // completion harmlessly miss the map.
    InFlightScope in_flight_scope(in_flight, request_id, orch_ptr, tenant.id);

    // ── Conversation thread resumption ──────────────────────────────────
    // When this request belongs to a stored conversation, replay prior
    // messages into the agent so the model sees the full history, then
    // append the user's new message to the DB.  Persistence of the
    // assistant's response happens after send_streaming returns (below).
    if (conversation_id > 0) {
        try {
            // Hard-cap history replay at 100 turns to keep token usage and
            // request payload size bounded.  If a thread is older, the
            // tail (most recent) is what gets sent — older context falls
            // off, which matches both Claude's and ChatGPT's default UX.
            const int kReplayCap = 100;
            auto prior = tenants.list_messages(tenant.id, conversation_id,
                                                /*after_id=*/0, kReplayCap);
            std::vector<Message> hist;
            hist.reserve(prior.size());
            for (auto& pm : prior) hist.push_back({pm.role, pm.content});
            orch->set_agent_history(agent_id, std::move(hist));
        } catch (const std::out_of_range&) {
            // Agent isn't loaded — surface as SSE error.
            log_error("agent '" + agent_id + "' not loaded for "
                      "conversation resumption");
            return;
        } catch (const std::exception& e) {
            log_error(std::string("history load failed: ") + e.what());
            return;
        }
        try {
            tenants.append_message(tenant.id, conversation_id,
                                    "user", message,
                                    /*input=*/0, /*output=*/0,
                                    request_id);
        } catch (const std::exception& e) {
            log_error(std::string("could not persist user message: ") + e.what());
            return;
        }
    }

    // Helper: stamp every outbound event with the current turn's
    // (agent, stream_id, depth).  Read lazily at emit time because each
    // turn runs inside its own StreamScope; this lets the same callback
    // serve master + delegated + parallel children without threading the
    // ids explicitly.
    auto stamp = [orch_ptr](std::shared_ptr<JsonValue>& p) {
        auto& m = p->as_object_mut();
        m["agent"]     = jstr(orch_ptr->current_stream_agent());
        m["stream_id"] = jnum(static_cast<double>(orch_ptr->current_stream_id()));
        m["depth"]     = jnum(static_cast<double>(orch_ptr->current_stream_depth()));
    };

    // File-write interceptor — captures content to the SSE stream so the
    // client sees every generated file regardless of where the bytes
    // also live.  When the sandbox is wired, the same bytes also land
    // in the tenant's workspace volume so a subsequent /exec inside the
    // container can read what /write just produced.  Per-response size
    // cap stops a runaway agent from OOMing the SSE buffer; once
    // exceeded, further writes are rejected with an ERR.
    std::atomic<size_t> bytes_captured{0};
    const size_t cap = opts.file_max_bytes;
    SandboxManager* sandbox_mgr = opts.sandbox;
    const int64_t sandbox_tid   = tenant.id;
    auto write_interceptor =
        [&emit, &bytes_captured, cap, stamp,
         sandbox_mgr, sandbox_tid](const std::string& path,
                                    const std::string& content) -> std::string {
        size_t size = content.size();
        size_t prev = bytes_captured.load();
        if (prev + size > cap) {
            return "ERR: per-response file-size cap (" + std::to_string(cap) +
                   " bytes) reached — this file was NOT included in the "
                   "response.  Reduce the file size or split across requests.";
        }
        bytes_captured.fetch_add(size);

        auto p = jobj();
        auto& m = p->as_object_mut();
        m["path"]     = jstr(path);
        m["size"]     = jnum(static_cast<double>(size));
        m["encoding"] = jstr("utf-8");
        m["content"]  = jstr(content);
        stamp(p);
        emit("file", p);

        std::string note = "OK: captured " + std::to_string(size) +
            " bytes for '" + path + "' (streamed to client";
        if (sandbox_mgr) {
            std::string werr;
            if (sandbox_mgr->write_to_workspace(sandbox_tid, path, content, werr)) {
                note += "; also saved to /workspace/" + path + " in sandbox)";
            } else {
                note += "; sandbox write failed: " + werr + ")";
            }
        } else {
            note += ", not persisted)";
        }
        return note;
    };
    orch->set_write_interceptor(write_interceptor);
    orch->set_exec_disabled(opts.exec_disabled);
    if (auto exec_inv = make_exec_invoker_callback(opts, tenant.id)) {
        orch->set_exec_invoker(std::move(exec_inv));
    }
    orch->client().set_circuit_breaker(opts.circuit_breaker);
    orch->client().set_metrics(opts.metrics);

    // Real-time read window into structured memory.  Bound to this request's
    // tenant so /mem entries|entry|search inside any agent turn (master or
    // delegated) sees only that tenant's graph entries.  Subagents inherit
    // the reader through Orchestrator's member, so depth-2 calls are scoped
    // identically without an extra plumb-through.
    const int64_t reader_tenant_id = tenant.id;
    // Captured by value so /mem entry can compute whether a linked
    // artifact lives in the active conversation (no via= needed) or
    // elsewhere (suggest the via=mem:<mid> form).  0 ⇒ no active
    // conversation (raw /v1/orchestrate); any artifact link is
    // therefore cross-conversation by definition.
    const int64_t reader_conversation_id = conversation_id;
    orch->set_structured_memory_reader(
        make_structured_memory_reader_callback(
            tenants, reader_tenant_id, reader_conversation_id, orch_ptr));

    // Structured-memory writer.  Tenant-scoped (mirrors the reader); writes
    // land directly in the curated graph and are visible to subsequent
    // reads on the next turn.  /mem add entry requires a non-empty body
    // (passed in as `body`) — the dispatcher rejects empty bodies before
    // the request reaches us, so by the time we see it we can trust the
    // body is meaningful synthesised text.
    orch->set_structured_memory_writer(
        make_structured_memory_writer_callback(
            tenants, reader_tenant_id, reader_conversation_id, orch_ptr));

    // ── MCP session manager ───────────────────────────────────────────
    // One Manager per request; subprocesses spawn lazily on first /mcp
    // reference and die when the orchestrator's `mcp_mgr` shared_ptr
    // falls out of scope (which happens at the end of this function or
    // when the InFlightScope unwinds on cancel).  The manager lives
    // beyond the orchestrator only via the invoker capture — when the
    // captured lambda is destroyed, so is the manager.
    auto mcp_mgr = make_mcp_manager(opts,
        [&log_error](const std::string& m) { log_error(m); });
    orch->set_mcp_invoker(make_mcp_invoker_callback(mcp_mgr));

    // ── A2A remote-agent delegation ───────────────────────────────────
    // Wire the /a2a slash command so the master orchestrator (and any
    // delegated sub-agent) can call out to remote A2A agents listed in
    // opts.a2a_agents_path.  See make_a2a_invoker for the subcommand
    // surface.  The Manager's lifetime is owned by the captured lambda;
    // it dies when the orchestrator does.  Auto-routing injects remote
    // agents into the master's roster so /agent and /a2a call sit side
    // by side in the routing menu.
    {
        Orchestrator::RemoteRosterProvider roster_cb;
        if (auto a2a_inv = make_a2a_invoker(opts, &roster_cb)) {
            orch->set_a2a_invoker(std::move(a2a_inv));
            if (roster_cb) orch->set_remote_roster_provider(std::move(roster_cb));
        }
    }

    // ── Scheduler bridge ──────────────────────────────────────────────
    // /schedule resolves through this callback at every depth.  The
    // task's conversation_id is snapshotted from the request context —
    // this is what lets a scheduled run land its work in the same
    // thread the agent originally scheduled it from.
    orch->set_scheduler_invoker(
        make_scheduler_invoker_callback(tenants, tenant.id, conversation_id));

    // ── Todo bridge ──────────────────────────────────────────────────
    // /todo resolves through this callback.  Pinned to the request's
    // conversation_id (or 0 = unscoped for raw /v1/orchestrate) so
    // /todo list scopes to the active thread by default.
    orch->set_todo_invoker(
        make_todo_invoker_callback(tenants, tenant.id, conversation_id));

    // ── Lesson bridge ────────────────────────────────────────────────
    // /lesson resolves through this callback.  Lessons are agent-scoped
    // (not conversation-scoped) — they follow the agent's identity
    // across every conversation in the tenant.
    orch->set_lesson_invoker(
        make_lesson_invoker_callback(tenants, tenant.id));

    // ── Web search ────────────────────────────────────────────────────
    // /search <query> [top=N] dispatches against the configured provider.
    // Only "brave" is implemented in v1; an unrecognised provider returns
    // ERR rather than silently doing the wrong thing.  Captures the key
    // by value so a future request that reloads ApiServerOptions doesn't
    // race the in-flight lambda.
    if (auto search_inv = make_search_invoker_callback(opts)) {
        orch->set_search_invoker(std::move(search_inv));
    }
    // No key / unsupported provider ⇒ leave the invoker null; the
    // dispatcher returns its own ERR with a useful message.

    // ── Artifact store bridges ────────────────────────────────────────
    // Wire /write --persist, /read, and /list against TenantStore +
    // the active conversation_id.  When the request didn't come in
    // through a conversation (e.g. raw /v1/orchestrate without a thread),
    // the writer/reader/lister stay null — the agent's slash dispatchers
    // surface a clear "no conversation context" warning + ephemeral
    // fallback for /write --persist.
    if (conversation_id > 0) {
        orch->set_artifact_writer(
            make_artifact_writer_callback(tenant.id, conversation_id, &tenants));
        ArtifactReader base_reader =
            make_artifact_reader_callback(tenant.id, conversation_id, &tenants);
        ArtifactLister base_lister =
            make_artifact_lister_callback(tenant.id, conversation_id, &tenants);
        // Sandbox fallback for path-form /read and /list: when the
        // artifact store doesn't have a hit (or the conversation has
        // no artifacts) but the tenant's workspace does, serve the
        // bytes from there.  Artifact-id reads bypass the fallback
        // because the workspace has no concept of an artifact id; it's
        // purely path-keyed.
        if (sandbox_mgr) {
            const int64_t stid = tenant.id;
            orch->set_artifact_reader(
                [base_reader, sandbox_mgr, stid](
                    const std::string& path, int64_t aid,
                    int64_t via) -> ArtifactReadResult {
                    ArtifactReadResult r = base_reader(path, aid, via);
                    const bool is_err = r.body.size() >= 4 &&
                                        r.body.compare(0, 4, "ERR:") == 0;
                    if (!is_err || aid != 0 || path.empty()) return r;
                    std::string content, mime, werr;
                    if (sandbox_mgr->read_from_workspace(stid, path,
                            content, mime, werr)) {
                        ArtifactReadResult w;
                        w.body       = std::move(content);
                        w.media_type = std::move(mime);
                        return w;
                    }
                    return r;
                });
            orch->set_artifact_lister(
                [base_lister, sandbox_mgr, stid]() -> std::string {
                    std::string out = base_lister();
                    std::string ws  = sandbox_mgr->list_workspace(stid);
                    if (ws.empty()) return out;
                    if (!out.empty() && out.back() != '\n') out += '\n';
                    out += "\n[sandbox workspace]\n";
                    out += ws;
                    return out;
                });
        } else {
            orch->set_artifact_reader(std::move(base_reader));
            orch->set_artifact_lister(std::move(base_lister));
        }
    } else if (sandbox_mgr) {
        // Raw /v1/orchestrate (no conversation_id) but sandbox is wired:
        // /read and /list still work against the workspace.  /write
        // --persist remains a no-op (no artifact store w/o a thread).
        const int64_t stid = tenant.id;
        orch->set_artifact_reader(
            [sandbox_mgr, stid](const std::string& path, int64_t aid,
                                int64_t /*via*/) -> ArtifactReadResult {
                ArtifactReadResult r;
                if (aid != 0) {
                    r.body = "ERR: /read #<id> requires a conversation; this "
                             "request has none.  Use /read <path> against "
                             "the sandbox workspace instead.";
                    return r;
                }
                std::string content, mime, werr;
                if (!sandbox_mgr->read_from_workspace(stid, path,
                        content, mime, werr)) {
                    r.body = "ERR: " + werr;
                    return r;
                }
                r.body       = std::move(content);
                r.media_type = std::move(mime);
                return r;
            });
        orch->set_artifact_lister(
            [sandbox_mgr, stid]() -> std::string {
                std::string ws = sandbox_mgr->list_workspace(stid);
                if (ws.empty()) return std::string{};
                return "[sandbox workspace]\n" + ws;
            });
    }

    // ── Fleet lifecycle ────────────────────────────────────────────────
    // stream_start/stream_end bracket each turn; consumers open a UI slot
    // on stream_start and close it on stream_end.  Fires at every depth
    // — master, delegated, parallel children.  stream_start_cb itself is
    // installed further below so it can also populate the master filter's
    // shared-state capture (used by filter.flush() after StreamScope unwinds).
    // stream_end is wired further down (after the master's StreamFilter is
    // constructed) so the handler can flush buffered text before emitting
    // the SSE `stream_end` frame — otherwise the last buffered line arrives
    // after stream_end and the consumer has to special-case out-of-order
    // deltas.

    // Sub-agent text streaming.  Fires for every clean delta produced by
    // a delegated turn (depth > 0); master-depth deltas continue to flow
    // through send_streaming's cb (wired further down) so we don't double-
    // emit.  Each delta arrives already stripped of /cmd lines by an
    // orchestrator-side StreamFilter, tagged with this turn's (agent,
    // stream_id).  Multiple parallel children may call this concurrently
    // — SseStream's internal mutex serializes frame writes so the events
    // interleave safely on the wire.
    orch->set_agent_stream_callback(
        [&emit](const std::string& agent, int sid, const std::string& delta) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(agent);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["delta"]     = jstr(delta);
            // depth omitted for sub-agent text by historical convention; the
            // logger reads it as 0 if absent which is correct enough for a
            // glance — the structured `done` event has the canonical depth.
            emit("text", p);
        });

    // agent_start still fires for delegated turns (just before the API call).
    // Keep it so consumers can distinguish "turn opened" (stream_start) from
    // "API call about to go out" (agent_start) — different timings once
    // we add pre-call checks.
    orch->set_agent_start_callback([&emit, stamp](const std::string&) {
        auto p = jobj();
        stamp(p);
        emit("agent_start", p);
    });
    orch->set_tool_status_callback(
        [&emit, stamp](const std::string& kind, bool ok) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["tool"] = jstr(kind);
            m["ok"]   = jbool(ok);
            stamp(p);
            emit("tool_call", p);
        });
    // Per-turn telemetry.  Direct billing has been pulled out of the
    // runtime — the billing service (when configured) is the source of truth
    // for cost accounting, cap enforcement, and credit consumption.
    // The runtime no longer prices turns locally; the SSE event carries
    // raw token counts and the model id, and the billing service's
    // `usage/record` endpoint settles the µ¢ figure on its side.
    std::atomic<int> turn_counter{0};

    orch->set_cost_callback(
        [&emit, &turn_counter, billing, &workspace_id,
         &request_id, orch_ptr, stamp](const std::string& id,
                                          const std::string& model,
                                          const ApiResponse& resp) {
            // Per-turn idempotency key for the billing service.  The runtime's
            // request_id covers the whole orchestration (master + delegated
            // sub-agents); each cost-callback firing is one logical LLM
            // turn, so we suffix a counter to give the billing service a stable
            // unique id per turn that survives a retry of *that* turn.
            const int turn_idx = turn_counter.fetch_add(1);
            const std::string turn_request_id =
                request_id + "-t" + std::to_string(turn_idx);

            if (billing && billing->enabled() &&
                !workspace_id.empty()) {
                BillingClient::UsageRecord ur;
                ur.request_id    = turn_request_id;
                ur.workspace_id  = workspace_id;
                ur.model         = model;
                ur.input_tokens  = resp.input_tokens;
                ur.output_tokens = resp.output_tokens;
                ur.cached_tokens = resp.cache_read_tokens;
                ur.agent_id      = id;
                ur.depth         = orch_ptr->current_stream_depth();
                billing->record_usage(ur);
            }

            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]         = jstr(id);
            m["model"]         = jstr(model);
            m["input_tokens"]  = jnum(static_cast<double>(resp.input_tokens));
            m["output_tokens"] = jnum(static_cast<double>(resp.output_tokens));
            if (resp.cache_read_tokens > 0)
                m["cache_read_tokens"]   = jnum(static_cast<double>(resp.cache_read_tokens));
            if (resp.cache_creation_tokens > 0)
                m["cache_create_tokens"] = jnum(static_cast<double>(resp.cache_creation_tokens));
            m["stream_id"] = jnum(static_cast<double>(orch_ptr->current_stream_id()));
            m["depth"]     = jnum(static_cast<double>(orch_ptr->current_stream_depth()));
            emit("token_usage", p);
        });
    // progress_callback fires at depth>0 with the sub-agent's full turn
    // output — the "completed delegation" signal for the caller.  Still
    // useful alongside streamed text: if the consumer wants to show the
    // final assistant message cleanly (no token-by-token reconstruction),
    // this event delivers the full assembled body once per turn.
    orch->set_progress_callback(
        [&emit, orch_ptr](const std::string& id, const std::string& content) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(orch_ptr->current_stream_id()));
            m["depth"]     = jnum(static_cast<double>(orch_ptr->current_stream_depth()));
            m["content"]   = jstr(content);
            emit("sub_agent_response", p);
        });
    // API mode has no interactive user and no TUI panes — deny any prompt
    // for confirmation and refuse /pane so the agent's tool-result block
    // tells it to adapt.
    orch->set_confirm_callback([](const std::string&) { return false; });
    orch->set_pane_spawner(
        [](const std::string&, const std::string&) -> std::string {
            return "ERR: /pane unavailable in API mode — use /agent for "
                   "synchronous delegation.";
        });

    // Kickoff event carries the preview of the caller's message back so
    // consumers can correlate streams in logs.  `request_id` is the handle
    // the client uses to call POST /v1/requests/:id/cancel.
    {
        auto p = jobj();
        auto& m = p->as_object_mut();
        m["request_id"] = jstr(request_id);
        m["agent"]      = jstr(agent_id);
        m["tenant_id"]  = jnum(static_cast<double>(tenant.id));
        m["tenant"]     = jstr(tenant.name);
        m["message"]    = jstr(message.size() > 200
                               ? message.substr(0, 200) + "…"
                               : message);
        emit("request_received", p);
    }

    // Pre-flight quota check.  Asks the billing service
    // whether this tenant has the budget to run the upcoming turn.  We
    // only know the master agent's model up front (delegations may pick
    // different models mid-stream), so the estimate is approximate;
    // the billing service's per-turn `usage/record` callback below settles
    // the actual cost.
    //
    // Skipped entirely when the billing service is not configured — the
    // runtime then becomes a thin pass-through to the operator-supplied
    // provider keys with no cap enforcement, per the documented escape
    // hatch in `ApiServerOptions::billing_url`.
    if (billing && billing->enabled() && !workspace_id.empty()) {
        // Best-effort model: prefer the inline agent_def's declared
        // model so quota_check prices against the right rate card; fall
        // back to a representative default when no agent_def is present
        // (e.g. resolved-by-id catalog agent — the billing service will treat
        // an unknown model as priced-at-zero, which is acceptable for
        // the budget *check* though not for `usage/record`).
        const std::string preflight_model =
            parsed_cfg ? parsed_cfg->model : std::string("claude-sonnet-4-6");

        // Conservative input estimate: ~3 chars/token rounds the count
        // up vs the 4-chars/token typical, so we err on the side of
        // declining a request that would be on the edge.  Output budget
        // is a fixed 4096-token cap; the agent rarely exceeds that and
        // overshooting only matters at the tenant's cap edge.
        const int est_in  = static_cast<int>(message.size() / 3);
        const int est_out = 4096;

        auto qr = billing->check_quota(workspace_id, preflight_model,
                                              est_in, est_out, request_id);
        if (qr.ok && !qr.allow) {
            auto e = jobj();
            auto& em = e->as_object_mut();
            em["message"] = jstr(qr.message.empty()
                                 ? std::string("request denied by billing service")
                                 : qr.message);
            if (!qr.reason.empty()) em["reason"] = jstr(qr.reason);
            em["estimated_cost_micro_cents"] =
                jnum(static_cast<double>(qr.estimated_cost_uc));
            if (qr.plan_remaining_uc >= 0)
                em["plan_remaining_micro_cents"] =
                    jnum(static_cast<double>(qr.plan_remaining_uc));
            em["credit_balance_micro_cents"] =
                jnum(static_cast<double>(qr.credit_balance_uc));
            if (qr.total_budget_uc >= 0)
                em["total_budget_micro_cents"] =
                    jnum(static_cast<double>(qr.total_budget_uc));
            emit("error", e);
            sse.close();
            return;
        }
        // Transport errors (qr.ok=false) fall through — fail open so a
        // billing-service blip doesn't take the runtime offline.  An
        // operator alert on the billing service availability is the right
        // place to act on this.
    }

    // The master agent's own streamed text arrives with /cmd lines
    // (/agent, /fetch, /write blocks, etc.) embedded in the prose.  Run
    // it through the same StreamFilter the TUI uses so clients get
    // clean narrative text — tool invocations still surface as separate
    // structured events via the callbacks registered above.
    //
    // Capture the master's (agent, stream_id) via a shared_ptr populated
    // by stream_start_cb at depth 0.  Reading from the orchestrator's
    // thread-local getters would work during the turn but return defaults
    // during filter.flush() at end-of-request — StreamScope unwinds before
    // we drain the final buffered line.
    auto master_agent = std::make_shared<std::string>(agent_id);
    auto master_sid   = std::make_shared<int>(0);
    orch->set_stream_start_callback(
        [&emit, master_sid, master_agent](const std::string& id, int sid, int depth) {
            if (depth == 0) {
                *master_sid   = sid;
                *master_agent = id;
            }
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["depth"]     = jnum(static_cast<double>(depth));
            emit("stream_start", p);
        });

    Config cfg;   // cfg.verbose defaults to false, which is what we want
    StreamFilter filter(cfg,
        [&emit, master_sid, master_agent](const std::string& chunk) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(*master_agent);
            m["stream_id"] = jnum(static_cast<double>(*master_sid));
            m["depth"]     = jnum(0);
            m["delta"]     = jstr(chunk);
            emit("text", p);
        });

    // Wired here (not further up) so the handler can drain the master's
    // line buffer before stream_end lands on the wire.  Non-master streams
    // don't use this filter; flushing it for them is a no-op.
    orch->set_stream_end_callback(
        [&emit, &filter, master_sid](const std::string& id, int sid, bool ok) {
            if (sid == *master_sid) filter.flush();
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["ok"]        = jbool(ok);
            emit("stream_end", p);
        });

    // Advisor gate halt — sibling of stream_end so SSE clients can show
    // the halt reason out-of-band from the agent's normal text deltas.
    // Fires before stream_end (which arrives with ok=false).
    orch->set_escalation_callback(
        [&emit](const std::string& id, int sid, const std::string& reason) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["reason"]    = jstr(reason);
            emit("escalation", p);
        });

    // Advisor activity — every consult and gate decision flows through
    // here.  Verbose logger renders these to stderr; SSE clients can
    // surface gate reasoning in their UI.  Distinct from `escalation`,
    // which fires only on terminal HALTs.
    orch->set_advisor_event_callback(
        [&emit](const Orchestrator::AdvisorEvent& ev) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(ev.agent_id);
            m["stream_id"] = jnum(static_cast<double>(ev.stream_id));
            m["kind"]      = jstr(ev.kind);
            if (!ev.detail.empty())  m["detail"]  = jstr(ev.detail);
            if (!ev.preview.empty()) m["preview"] = jstr(ev.preview);
            if (ev.malformed)        m["malformed"] = jbool(true);
            emit("advisor", p);
        });

    try {
        auto resp = orch->send_streaming(agent_id, std::move(message_parts),
            [&filter](const std::string& chunk) { filter.feed(chunk); });
        filter.flush();

        auto done = jobj();
        auto& m = done->as_object_mut();
        m["ok"]      = jbool(resp.ok);
        if (!resp.ok) {
            // Never proxy the provider's free-form error message —
            // log it operator-side, ship a fixed taxonomy on the wire.
            const char* code = sanitised_provider_error_code(resp.error_type);
            m["error"]      = jstr(sanitised_provider_error_message(code));
            m["error_code"] = jstr(code);
            std::fprintf(stderr,
                "[arbiter] tenant=%lld request=%s upstream error: type=%s message=%s\n",
                static_cast<long long>(tenant.id), request_id.c_str(),
                resp.error_type.c_str(), resp.error.c_str());
        }
        m["content"] = jstr(resp.content);
        m["input_tokens"]  = jnum(static_cast<double>(resp.input_tokens));
        m["output_tokens"] = jnum(static_cast<double>(resp.output_tokens));
        m["files_bytes"]   = jnum(static_cast<double>(bytes_captured.load()));

        // No local cost figure on the runtime side — the billing service's
        // ledger is authoritative for the billed amount.  Consumers
        // wanting a request-level total query the billing service directly.
        m["tenant_id"]   = jnum(static_cast<double>(tenant.id));
        m["request_id"]  = jstr(request_id);
        if (conversation_id > 0)
            m["conversation_id"] = jnum(static_cast<double>(conversation_id));

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        m["duration_ms"] = jnum(static_cast<double>(elapsed_ms));
        metric_scope.ok  = resp.ok;
        emit("done", done);

        // Flush any pending coalesced text and stamp the run terminal.
        // close() is idempotent; re-entry from the catch-all below
        // would be a no-op.
        sse.close();
        if (request_status_created) {
            const int64_t completed = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            tenants.update_request_status(request_id,
                std::optional<std::string>(resp.ok ? "completed" : "failed"),
                completed,
                resp.ok ? std::nullopt : std::optional<std::string>(resp.error),
                std::nullopt);
        }

        // Persist the assistant turn to the conversation thread.  resp's
        // content + token counts are cumulative across all tool-call
        // re-entry iterations (Orchestrator::send_streaming aggregates
        // them before returning), so what we persist is the full
        // multi-turn assistant response — not just the closing remark.
        if (conversation_id > 0 && resp.ok) {
            try {
                tenants.append_message(tenant.id, conversation_id,
                                        "assistant", resp.content,
                                        resp.input_tokens, resp.output_tokens,
                                        request_id);
            } catch (...) {
                // Best-effort persistence; emit but don't fail the stream.
                log_error("assistant message could not be persisted to "
                          "conversation");
            }
        }
    } catch (const std::exception& e) {
        log_error(std::string("orchestration failed: ") + e.what());
        if (request_status_created) {
            const int64_t completed = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            tenants.update_request_status(request_id,
                std::optional<std::string>("failed"),
                completed,
                std::optional<std::string>(e.what()),
                std::nullopt);
        }
    }

    sse.close();
}

} // namespace

// Public wrapper exposing the (anon-namespace) builder so other TUs —
// notably the background Scheduler — can construct an Orchestrator with
// the same wiring used by A2A's synchronous message/send path.
std::unique_ptr<Orchestrator>
build_blocking_orchestrator(const ApiServerOptions& opts,
                             TenantStore& tenants,
                             const Tenant& tenant,
                             std::string& err_out) {
    return build_a2a_orchestrator(opts, tenants, tenant, err_out);
}

// ─── ApiServer public API ───────────────────────────────────────────────────

ApiServer::ApiServer(ApiServerOptions opts, TenantStore& tenants)
    : opts_(std::move(opts)), tenants_(tenants) {
    if (!opts_.billing_url.empty()) {
        billing_ = std::make_unique<BillingClient>(
            opts_.billing_url);
    }
    notifications_   = std::make_unique<NotificationBus>();
    request_events_  = std::make_unique<RequestEventBus>();
    scheduler_       = std::make_unique<Scheduler>(
        &opts_, &tenants_, notifications_.get());
    // Per-tenant limiter: defaults from env (ARBITER_TENANT_MAX_CONCURRENT
    // / RATE_PER_MIN / BURST).  Zeroed defaults ⇒ unlimited; the limiter
    // grants every acquire without taking the lock.
    limiter_ = std::make_unique<TenantLimiter>(load_tenant_limits_from_env());

    // Idempotency cache for retry-safe POSTs.  Always present; an
    // absent Idempotency-Key header on a request bypasses it entirely.
    idempotency_  = std::make_unique<IdempotencyCache>();
    opts_.idempotency = idempotency_.get();

    // Metrics registry; rendered at GET /v1/metrics.
    metrics_      = std::make_unique<Metrics>();
    opts_.metrics = metrics_.get();

    // Circuit breaker shared across all per-request ApiClients.
    // Defaults (5 consecutive failures → open, 30s cooldown) are
    // tuned to tolerate transient hiccups while catching sustained
    // provider outages.  Operator-tunable env vars are a Phase 5
    // follow-up; the defaults serve the v1 target deployments well.
    circuit_breaker_ = std::make_unique<ProviderCircuitBreaker>();
    circuit_breaker_->set_metrics(metrics_.get());
    opts_.circuit_breaker = circuit_breaker_.get();

    // Per-tenant /exec sandbox.  When configured but unusable (docker
    // missing, no image, etc.) we log the reason and continue with
    // /exec disabled — the alternative (refusing to start the server)
    // is too sharp a knife for an opt-in feature.
    if (opts_.sandbox_enabled) {
        SandboxConfig sc;
        sc.runtime              = opts_.sandbox_runtime;
        sc.image                = opts_.sandbox_image;
        sc.workspaces_root      = opts_.sandbox_workspaces_root;
        sc.network              = opts_.sandbox_network;
        sc.memory_mb            = opts_.sandbox_memory_mb;
        sc.cpus                 = opts_.sandbox_cpus;
        sc.pids_limit           = opts_.sandbox_pids_limit;
        sc.exec_timeout_seconds = opts_.sandbox_exec_timeout_seconds;
        sc.workspace_max_bytes  = opts_.sandbox_workspace_max_bytes;
        sc.idle_seconds         = opts_.sandbox_idle_seconds;
        // Always keep the manager, even on usability failure — cli.cpp
        // queries it post-banner-clear to render the startup status
        // line (the ctor's stderr log is wiped by `\033[2J`).  The
        // unusable path is cheap: SandboxConfig validation fails before
        // the reaper thread spawns, so a stored-but-unusable manager
        // costs nothing beyond the SandboxConfig snapshot.
        sandbox_ = std::make_unique<SandboxManager>(std::move(sc));
        if (sandbox_->usable()) {
            // Stash the non-owning pointer into opts so downstream
            // request handlers / orchestrator factories pick it up
            // without an extra parameter on every signature.
            opts_.sandbox = sandbox_.get();
            sandbox_->set_metrics(metrics_.get());
            char cpus_buf[16];
            std::snprintf(cpus_buf, sizeof(cpus_buf), "%.2f",
                          opts_.sandbox_cpus);
            Logger::global().info("sandbox_enabled", {
                {"image",   opts_.sandbox_image},
                {"network", opts_.sandbox_network},
                {"memory_mb", std::to_string(opts_.sandbox_memory_mb)},
                {"cpus",      cpus_buf},
                {"pids",      std::to_string(opts_.sandbox_pids_limit)},
                {"timeout_s", std::to_string(
                    opts_.sandbox_exec_timeout_seconds)},
            });
        } else {
            Logger::global().warn("sandbox_disabled",
                {{"reason", sandbox_->unusable_reason()}});
        }
    }

    // Recovery sweep: any request_status row left in 'running' from a
    // previous process must have been interrupted by a crash or kill.
    // Mark them failed and append a synthetic terminal event so a
    // reconnecting client (or a resumed UI tab) sees a clean done.
    try {
        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto orphaned = tenants_.recover_running_requests(
            "failed", now_s,
            "request was interrupted by a server restart; reconnect to retry");
        for (const auto& rid : orphaned) {
            // Synthesise a terminal `done` event so resubscribe finds
            // a clean tail.  Use the next seq (last_seq+1) for a
            // reconnecting client tracking the cursor.
            auto status = tenants_.get_request_status(/*tenant_id=*/0, rid);
            // get_request_status takes tenant_id, but we don't know
            // the tenant of an orphan without re-querying.  Quick
            // helper: pull the row directly.  list_request_status
            // doesn't filter on a single id; use the cross-tenant
            // approach: skip tenant filter here by reaching into
            // list_request_events.  Simpler: append at last_seq+1
            // using a single-purpose append that doesn't need the
            // tenant.  For v1 we omit the synthetic done — the
            // resubscribe handler's heartbeat-time poll catches the
            // status flip and emits the terminal frame on demand.
            (void)status; (void)rid;
        }
        if (!orphaned.empty()) {
            Logger::global().info("recovery_sweep", {
                {"orphaned_count", std::to_string(orphaned.size())},
                {"new_state",      "failed"},
            });
        }
    } catch (const std::exception& e) {
        Logger::global().error("recovery_sweep_failed",
            {{"error", e.what()}});
    }
}

ApiServer::~ApiServer() { stop(); }

// Install a SIGSEGV/SIGABRT/SIGBUS handler that prints a backtrace
// before re-raising the signal.  Once-only — multiple ApiServer
// instances in the same process share the handler.  Used to leave a
// forensic trail when the API server crashes mid-request: the kernel
// signal arrives, we print frames to stderr, then re-raise so the
// default action runs (core dump or exit).
//
// Uses backtrace(3) which is async-signal-unsafe in the strict sense
// but works in practice on darwin and glibc-Linux for crashes that
// don't corrupt malloc state — that's exactly the case we want
// breadcrumbs for.  If a future crash hangs in the handler we'll
// switch to a pre-allocated buffer.

namespace {

void crash_handler(int sig) {
    constexpr int kFrames = 32;
    void* buf[kFrames];
    int n = ::backtrace(buf, kFrames);
    const char* sig_name =
        sig == SIGSEGV ? "SIGSEGV" :
        sig == SIGABRT ? "SIGABRT" :
        sig == SIGBUS  ? "SIGBUS"  :
        sig == SIGFPE  ? "SIGFPE"  : "signal";
    // write(2) is async-signal-safe; fprintf is not, but in practice
    // it works for the cases we care about (the handler is best-effort
    // forensic, not a guarantee).
    std::fprintf(stderr, "\n=== arbiter crashed (%s, sig %d) — backtrace ===\n",
                 sig_name, sig);
    ::backtrace_symbols_fd(buf, n, fileno(stderr));
    std::fprintf(stderr, "=== end backtrace ===\n");
    std::fflush(stderr);
    // Restore default disposition and re-raise so the OS records the
    // crash properly (core file, parent's wait status).
    std::signal(sig, SIG_DFL);
    ::raise(sig);
}

void install_crash_handlers_once() {
    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true)) return;
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGBUS,  crash_handler);
    std::signal(SIGFPE,  crash_handler);
    // Ignore SIGPIPE process-wide: any write to a peer that hung up
    // mid-response would otherwise terminate the server with the
    // default disposition.  macOS lacks MSG_NOSIGNAL on send(), so
    // write_all() can't suppress at the call site portably.  Ignoring
    // turns the failed write into a normal -1/EPIPE return that
    // write_all already handles by giving up on that connection.
    std::signal(SIGPIPE, SIG_IGN);
}

} // namespace

void ApiServer::start() {
    if (running_.load()) return;
    install_crash_handlers_once();

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("socket(): ") + std::strerror(errno));

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(opts_.port));
    if (opts_.bind == "0.0.0.0" || opts_.bind.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, opts_.bind.c_str(), &addr.sin_addr) != 1) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            throw std::runtime_error("invalid bind address: " + opts_.bind);
        }
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error(
            "bind() failed on " + opts_.bind + ":" +
            std::to_string(opts_.port) + ": " + err);
    }

    if (::listen(listen_fd_, 32) < 0) {
        std::string err = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error(std::string("listen(): ") + err);
    }

    // If opts_.port was 0 the kernel picked a free one; read it back so
    // callers (tests, probes) can find us.
    socklen_t alen = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    bound_port_ = ntohs(addr.sin_port);

    running_ = true;
    accept_thread_ = std::thread(&ApiServer::accept_loop, this);

    if (scheduler_) scheduler_->start();
}

void ApiServer::stop() {
    if (!running_.exchange(false)) return;
    if (scheduler_) scheduler_->stop();
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    // Drain in-flight connections.  First, signal every orchestration
    // currently in the InFlightRegistry to cancel — that wakes the
    // streaming LLM read so the SSE handler can write its final frame
    // and unwind.  Then wait on the connection counter with a deadline.
    int drain_seconds = 30;
    if (const char* e = std::getenv("ARBITER_DRAIN_SECONDS"); e && *e) {
        try {
            int v = std::stoi(e);
            if (v >= 0) drain_seconds = v;
        } catch (...) { /* fall through to default */ }
    }

    {
        std::lock_guard<std::mutex> lk(in_flight_.mu);
        for (auto& kv : in_flight_.by_id) {
            if (kv.second.orch) {
                try { kv.second.orch->cancel(); } catch (...) {}
            }
        }
    }

    const int initial = active_connections_.load(std::memory_order_acquire);
    if (initial > 0) {
        Logger::global().info("drain_started", {
            {"in_flight",   std::to_string(initial)},
            {"deadline_s",  std::to_string(drain_seconds)},
        });
    }
    if (drain_seconds > 0 && initial > 0) {
        std::unique_lock<std::mutex> lk(drain_mu_);
        drain_cv_.wait_for(lk, std::chrono::seconds(drain_seconds),
            [this]{ return active_connections_.load(
                        std::memory_order_acquire) == 0; });
    }
    const int leftover = active_connections_.load(std::memory_order_acquire);
    if (leftover > 0) {
        Logger::global().warn("drain_timeout",
            {{"leftover", std::to_string(leftover)}});
    } else if (initial > 0) {
        Logger::global().info("drain_complete");
    }

    // Sandbox containers outlive a single connection but should go down
    // with the server.  Workspace bytes remain on disk — only the
    // containers are torn down.
    if (sandbox_) sandbox_->stop_all();
}

void ApiServer::accept_loop() {
    while (running_.load()) {
        int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            if (!running_.load()) return;
            continue;
        }
        int flag = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        // Bump the in-flight counter before detaching so a shutdown
        // racing the spawn always sees the worker.  Decrement + notify
        // happens in the worker's exit path.
        active_connections_.fetch_add(1, std::memory_order_acq_rel);
        // Each connection gets its own thread.  Detached because this
        // server does not retain per-connection handles; the connection
        // thread owns cleanup.
        std::thread([this, client]() {
            try { handle_connection(client); }
            catch (...) { /* drop — client socket will be closed below */ }
            ::close(client);
            if (active_connections_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lk(drain_mu_);
                drain_cv_.notify_all();
            }
        }).detach();
    }
}

void ApiServer::handle_connection(int fd) {
    HttpRequest req;
    if (!parse_http_request(fd, req)) {
        write_plain_response(fd, 400, "Bad Request", "bad request\n");
        return;
    }
    // Connection-level exception trap.  Without this, an uncaught throw
    // anywhere downstream propagates out of the connection thread and
    // calls std::terminate, killing the whole API server process —
    // which most users perceive as a "segfault".  The try/catch keeps
    // the daemon up and tells us exactly what threw + on which route.
    auto log_uncaught = [&](const char* what) {
        std::time_t now = std::time(nullptr);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&now));
        std::fprintf(stderr,
            "[%s] [api] UNCAUGHT EXCEPTION in %s %s: %s\n",
            ts, req.method.c_str(), req.path.c_str(), what);
        std::fflush(stderr);
    };
    try {

    // CORS preflight short-circuits before auth — browsers fire OPTIONS
    // without the Authorization header by design.  Any path answers the
    // same way; the subsequent real request re-validates.
    if (req.method == "OPTIONS") {
        write_preflight_response(fd);
        return;
    }

    // Prometheus-style metrics scrape — no auth, intended to live
    // behind the same reverse proxy that gates the tenant routes.
    // Operators wanting tighter access should restrict /v1/metrics
    // at the proxy or via a network policy; arbiter itself doesn't
    // gate it so the typical "in-cluster Prometheus" setup works
    // out of the box.
    if (req.method == "GET" && req.path == "/v1/metrics") {
        std::string body = metrics_ ? metrics_->render() : std::string{};
        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n" +
            kCorsHeaders +
            "Connection: close\r\n\r\n";
        write_all(fd, headers);
        write_all(fd, body);
        return;
    }

    // Health check — no auth, tiny response, useful for liveness probes.
    if (req.method == "GET" && req.path == "/v1/health") {
        write_plain_response(fd, 200, "OK", "ok\n");
        return;
    }

    // A2A well-known discovery — unauth, returns a top-level stub
    // describing how to authenticate for per-agent cards.  Sits ahead of
    // the bearer check so spec-compliant clients can dial in cold.
    if (req.method == "GET" && req.path == "/.well-known/agent-card.json") {
        handle_a2a_well_known_card(fd, req, opts_);
        return;
    }

    // Admin routes have their own auth (admin token, not tenant tokens).
    // Matched by prefix so /v1/admin, /v1/admin/tenants, /v1/admin/usage?…
    // all funnel into handle_admin, which sub-dispatches.
    if (req.path.rfind("/v1/admin", 0) == 0) {
        handle_admin(fd, req, tenants_, in_flight_, opts_);
        return;
    }

    const std::string token = extract_bearer(req);
    std::optional<Tenant> tenant;
    if (!token.empty()) tenant = tenants_.find_by_token(token);
    if (!tenant) {
        write_plain_response(fd, 401, "Unauthorized",
                             "missing or invalid bearer token\n");
        return;
    }

    // the billing service gate.  When billing is configured, every authenticated
    // request goes through /v1/runtime/auth/validate so a back-office
    // suspension or revocation lands within the cached TTL window.  A
    // transport-error to the billing service fails open — we'd rather bill
    // imperfectly than brick the runtime on a single-service outage.
    std::string workspace_id;
    if (billing_ && billing_->enabled()) {
        auto av = billing_->validate(token);
        if (av.ok) {
            workspace_id = av.workspace_id;
        } else if (av.http_status == 401) {
            write_plain_response(fd, 401, "Unauthorized",
                                 "billing service rejected token\n");
            return;
        } else if (av.http_status == 403) {
            write_plain_response(fd, 403, "Forbidden",
                                 av.message.empty()
                                     ? "tenant not active\n"
                                     : (av.message + "\n"));
            return;
        }
        // Anything else (transport_error, 5xx, malformed) falls through —
        // workspace_id stays empty, so downstream quota_check sees an
        // unknown workspace and the BillingClient's own
        // fail-open path keeps the request flowing.
    }

    if (req.method == "POST" && req.path == "/v1/orchestrate") {
        auto lim = limiter_->acquire(tenant->id);
        if (!lim.granted()) {
            write_429_response(fd, lim.retry_after_seconds,
                lim.kind == TenantLimiter::Result::Kind::ConcurrentExceeded
                    ? "concurrent_request_limit"
                    : "rate_limit",
                metrics_.get(), tenant->id);
            return;
        }
        // lim.guard releases the in-flight slot when this scope exits.
        handle_orchestrate(fd, req, opts_, tenants_, in_flight_,
                           billing_.get(), workspace_id, *tenant,
                           request_events_.get());
        return;
    }

    // Model catalogue — powers the frontend's model picker.
    if (req.method == "GET" && req.path == "/v1/models") {
        handle_models_list(fd);
        return;
    }

    // Cancel an in-flight /v1/orchestrate request by its request_id.
    if (req.method == "POST" &&
        req.path.rfind("/v1/requests/", 0) == 0 &&
        req.path.find("/cancel") != std::string::npos) {
        handle_cancel(fd, req, in_flight_, *tenant);
        return;
    }

    // ── Request log + resubscribe ────────────────────────────────────
    // GET /v1/requests                       — list recent runs
    // GET /v1/requests/:id                   — fetch one run's status
    // GET /v1/requests/:id/events?since_seq= — replay + live tail (SSE)
    {
        const auto rsegs = split_path(req.path);
        if (rsegs.size() >= 2 && rsegs[0] == "v1" && rsegs[1] == "requests") {
            if (rsegs.size() == 2) {
                if (req.method == "GET")
                    return handle_request_list(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (rsegs.size() == 3) {
                if (req.method == "GET")
                    return handle_request_get(fd, rsegs[2], tenants_, *tenant);
                // POST → cancel handled above; everything else 405.
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (rsegs.size() == 4 && rsegs[3] == "events") {
                if (req.method != "GET") {
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                handle_request_events(fd, rsegs[2], req, tenants_, *tenant,
                                       request_events_.get());
                return;
            }
            // Fall through to the cancel route handled above.
        }
    }

    // ── Conversations CRUD ───────────────────────────────────────────────
    {
        const auto segs = split_path(req.path);
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "conversations") {
            // POST /v1/conversations               — create
            // GET  /v1/conversations               — list
            if (segs.size() == 2) {
                if (req.method == "POST")
                    return handle_conversation_create(fd, req, tenants_, *tenant);
                if (req.method == "GET")
                    return handle_conversation_list(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            // /v1/conversations/:id and /v1/conversations/:id/messages
            int64_t id = 0;
            try { id = std::stoll(segs[2]); } catch (...) { id = 0; }
            if (id <= 0) {
                write_plain_response(fd, 400, "Bad Request", "bad conversation id\n");
                return;
            }
            if (segs.size() == 3) {
                if (req.method == "GET")
                    return handle_conversation_get(fd, id, tenants_, *tenant);
                if (req.method == "PATCH")
                    return handle_conversation_patch(fd, id, req, tenants_, *tenant);
                if (req.method == "DELETE")
                    return handle_conversation_delete(fd, id, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (segs.size() == 4 && segs[3] == "messages") {
                if (req.method == "GET")
                    return handle_conversation_messages(fd, id, req, tenants_, *tenant);
                // POST routes through handle_orchestrate with conversation_id
                // set — same SSE pipeline + billing, but the agent's
                // history is hydrated from prior messages and the
                // user/assistant pair is persisted around the call.
                if (req.method == "POST") {
                    auto conv = tenants_.get_conversation(tenant->id, id);
                    if (!conv) {
                        auto err = jobj();
                        err->as_object_mut()["error"] = jstr("conversation not found");
                        write_json_response(fd, 404, err);
                        return;
                    }
                    auto lim = limiter_->acquire(tenant->id);
                    if (!lim.granted()) {
                        write_429_response(fd, lim.retry_after_seconds,
                            lim.kind == TenantLimiter::Result::Kind::ConcurrentExceeded
                                ? "concurrent_request_limit"
                                : "rate_limit",
                            metrics_.get(), tenant->id);
                        return;
                    }
                    handle_orchestrate(fd, req, opts_, tenants_, in_flight_,
                                        billing_.get(), workspace_id,
                                        *tenant,
                                        request_events_.get(),
                                        /*agent_override=*/conv->agent_id,
                                        /*conversation_id=*/id,
                                        /*conversation_agent_def_json=*/conv->agent_def_json);
                    return;
                }
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            // ── /v1/conversations/:id/artifacts[/:aid][/raw] ───────────
            if (segs.size() >= 4 && segs[3] == "artifacts") {
                if (segs.size() == 4) {
                    if (req.method == "POST")
                        return handle_artifact_create(fd, id, req, tenants_, *tenant);
                    if (req.method == "GET")
                        return handle_artifact_list_conversation(fd, id, tenants_, *tenant);
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                if (segs.size() >= 5) {
                    int64_t aid = 0;
                    try { aid = std::stoll(segs[4]); } catch (...) { aid = 0; }
                    if (aid <= 0) {
                        write_plain_response(fd, 400, "Bad Request",
                                              "bad artifact id\n");
                        return;
                    }
                    // /raw returns the blob with proper Content-Type +
                    // ETag; the bare id returns metadata JSON.
                    if (segs.size() == 6 && segs[5] == "raw") {
                        if (req.method != "GET") {
                            write_plain_response(fd, 405, "Method Not Allowed",
                                                  "method not allowed\n");
                            return;
                        }
                        return handle_artifact_get_raw(fd, aid, req, tenants_, *tenant);
                    }
                    if (segs.size() == 5) {
                        if (req.method == "GET")
                            return handle_artifact_get_meta(fd, aid, tenants_, *tenant);
                        if (req.method == "DELETE")
                            return handle_artifact_delete(fd, aid, tenants_, *tenant);
                        write_plain_response(fd, 405, "Method Not Allowed",
                                              "method not allowed\n");
                        return;
                    }
                }
                write_plain_response(fd, 404, "Not Found",
                                      "artifact route not found\n");
                return;
            }
            write_plain_response(fd, 404, "Not Found",
                                 "conversation route not found\n");
            return;
        }

        // ── /v1/artifacts (tenant-scoped, cross-conversation) ─────────
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "artifacts") {
            if (segs.size() == 2) {
                if (req.method == "GET")
                    return handle_artifact_list_tenant(fd, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                      "method not allowed\n");
                return;
            }
            if (segs.size() >= 3) {
                int64_t aid = 0;
                try { aid = std::stoll(segs[2]); } catch (...) { aid = 0; }
                if (aid <= 0) {
                    write_plain_response(fd, 400, "Bad Request",
                                          "bad artifact id\n");
                    return;
                }
                if (segs.size() == 4 && segs[3] == "raw") {
                    if (req.method != "GET") {
                        write_plain_response(fd, 405, "Method Not Allowed",
                                              "method not allowed\n");
                        return;
                    }
                    return handle_artifact_get_raw(fd, aid, req, tenants_, *tenant);
                }
                if (segs.size() == 3) {
                    if (req.method == "GET")
                        return handle_artifact_get_meta(fd, aid, tenants_, *tenant);
                    if (req.method == "DELETE")
                        return handle_artifact_delete(fd, aid, tenants_, *tenant);
                    write_plain_response(fd, 405, "Method Not Allowed",
                                          "method not allowed\n");
                    return;
                }
            }
            write_plain_response(fd, 404, "Not Found",
                                  "artifact route not found\n");
            return;
        }
    }

    // ── A2A protocol surface ─────────────────────────────────────────────
    // GET  /v1/a2a/agents/:id/agent-card.json   — per-agent card (PR-1)
    // POST /v1/a2a/agents/:id                   — JSON-RPC dispatch (PR-2..)
    {
        const auto segs = split_path(req.path);
        if (segs.size() >= 4 && segs[0] == "v1" && segs[1] == "a2a"
            && segs[2] == "agents") {
            const std::string agent_id = segs[3];
            if (agent_id.empty()) {
                write_plain_response(fd, 400, "Bad Request", "agent id missing\n");
                return;
            }
            if (segs.size() == 5 && segs[4] == "agent-card.json") {
                if (req.method != "GET") {
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                handle_a2a_agent_card_get(fd, agent_id, req, opts_,
                                           tenants_, *tenant);
                return;
            }
            // POST /v1/a2a/agents/:id → JSON-RPC dispatch.  Every method
            // for this URL funnels through handle_a2a_rpc which writes
            // exactly one JSON-RPC envelope (success or error) back.
            if (segs.size() == 4) {
                if (req.method != "POST") {
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                auto lim = limiter_->acquire(tenant->id);
                if (!lim.granted()) {
                    write_429_response(fd, lim.retry_after_seconds,
                        lim.kind == TenantLimiter::Result::Kind::ConcurrentExceeded
                            ? "concurrent_request_limit"
                            : "rate_limit",
                        metrics_.get(), tenant->id);
                    return;
                }
                handle_a2a_rpc(fd, agent_id, req, opts_, tenants_,
                                in_flight_, *tenant,
                                request_events_.get());
                return;
            }
            write_plain_response(fd, 404, "Not Found", "a2a route not found\n");
            return;
        }
    }

    // ── Agent catalog + direct chat ──────────────────────────────────────
    // GET    /v1/agents              — list this tenant's stored agents + index
    // POST   /v1/agents              — create a stored agent for this tenant
    // GET    /v1/agents/:id          — fetch one (index or stored)
    // PATCH  /v1/agents/:id          — replace a stored agent's blob
    // DELETE /v1/agents/:id          — remove a stored agent
    // POST   /v1/agents/:id/chat     — orchestrate against a stored agent (or index)
    {
        const auto segs = split_path(req.path);
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "agents") {
            if (segs.size() == 2) {
                if (req.method == "GET")
                    return handle_agents_list(fd, opts_, tenants_, *tenant);
                if (req.method == "POST")
                    return handle_agent_create(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (segs.size() == 3) {
                if (req.method == "GET")
                    return handle_agent_get(fd, segs[2], opts_, tenants_, *tenant);
                if (req.method == "PATCH")
                    return handle_agent_patch(fd, segs[2], req, tenants_, *tenant);
                if (req.method == "DELETE")
                    return handle_agent_delete(fd, segs[2], tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (req.method == "POST" && segs.size() == 4 && segs[3] == "chat") {
                auto lim = limiter_->acquire(tenant->id);
                if (!lim.granted()) {
                    write_429_response(fd, lim.retry_after_seconds,
                        lim.kind == TenantLimiter::Result::Kind::ConcurrentExceeded
                            ? "concurrent_request_limit"
                            : "rate_limit",
                        metrics_.get(), tenant->id);
                    return;
                }
                handle_orchestrate(fd, req, opts_, tenants_, in_flight_,
                                    billing_.get(), workspace_id,
                                    *tenant,
                                    request_events_.get(),
                                    segs[2]);
                return;
            }
            write_plain_response(fd, 404, "Not Found", "agents route not found\n");
            return;
        }

        // ── Memory ────────────────────────────────────────────────────────
        // Two parallel sub-systems share this URL space:
        //
        //   File scratchpads (legacy, read-only):
        //     GET /v1/memory                  — list this tenant's memory files
        //     GET /v1/memory/shared           — the shared scratchpad
        //     GET /v1/memory/:agent_id        — one agent's persistent memory
        //
        //   Structured graph storage (CRUD):
        //     /v1/memory/entries[/:id]
        //     /v1/memory/relations[/:id]
        //     /v1/memory/graph
        //
        // The reserved sub-resource segments must short-circuit before the
        // `:agent_id` fallthrough so a literal "entries" agent id never
        // shadows the structured surface.
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "memory") {
            // ── /v1/memory/entries ─────────────────────────────────────
            if (segs.size() >= 3 && segs[2] == "entries") {
                if (segs.size() == 3) {
                    if (req.method == "POST")
                        return handle_memory_entry_create(fd, req, opts_, tenants_, *tenant);
                    if (req.method == "GET")
                        return handle_memory_entry_list(fd, req, opts_, tenants_, *tenant);
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                if (segs.size() == 4) {
                    int64_t id = 0;
                    try { id = std::stoll(segs[3]); } catch (...) { id = 0; }
                    if (id <= 0) {
                        write_plain_response(fd, 400, "Bad Request", "bad entry id\n");
                        return;
                    }
                    if (req.method == "GET")
                        return handle_memory_entry_get(fd, id, tenants_, *tenant);
                    if (req.method == "PATCH")
                        return handle_memory_entry_patch(fd, id, req, tenants_, *tenant);
                    if (req.method == "DELETE")
                        return handle_memory_entry_delete(fd, id, tenants_, *tenant);
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                // /v1/memory/entries/:id/invalidate (POST) — soft-delete
                // with a temporal window.  See handle_memory_entry_invalidate.
                if (segs.size() == 5 && segs[4] == "invalidate") {
                    int64_t id = 0;
                    try { id = std::stoll(segs[3]); } catch (...) { id = 0; }
                    if (id <= 0) {
                        write_plain_response(fd, 400, "Bad Request", "bad entry id\n");
                        return;
                    }
                    if (req.method != "POST") {
                        write_plain_response(fd, 405, "Method Not Allowed",
                                             "method not allowed\n");
                        return;
                    }
                    return handle_memory_entry_invalidate(fd, id, req,
                                                           tenants_, *tenant);
                }
                write_plain_response(fd, 404, "Not Found", "memory route not found\n");
                return;
            }
            // ── /v1/memory/relations ───────────────────────────────────
            if (segs.size() >= 3 && segs[2] == "relations") {
                if (segs.size() == 3) {
                    if (req.method == "POST")
                        return handle_memory_relation_create(fd, req, tenants_, *tenant);
                    if (req.method == "GET")
                        return handle_memory_relation_list(fd, req, tenants_, *tenant);
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                if (segs.size() == 4) {
                    int64_t id = 0;
                    try { id = std::stoll(segs[3]); } catch (...) { id = 0; }
                    if (id <= 0) {
                        write_plain_response(fd, 400, "Bad Request", "bad relation id\n");
                        return;
                    }
                    if (req.method == "DELETE")
                        return handle_memory_relation_delete(fd, id, tenants_, *tenant);
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                write_plain_response(fd, 404, "Not Found", "memory route not found\n");
                return;
            }
            // ── /v1/memory/graph ───────────────────────────────────────
            if (segs.size() == 3 && segs[2] == "graph") {
                if (req.method != "GET") {
                    write_plain_response(fd, 405, "Method Not Allowed",
                                         "method not allowed\n");
                    return;
                }
                handle_memory_graph(fd, req, tenants_, *tenant);
                return;
            }

            // ── File scratchpads (read-only fallthrough) ───────────────
            if (req.method != "GET") {
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "memory endpoints are read-only\n");
                return;
            }
            if (segs.size() == 2) {
                handle_memory_list(fd, opts_, tenants_, *tenant);
                return;
            }
            if (segs.size() == 3) {
                handle_memory_read(fd, segs[2], tenants_, *tenant);
                return;
            }
            write_plain_response(fd, 404, "Not Found", "memory route not found\n");
            return;
        }

        // ── Lessons ───────────────────────────────────────────────────────
        // POST   /v1/lessons              — create
        // GET    /v1/lessons              — list (?agent_id=&q=&limit=)
        // GET    /v1/lessons/:id          — fetch one
        // PATCH  /v1/lessons/:id          — update signature/lesson_text
        // DELETE /v1/lessons/:id          — hard remove
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "lessons") {
            if (segs.size() == 2) {
                if (req.method == "POST")
                    return handle_lesson_create(fd, req, tenants_, *tenant);
                if (req.method == "GET")
                    return handle_lesson_list(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (segs.size() == 3) {
                int64_t id = 0;
                try { id = std::stoll(segs[2]); } catch (...) { id = 0; }
                if (id <= 0) {
                    write_plain_response(fd, 400, "Bad Request", "bad lesson id\n");
                    return;
                }
                if (req.method == "GET")
                    return handle_lesson_get(fd, id, tenants_, *tenant);
                if (req.method == "PATCH")
                    return handle_lesson_patch(fd, id, req, tenants_, *tenant);
                if (req.method == "DELETE")
                    return handle_lesson_delete(fd, id, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            write_plain_response(fd, 404, "Not Found", "lesson route not found\n");
            return;
        }

        // ── Todos ─────────────────────────────────────────────────────────
        // POST   /v1/todos                — create
        // GET    /v1/todos                — list (?conversation_id=&status=&agent_id=)
        // GET    /v1/todos/:id            — fetch one
        // PATCH  /v1/todos/:id            — update subject/description/status/position
        // DELETE /v1/todos/:id            — hard remove
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "todos") {
            if (segs.size() == 2) {
                if (req.method == "POST")
                    return handle_todo_create(fd, req, tenants_, *tenant);
                if (req.method == "GET")
                    return handle_todo_list(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (segs.size() == 3) {
                int64_t id = 0;
                try { id = std::stoll(segs[2]); } catch (...) { id = 0; }
                if (id <= 0) {
                    write_plain_response(fd, 400, "Bad Request", "bad todo id\n");
                    return;
                }
                if (req.method == "GET")
                    return handle_todo_get(fd, id, tenants_, *tenant);
                if (req.method == "PATCH")
                    return handle_todo_patch(fd, id, req, tenants_, *tenant);
                if (req.method == "DELETE")
                    return handle_todo_delete(fd, id, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            write_plain_response(fd, 404, "Not Found", "todo route not found\n");
            return;
        }

        // ── Schedules ─────────────────────────────────────────────────────
        // POST   /v1/schedules                — create
        // GET    /v1/schedules                — list (?status=active|paused|completed)
        // GET    /v1/schedules/:id            — fetch one
        // PATCH  /v1/schedules/:id            — update status
        // DELETE /v1/schedules/:id            — cancel + remove
        // GET    /v1/schedules/:id/runs       — run history for the schedule
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "schedules") {
            if (segs.size() == 2) {
                if (req.method == "POST")
                    return handle_schedule_create(fd, req, tenants_, *tenant);
                if (req.method == "GET")
                    return handle_schedule_list(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            int64_t id = 0;
            try { id = std::stoll(segs[2]); } catch (...) { id = 0; }
            if (id <= 0) {
                write_plain_response(fd, 400, "Bad Request", "bad schedule id\n");
                return;
            }
            if (segs.size() == 3) {
                if (req.method == "GET")
                    return handle_schedule_get(fd, id, tenants_, *tenant);
                if (req.method == "PATCH")
                    return handle_schedule_patch(fd, id, req, tenants_, *tenant);
                if (req.method == "DELETE")
                    return handle_schedule_delete(fd, id, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (segs.size() == 4 && segs[3] == "runs" && req.method == "GET")
                return handle_schedule_runs(fd, id, req, tenants_, *tenant);
            write_plain_response(fd, 404, "Not Found", "schedule route not found\n");
            return;
        }

        // ── Runs (cross-schedule, tenant-wide) ────────────────────────────
        // GET /v1/runs[?since=<epoch>&task_id=<id>]
        // GET /v1/runs/:id
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "runs") {
            if (segs.size() == 2) {
                if (req.method == "GET")
                    return handle_runs_list(fd, req, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (segs.size() == 3) {
                int64_t id = 0;
                try { id = std::stoll(segs[2]); } catch (...) { id = 0; }
                if (id <= 0) {
                    write_plain_response(fd, 400, "Bad Request", "bad run id\n");
                    return;
                }
                if (req.method == "GET")
                    return handle_run_get(fd, id, tenants_, *tenant);
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            write_plain_response(fd, 404, "Not Found", "run route not found\n");
            return;
        }

        // ── Notifications stream ─────────────────────────────────────────
        // GET /v1/notifications/stream  (long-lived SSE)
        if (segs.size() == 3 && segs[0] == "v1" &&
            segs[1] == "notifications" && segs[2] == "stream") {
            if (req.method != "GET") {
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "method not allowed\n");
                return;
            }
            if (!notifications_) {
                write_plain_response(fd, 503, "Service Unavailable",
                                     "notifications subsystem not initialized\n");
                return;
            }
            handle_notifications_stream(fd, *tenant, *notifications_);
            return;
        }
    }

    write_plain_response(fd, 404, "Not Found", "endpoint not found\n");
    } catch (const std::exception& e) {
        log_uncaught(e.what());
        write_plain_response(fd, 500, "Internal Server Error",
                              std::string("internal error: ") + e.what() + "\n");
    } catch (...) {
        log_uncaught("(non-std exception)");
        write_plain_response(fd, 500, "Internal Server Error", "internal error\n");
    }
}

} // namespace arbiter
