// index/src/api_server.cpp — see api_server.h
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
#include "mcp/manager.h"
#include "orchestrator.h"
#include "billing_client.h"
#include "tenant_store.h"
#include "tui/stream_filter.h"
#include "api_client.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
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

namespace index_ai {

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
    "Access-Control-Allow-Headers: Authorization, Content-Type, Accept\r\n"
    "Access-Control-Max-Age: 86400\r\n";

// ─── HTTP response writers (non-SSE) ────────────────────────────────────────

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
    }

    void emit(const std::string& event, std::shared_ptr<JsonValue> data) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        std::string payload = data ? json_serialize(*data) : "{}";
        std::string frame = "event: " + event + "\ndata: " + payload + "\n\n";
        write_all(fd_, frame);
    }

    void close() {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }

private:
    int        fd_;
    std::mutex mu_;
    bool       closed_ = false;
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
        (void)request_id_;    // retained for future structured logging;
        (void)tenant_name_;   // currently suppressed for demo readability.
    }

    // Emit one event.  `ev` is the SSE event name; `payload` mirrors the
    // JSON body about to be written to the wire.  The logger reads only
    // the fields it cares about; unknown shapes are tolerated.
    void log(const std::string& ev, const std::shared_ptr<JsonValue>& payload) {
        const bool always = (ev == "request_received" || ev == "done" ||
                             ev == "error");
        if (!always && !verbose_) return;

        // Events suppressed entirely in the demo-friendly verbose stream:
        //   stream_start  — pre-announces a turn before any real content;
        //                   the first text/tool line is a sufficient cue.
        //   agent_start   — already silent in the legacy logger; kept so.
        if (ev == "stream_start" || ev == "agent_start") {
            return;
        }

        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream line;

        if (ev == "request_received") {
            const std::string agent = payload ? payload->get_string("agent") : "";
            const std::string msg   = payload ? payload->get_string("message") : "";
            line << color(kBoldCyan) << "POST /orchestrate" << reset()
                 << "  agent=" << color_for_agent(agent)
                 << display_agent(agent) << reset()
                 << "  " << quote_short(msg, 100);
        } else if (ev == "done") {
            const bool ok    = payload && payload->get_bool("ok");
            const double dur = payload ? payload->get_number("duration_ms") : 0;
            const double in  = payload ? payload->get_number("input_tokens") : 0;
            const double out = payload ? payload->get_number("output_tokens") : 0;
            line << color(ok ? kBoldGreen : kBoldRed)
                 << (ok ? "DONE " : "FAIL ") << reset()
                 << static_cast<int64_t>(dur) << "ms"
                 << color(kDim)
                 << "  in=" << static_cast<int>(in)
                 << " out=" << static_cast<int>(out)
                 << reset();
            if (!ok && payload) {
                const std::string err = payload->get_string("error");
                if (!err.empty()) line << "  " << quote_short(err, 80);
            }
            // Final flush of any text/thinking buffers that didn't end on a newline.
            flush_all_locked(line);
        } else if (ev == "error") {
            const std::string m = payload ? payload->get_string("message") : "";
            line << color(kBoldRed) << "ERROR" << reset() << "  " << quote_short(m, 100);
        } else if (ev == "stream_end") {
            const bool ok = payload && payload->get_bool("ok");
            const std::string agent = payload ? payload->get_string("agent") : "";
            // Drain that stream's buffered text so the next line isn't a
            // mid-sentence stub.
            if (payload) flush_buffered_locked(static_cast<int>(payload->get_number("stream_id")), line);
            // Successful ends are quiet (a coloured success marker on its own
            // line is more noise than signal across many parallel streams);
            // failures still surface so an operator notices a stalled sub-agent.
            if (ok) return;
            line << color(kBoldRed) << "FAIL" << reset()
                 << " " << color_for_agent(agent)
                 << display_agent(agent) << reset()
                 << " " << color(kDim) << "stream ended without ok" << reset();
        } else if (ev == "text") {
            buffer_delta_locked(payload, /*kind=*/"text", line);
        } else if (ev == "thinking") {
            buffer_delta_locked(payload, /*kind=*/"thinking", line);
        } else if (ev == "tool_call") {
            const std::string tool  = payload ? payload->get_string("tool") : "";
            const std::string agent = payload ? payload->get_string("agent") : "";
            const bool ok = payload && payload->get_bool("ok");
            line << color_for_agent(agent) << display_agent(agent) << reset()
                 << "  " << color_for_tool(tool) << tool << reset()
                 << " " << (ok ? color(kGreen) : color(kBoldRed))
                 << (ok ? "ok" : "ERR") << reset();
        } else if (ev == "token_usage") {
            // Per-turn token tally, dimmed since it's a sidebar metric.
            const std::string agent = payload ? payload->get_string("agent") : "";
            const double in  = payload ? payload->get_number("input_tokens") : 0;
            const double out = payload ? payload->get_number("output_tokens") : 0;
            line << color_for_agent(agent) << display_agent(agent) << reset()
                 << "  " << color(kDim) << "tokens: "
                 << "in=" << static_cast<int>(in)
                 << " out=" << static_cast<int>(out)
                 << reset();
        } else if (ev == "sub_agent_response") {
            // Boundary marker when a /agent or parallel child returns.
            // Show the size only — the deltas already streamed the body,
            // so a content reprint would be noise.
            const std::string agent = payload ? payload->get_string("agent") : "";
            const std::string content = payload ? payload->get_string("content") : "";
            line << color_for_agent(agent) << display_agent(agent) << reset()
                 << "  " << color(kDim) << "returned "
                 << fmt_size(static_cast<int64_t>(content.size()))
                 << reset();
        } else if (ev == "file") {
            const std::string path  = payload ? payload->get_string("path") : "";
            const std::string agent = payload ? payload->get_string("agent") : "";
            const double size = payload ? payload->get_number("size") : 0;
            line << color_for_agent(agent) << display_agent(agent) << reset()
                 << "  " << color(kBoldMagenta) << "wrote " << path << reset()
                 << color(kDim) << " (" << fmt_size(static_cast<int64_t>(size)) << ")"
                 << reset();
        } else if (ev == "advisor") {
            // Runtime gate / /advise consultation activity.  Each decision
            // gets one stderr line so the operator can watch the gate
            // working — colour-coded by signal type so a redirect or halt
            // jumps out of a long stream.
            const std::string agent  = payload ? payload->get_string("agent") : "";
            const std::string kind   = payload ? payload->get_string("kind")  : "";
            const std::string detail = payload ? payload->get_string("detail") : "";
            const std::string preview = payload ? payload->get_string("preview") : "";
            const bool malformed = payload && payload->get_bool("malformed");

            const char* tag_color = kDim;
            std::string label = kind;
            if (kind == "consult")        { tag_color = kCyan;     label = "advise"; }
            else if (kind == "gate_continue") { tag_color = kGreen;    label = "gate ✓"; }
            else if (kind == "gate_redirect") { tag_color = kYellow;   label = "gate ↻"; }
            else if (kind == "gate_halt")     { tag_color = kBoldRed;  label = "gate ✗"; }
            else if (kind == "gate_budget")   { tag_color = kBoldRed;  label = "gate ⛔"; }

            line << color_for_agent(agent) << display_agent(agent) << reset()
                 << "  " << color(tag_color) << label << reset();
            if (malformed)
                line << " " << color(kDim) << "(malformed)" << reset();
            if (!detail.empty())
                line << "  " << quote_short(detail, 100);
            else if (!preview.empty())
                line << "  " << color(kDim) << "← " << quote_short(preview, 80) << reset();
        } else {
            // Unknown event — log the name only; useful while iterating.
            line << color(kDim) << ev << reset();
        }

        const std::string s = line.str();
        if (s.empty()) return;  // delta buffered, nothing to flush yet
        std::fputs(s.c_str(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

private:
    // ANSI colour codes — only emitted when stderr is a TTY.
    static constexpr const char* kReset        = "\033[0m";
    static constexpr const char* kDim          = "\033[2m";
    static constexpr const char* kRed          = "\033[31m";
    static constexpr const char* kGreen        = "\033[32m";
    static constexpr const char* kYellow       = "\033[33m";
    static constexpr const char* kCyan         = "\033[36m";
    static constexpr const char* kBoldRed      = "\033[1;31m";
    static constexpr const char* kBoldGreen    = "\033[1;32m";
    static constexpr const char* kBoldCyan     = "\033[1;36m";
    static constexpr const char* kBoldMagenta  = "\033[1;35m";
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

    // Per-stream rolling buffer for text / thinking deltas.  We flush a
    // line whenever we see '\n', and on stream_end / done, so that
    // sentence-by-sentence streaming reads naturally even when the model
    // emits one token at a time.  Map keyed by (stream_id << 1 | kind_bit).
    struct Buf {
        std::string kind;   // "text" or "thinking"
        std::string agent;
        int         depth = 0;
        std::string pending;
    };
    std::map<int, Buf> bufs_;

    static int buf_key(int sid, bool thinking) { return (sid << 1) | (thinking ? 1 : 0); }

    void buffer_delta_locked(const std::shared_ptr<JsonValue>& payload,
                             const std::string& kind,
                             std::ostringstream& line) {
        if (!payload) return;
        const int sid = static_cast<int>(payload->get_number("stream_id"));
        const std::string agent = payload->get_string("agent");
        const int depth = static_cast<int>(payload->get_number("depth"));
        const std::string delta = payload->get_string("delta");
        const bool thinking = (kind == "thinking");

        auto& b = bufs_[buf_key(sid, thinking)];
        if (b.kind.empty()) { b.kind = kind; b.agent = agent; b.depth = depth; }
        b.pending += delta;

        // Flush any complete lines.
        size_t nl;
        while ((nl = b.pending.find('\n')) != std::string::npos) {
            std::string chunk = b.pending.substr(0, nl);
            b.pending.erase(0, nl + 1);
            // Skip empty lines inside the buffer — they pile up otherwise.
            if (chunk.empty()) continue;
            emit_text_line_locked(b.agent, thinking, chunk, line);
        }
    }

    void flush_buffered_locked(int sid, std::ostringstream& line) {
        for (int kindbit = 0; kindbit < 2; ++kindbit) {
            auto it = bufs_.find(buf_key(sid, kindbit == 1));
            if (it == bufs_.end()) continue;
            auto& b = it->second;
            if (!b.pending.empty()) {
                emit_text_line_locked(b.agent, kindbit == 1, b.pending, line);
                b.pending.clear();
            }
            bufs_.erase(it);
        }
    }

    // Emit one text/thinking line in the demo format and reset `line` so
    // the caller can either chain another emit or fall through to log()'s
    // own stderr write with an empty line.
    void emit_text_line_locked(const std::string& agent, bool thinking,
                                const std::string& chunk,
                                std::ostringstream& line) {
        const std::string disp = display_agent(agent);
        // Thinking blocks are rare (only some models surface them) and
        // dim-grey so they read as side-channel reasoning, not output.
        if (thinking) {
            line << color(kDim) << "(" << disp << " thinking) "
                 << quote_short(chunk, 100) << reset();
        } else {
            line << color_for_agent(agent) << disp << reset()
                 << "  " << quote_short(chunk, 110);
        }
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();
    }

    void flush_all_locked(std::ostringstream& line) {
        // Drain any lingering deltas from streams that didn't get a
        // proper stream_end (defensive — e.g., on cancellation paths).
        std::vector<int> keys;
        keys.reserve(bufs_.size());
        for (auto& [k, _] : bufs_) keys.push_back(k);
        for (int k : keys) flush_buffered_locked(k >> 1, line);
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "arbiter/0.3.6");

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
        {"claude-opus-4-7",    "anthropic"},
        {"claude-opus-4-6",    "anthropic"},
        {"claude-opus-4-5",    "anthropic"},
        {"claude-sonnet-4-6",  "anthropic"},
        {"claude-sonnet-4-5",  "anthropic"},
        {"claude-haiku-4-5",   "anthropic"},
        // OpenAI
        {"openai/gpt-5.4",     "openai"},
        {"openai/gpt-4.1",     "openai"},
        {"openai/gpt-4o",      "openai"},
        {"openai/gpt-4o-mini", "openai"},
        {"openai/o4-mini",     "openai"},
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
    std::ostringstream prompt;
    prompt << "Rerank these search results by relevance to the query.\n\n"
           << "Query: \"" << query << "\"\n\n"
           << "Candidates:\n";
    for (auto& e : candidates) {
        prompt << "[id=" << e.id << "] " << e.title << "\n";
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

    auto e = tenants.create_entry(tenant.id, type, title, content, source,
                                    *tags, artifact_id, conversation_id);
    write_json_response(fd, 201, memory_entry_to_json_hydrated(e, tenants));
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

    auto entries = use_graduated
        ? tenants.search_entries_graduated(tenant.id, f)
        : tenants.list_entries(tenant.id, f);

    std::shared_ptr<JsonValue> rerank_meta;
    if (!rerank_model.empty() && !f.q.empty() && entries.size() > 1) {
        auto opts_keys = opts.api_keys;  // copy — captured by the lambda
        const std::string sys_prompt =
            "You are an advisor consulted by another AI agent.  Answer "
            "the question directly and concisely.  No preamble.  No "
            "pleasantries.  No restating the question.  No offers to "
            "help further — the executor will re-engage if it needs "
            "more.";
        auto build_advisor = [opts_keys, sys_prompt]
                             (const std::string& model) {
            return [opts_keys, model, sys_prompt]
                   (const std::string& prompt) -> std::string {
                // Per-request ApiClient — TLS handshake amortizes
                // inside .complete().  Building one per rerank is
                // fine; if this becomes a hot path we can move to a
                // shared client member on ApiServer with the same
                // thread-safe contract the Orchestrator's client
                // uses.
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
        };

        auto coarse_advisor = build_advisor(rerank_model);
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
            auto fine_advisor = build_advisor(rerank_fine_model);
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

    std::string message = body->get_string("message");
    if (message.empty()) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr("missing required field: 'message'");
        write_json_response(fd, 400, err);
        return;
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
    {
        const int64_t tid = tenant.id;
        TenantStore*  store = &tenants;
        orch->set_memory_scratchpad(
            [tid, store](const std::string& op,
                          const std::string& agent_id,
                          const std::string& args) -> std::string {
                // Per-agent ops use the calling agent's id; shared-* use
                // the empty scope key.  Output strings match the legacy
                // file-based responses so the model sees the same
                // [/mem write] OK: ... framing it always has.
                if (op == "read") {
                    return store->read_scratchpad(tid, agent_id);
                }
                if (op == "shared-read") {
                    return store->read_scratchpad(tid, std::string{});
                }
                if (op == "write") {
                    int64_t sz = store->append_scratchpad(tid, agent_id, args);
                    return "OK: memory written (" + std::to_string(sz) +
                           " bytes total in scratchpad)";
                }
                if (op == "shared-write") {
                    int64_t sz = store->append_scratchpad(tid, std::string{}, args);
                    return "OK (" + std::to_string(sz) + " bytes total)";
                }
                if (op == "clear") {
                    store->clear_scratchpad(tid, agent_id);
                    return "OK: memory cleared";
                }
                if (op == "shared-clear") {
                    store->clear_scratchpad(tid, std::string{});
                    return "OK";
                }
                return "ERR: unknown scratchpad op '" + op + "'";
            });
    }

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

    // File-write interceptor — captures content to the SSE stream instead
    // of persisting to the server's filesystem.  Enforces the per-response
    // size cap; once exceeded, further writes are rejected with an ERR
    // that tells the agent to stop trying (dedup cache ensures it won't
    // retry the identical /write).
    std::atomic<size_t> bytes_captured{0};
    const size_t cap = opts.file_max_bytes;
    auto write_interceptor =
        [&emit, &bytes_captured, cap, stamp](const std::string& path,
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

        return "OK: captured " + std::to_string(size) +
               " bytes for '" + path + "' (streamed to client, not persisted)";
    };
    orch->set_write_interceptor(write_interceptor);
    orch->set_exec_disabled(opts.exec_disabled);

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
                l << "- #" << e.id << "  [" << e.type << "]  " << e.title
                  << fmt_tags(e.tags_json);
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

                TenantStore::EntryFilter f;
                f.q               = q;
                f.conversation_id = reader_conversation_id;
                f.limit           = rerank ? kAgentRerankPool : 50;

                auto entries = (reader_conversation_id > 0)
                    ? tenants.search_entries_graduated(reader_tenant_id, f)
                    : tenants.list_entries(reader_tenant_id, f);

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
        });

    // Structured-memory writer.  Tenant-scoped (mirrors the reader); writes
    // land directly in the curated graph and are visible to subsequent
    // reads on the next turn.  /mem add entry requires a non-empty body
    // (passed in as `body`) — the dispatcher rejects empty bodies before
    // the request reaches us, so by the time we see it we can trust the
    // body is meaningful synthesised text.
    orch->set_structured_memory_writer(
        [&tenants, reader_tenant_id, reader_conversation_id]
        (const std::string& kind,
         const std::string& args,
         const std::string& body) -> std::string {
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

                // Pin the entry to the active conversation when one is
                // present.  Without that link, /mem add entry inside a
                // conversation produces tenant-wide entries that don't
                // bias the conversation-scoped /mem search ranking.
                auto e = tenants.create_entry(reader_tenant_id, type, title,
                                               content, /*source=*/"agent",
                                               /*tags_json=*/"[]",
                                               artifact_id,
                                               reader_conversation_id);
                std::ostringstream out;
                out << "OK: added entry #" << e.id << " [" << e.type << "] "
                    << e.title;
                if (artifact_id > 0) {
                    out << " (linked to artifact #" << artifact_id << ")";
                }
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
        });

    // ── MCP session manager ───────────────────────────────────────────
    // One Manager per request; subprocesses spawn lazily on first /mcp
    // reference and die when the orchestrator's `mcp_mgr` shared_ptr
    // falls out of scope (which happens at the end of this function or
    // when the InFlightScope unwinds on cancel).  The manager lives
    // beyond the orchestrator only via the invoker capture below — when
    // the lambda is destroyed, so is the manager.
    //
    // Registry-load failures are non-fatal at request time: we log and
    // proceed with an empty manager so /mcp returns a clean ERR rather
    // than failing the whole request.  Operators see the parse error in
    // the API server log.
    std::shared_ptr<mcp::Manager> mcp_mgr;
    if (!opts.mcp_servers_path.empty()) {
        try {
            auto specs = mcp::load_server_registry(opts.mcp_servers_path);
            mcp_mgr = std::make_shared<mcp::Manager>(std::move(specs));
        } catch (const std::exception& e) {
            log_error(std::string("MCP registry load failed: ") + e.what());
        }
    }
    if (!mcp_mgr) mcp_mgr = std::make_shared<mcp::Manager>(std::vector<mcp::ServerSpec>{});

    orch->set_mcp_invoker(
        [mcp_mgr](const std::string& kind, const std::string& args) -> std::string {
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
        });

    // ── Web search ────────────────────────────────────────────────────
    // /search <query> [top=N] dispatches against the configured provider.
    // Only "brave" is implemented in v1; an unrecognised provider returns
    // ERR rather than silently doing the wrong thing.  Captures the key
    // by value so a future request that reloads ApiServerOptions doesn't
    // race the in-flight lambda.
    {
        const std::string provider = opts.search_provider.empty()
                                        ? std::string("brave")
                                        : opts.search_provider;
        const std::string key      = opts.search_api_key;
        if (provider == "brave" && !key.empty()) {
            orch->set_search_invoker(
                [key](const std::string& query, int top_n) -> std::string {
                    return brave_search(query, key, top_n);
                });
        } else if (!provider.empty() && provider != "brave" && !key.empty()) {
            orch->set_search_invoker(
                [provider](const std::string&, int) -> std::string {
                    return "ERR: search provider '" + provider +
                           "' is configured but not implemented in this "
                           "build.  Only 'brave' is supported in v1.";
                });
        }
        // No key ⇒ leave the invoker null; the dispatcher returns its own
        // ERR with a more useful message ("web search unavailable…").
    }

    // ── Artifact store bridges ────────────────────────────────────────
    // Wire /write --persist, /read, and /list against TenantStore +
    // the active conversation_id.  When the request didn't come in
    // through a conversation (e.g. raw /v1/orchestrate without a thread),
    // the writer/reader/lister stay null — the agent's slash dispatchers
    // surface a clear "no conversation context" warning + ephemeral
    // fallback for /write --persist.
    if (conversation_id > 0) {
        const int64_t tid    = tenant.id;
        const int64_t cid    = conversation_id;
        TenantStore*  store  = &tenants;

        orch->set_artifact_writer(
            [tid, cid, store](const std::string& raw_path,
                                const std::string& content) -> std::string {
                std::string err;
                auto canonical = sanitize_artifact_path(raw_path, err);
                if (!canonical) {
                    return std::string("ERR: invalid path: ") + err;
                }
                // mime_type stays default ('application/octet-stream') —
                // the agent doesn't know what to declare and we don't
                // sniff in v1.  HTTP callers can set it explicitly via
                // the POST endpoint.
                auto put = store->put_artifact(tid, cid, *canonical,
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
            });

        orch->set_artifact_reader(
            [tid, cid, store](const std::string& raw_path,
                                int64_t artifact_id,
                                int64_t via_memory_id) -> std::string {
                // Path-form: same-conversation lookup.  Sanitiser gates
                // bad paths before we touch the DB.
                if (artifact_id == 0) {
                    std::string err;
                    auto canonical = sanitize_artifact_path(raw_path, err);
                    if (!canonical) return std::string("ERR: invalid path: ") + err;

                    auto meta = store->get_artifact_meta_by_path(tid, cid, *canonical);
                    if (!meta) {
                        return std::string("ERR: '") + *canonical +
                               "' not found in this conversation's artifacts";
                    }
                    auto blob = store->get_artifact_content(tid, meta->id);
                    if (!blob) {
                        return std::string("ERR: artifact #") +
                               std::to_string(meta->id) + " content missing";
                    }
                    return *blob;
                }

                // Id-form: tenant-scoped lookup.  Cross-conversation
                // reads require a `via=mem:<id>` capability that points
                // at this artifact_id from a memory entry the tenant
                // owns.  Same-conversation reads are allowed without
                // citation — same trust boundary as path-form.
                auto art = store->get_artifact_meta(tid, artifact_id);
                if (!art) {
                    return std::string("ERR: artifact #") +
                           std::to_string(artifact_id) +
                           " not found for this tenant";
                }
                if (art->conversation_id != cid) {
                    if (via_memory_id == 0) {
                        return std::string("ERR: artifact #") +
                               std::to_string(artifact_id) +
                               " is in a different conversation; cite the "
                               "memory entry that links it: "
                               "/read #" + std::to_string(artifact_id) +
                               " via=mem:<entry_id>";
                    }
                    auto mem = store->get_entry(tid, via_memory_id);
                    if (!mem) {
                        return std::string("ERR: via=mem:") +
                               std::to_string(via_memory_id) +
                               " — memory entry not found for this tenant";
                    }
                    if (mem->artifact_id != artifact_id) {
                        return std::string("ERR: memory entry #") +
                               std::to_string(via_memory_id) +
                               " does not reference artifact #" +
                               std::to_string(artifact_id) +
                               " (its artifact_id=" +
                               std::to_string(mem->artifact_id) + ")";
                    }
                }
                auto blob = store->get_artifact_content(tid, artifact_id);
                if (!blob) {
                    return std::string("ERR: artifact #") +
                           std::to_string(artifact_id) +
                           " content missing";
                }
                return *blob;
            });

        orch->set_artifact_lister(
            [tid, cid, store]() -> std::string {
                auto rows = store->list_artifacts_conversation(tid, cid, 200);
                if (rows.empty()) return std::string{};
                std::ostringstream out;
                for (auto& r : rows) {
                    out << r.path << "  (" << r.size
                        << " bytes, mime=" << r.mime_type
                        << ", id=" << r.id << ")\n";
                }
                return out.str();
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
        auto resp = orch->send_streaming(agent_id, message,
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
        emit("done", done);

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
    }

    sse.close();
}

} // namespace

// ─── ApiServer public API ───────────────────────────────────────────────────

ApiServer::ApiServer(ApiServerOptions opts, TenantStore& tenants)
    : opts_(std::move(opts)), tenants_(tenants) {
    if (!opts_.billing_url.empty()) {
        billing_ = std::make_unique<BillingClient>(
            opts_.billing_url);
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
}

void ApiServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    // In-flight connection threads are detached; they'll terminate when
    // their TCP read fails post-shutdown or their response completes.
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
        // Each connection gets its own thread.  Detached because this
        // server does not retain per-connection handles; the connection
        // thread owns cleanup.
        std::thread([this, client]() {
            try { handle_connection(client); }
            catch (...) { /* drop — client socket will be closed below */ }
            ::close(client);
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

    // Health check — no auth, tiny response, useful for liveness probes.
    if (req.method == "GET" && req.path == "/v1/health") {
        write_plain_response(fd, 200, "OK", "ok\n");
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
        handle_orchestrate(fd, req, opts_, tenants_, in_flight_,
                           billing_.get(), workspace_id, *tenant);
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
                    handle_orchestrate(fd, req, opts_, tenants_, in_flight_,
                                        billing_.get(), workspace_id,
                                        *tenant,
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
                handle_orchestrate(fd, req, opts_, tenants_, in_flight_,
                                    billing_.get(), workspace_id,
                                    *tenant, segs[2]);
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
                        return handle_memory_entry_create(fd, req, tenants_, *tenant);
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

} // namespace index_ai
