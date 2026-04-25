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
#include "cost_tracker.h"
#include "json.h"
#include "orchestrator.h"
#include "tenant_store.h"
#include "tui/stream_filter.h"
#include "api_client.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
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
        req.headers[std::move(name)] = value.substr(vstart);
    }

    // Body — Content-Length only.  Chunked / keep-alive / pipelining are
    // out of scope; the one caller of this API sends a simple POST.
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        size_t want = 0;
        try { want = static_cast<size_t>(std::stoul(it->second)); }
        catch (...) { return false; }
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

// ─── HTTP response writers (non-SSE) ────────────────────────────────────────

void write_plain_response(int fd, int code, const std::string& reason,
                          const std::string& body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << reason << "\r\n"
       << "Content-Type: text/plain; charset=utf-8\r\n"
       << "Content-Length: " << body.size() << "\r\n"
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
       << "Connection: close\r\n\r\n"
       << payload;
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
        static constexpr const char* kHdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "X-Accel-Buffering: no\r\n"
            "Connection: close\r\n\r\n";
        std::lock_guard<std::mutex> lk(mu_);
        write_all(fd_, kHdr, std::strlen(kHdr));
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

void emit_error(SseStream& sse, const std::string& msg) {
    auto o = jobj();
    o->as_object_mut()["message"] = jstr(msg);
    sse.emit("error", o);
}

// ─── Admin endpoints ────────────────────────────────────────────────────────
//
// All admin routes are JSON-in, JSON-out.  Field naming is stable: once
// documented for the sibling billing service, these shapes need to be
// versioned rather than renamed.  Monetary values are always in µ¢
// (micro-cents; 1 USD = 1,000,000 µ¢) on the wire — the sibling decides
// how to display them.

std::shared_ptr<JsonValue> tenant_to_json(const Tenant& t) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]                        = jnum(static_cast<double>(t.id));
    m["name"]                      = jstr(t.name);
    m["disabled"]                  = jbool(t.disabled);
    m["monthly_cap_micro_cents"]   = jnum(static_cast<double>(t.monthly_cap_uc));
    m["month_yyyymm"]              = jstr(t.month_yyyymm);
    m["month_to_date_micro_cents"] = jnum(static_cast<double>(t.month_to_date_uc));
    m["created_at"]                = jnum(static_cast<double>(t.created_at));
    m["last_used_at"]              = jnum(static_cast<double>(t.last_used_at));
    return o;
}

std::shared_ptr<JsonValue> usage_to_json(const UsageEntry& e) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"]                          = jnum(static_cast<double>(e.id));
    m["tenant_id"]                   = jnum(static_cast<double>(e.tenant_id));
    m["timestamp"]                   = jnum(static_cast<double>(e.timestamp));
    m["model"]                       = jstr(e.model);
    m["input_tokens"]                = jnum(static_cast<double>(e.input_tokens));
    m["output_tokens"]               = jnum(static_cast<double>(e.output_tokens));
    m["cache_read_tokens"]           = jnum(static_cast<double>(e.cache_read_tokens));
    m["cache_create_tokens"]         = jnum(static_cast<double>(e.cache_create_tokens));
    m["input_micro_cents"]           = jnum(static_cast<double>(e.input_uc));
    m["output_micro_cents"]          = jnum(static_cast<double>(e.output_uc));
    m["cache_read_micro_cents"]      = jnum(static_cast<double>(e.cache_read_uc));
    m["cache_create_micro_cents"]    = jnum(static_cast<double>(e.cache_create_uc));
    m["provider_micro_cents"]        = jnum(static_cast<double>(e.provider_uc));
    m["markup_micro_cents"]          = jnum(static_cast<double>(e.markup_uc));
    m["billed_micro_cents"]          = jnum(static_cast<double>(e.provider_uc + e.markup_uc));
    if (!e.request_id.empty()) m["request_id"] = jstr(e.request_id);
    return o;
}

std::shared_ptr<JsonValue> bucket_to_json(const TenantStore::UsageBucket& b) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["key"]                         = jstr(b.key);
    m["calls"]                       = jnum(static_cast<double>(b.calls));
    m["input_tokens"]                = jnum(static_cast<double>(b.input_tokens));
    m["output_tokens"]               = jnum(static_cast<double>(b.output_tokens));
    m["cache_read_tokens"]           = jnum(static_cast<double>(b.cache_read_tokens));
    m["cache_create_tokens"]         = jnum(static_cast<double>(b.cache_create_tokens));
    m["input_micro_cents"]           = jnum(static_cast<double>(b.input_uc));
    m["output_micro_cents"]          = jnum(static_cast<double>(b.output_uc));
    m["cache_read_micro_cents"]      = jnum(static_cast<double>(b.cache_read_uc));
    m["cache_create_micro_cents"]    = jnum(static_cast<double>(b.cache_create_uc));
    m["provider_micro_cents"]        = jnum(static_cast<double>(b.provider_uc));
    m["markup_micro_cents"]          = jnum(static_cast<double>(b.markup_uc));
    m["billed_micro_cents"]          = jnum(static_cast<double>(b.provider_uc + b.markup_uc));
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
                // Accept either cap_usd (float) or monthly_cap_micro_cents
                // (integer) so callers can choose their preferred unit.
                int64_t cap_uc = 0;
                if (auto v = body->get("monthly_cap_micro_cents"); v && v->is_number()) {
                    cap_uc = static_cast<int64_t>(v->as_number());
                } else if (auto v2 = body->get("cap_usd"); v2 && v2->is_number()) {
                    cap_uc = usd_to_uc(v2->as_number());
                }
                if (cap_uc < 0) cap_uc = 0;

                TenantStore::CreatedTenant created;
                try { created = tenants.create_tenant(name, cap_uc); }
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
                // Both fields optional; apply whichever are present.
                if (auto v = body->get("disabled"); v && v->is_bool()) {
                    tenants.set_disabled(std::to_string(id), v->as_bool());
                }
                if (auto v = body->get("monthly_cap_micro_cents"); v && v->is_number()) {
                    tenants.set_cap(id, static_cast<int64_t>(v->as_number()));
                } else if (auto v2 = body->get("monthly_cap_usd"); v2 && v2->is_number()) {
                    tenants.set_cap(id, usd_to_uc(v2->as_number()));
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

    // ── /v1/admin/usage and /v1/admin/usage/summary ─────────────────────
    if (resource == "usage") {
        if (req.method != "GET") { admin_error(fd, 405, "method not allowed"); return; }
        const auto qp = parse_query(req.path);
        auto as_int64 = [&](const std::string& k) -> int64_t {
            auto it = qp.find(k);
            if (it == qp.end()) return 0;
            try { return std::stoll(it->second); } catch (...) { return 0; }
        };
        const int64_t tenant_id = as_int64("tenant_id");
        const int64_t since     = as_int64("since");
        const int64_t until     = as_int64("until");

        // /v1/admin/usage/summary?group_by=model|day|tenant — pre-aggregated
        // rollups for analytics.  Saves the sibling service from pulling
        // tens of thousands of raw rows just to render a chart.
        if (segs.size() == 4 && segs[3] == "summary") {
            std::string group_by = "model";
            if (auto it = qp.find("group_by"); it != qp.end()) group_by = it->second;
            auto buckets = tenants.usage_summary(tenant_id, since, until, group_by);

            auto arr = jarr();
            auto& a = arr->as_array_mut();
            for (auto& b : buckets) a.push_back(bucket_to_json(b));
            auto body = jobj();
            auto& m = body->as_object_mut();
            m["group_by"] = jstr(group_by);
            m["buckets"]  = arr;
            m["count"]    = jnum(static_cast<double>(buckets.size()));
            write_json_response(fd, 200, body);
            return;
        }

        if (segs.size() != 3) {
            admin_error(fd, 404, "admin route not found");
            return;
        }

        const int     limit     = static_cast<int>(as_int64("limit"));
        auto rows = tenants.list_usage(tenant_id, since, until, limit);
        auto arr = jarr();
        auto& a = arr->as_array_mut();
        for (auto& e : rows) a.push_back(usage_to_json(e));
        auto body = jobj();
        auto& m = body->as_object_mut();
        m["entries"] = arr;
        m["count"]   = jnum(static_cast<double>(rows.size()));
        write_json_response(fd, 200, body);
        return;
    }

    admin_error(fd, 404, "admin resource not found");
}

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

// Fire up a disposable orchestrator just to enumerate / load agents.  No
// API calls get made; we only need the Agent map populated for reflection.
// Cheap — the constructor parses a handful of JSON files.
std::unique_ptr<Orchestrator> make_reflect_orchestrator(const ApiServerOptions& opts) {
    auto orch = std::make_unique<Orchestrator>(opts.api_keys);
    if (!opts.agents_dir.empty()) orch->load_agents(opts.agents_dir);
    return orch;
}

void handle_agents_list(int fd, const ApiServerOptions& opts) {
    std::unique_ptr<Orchestrator> orch;
    try { orch = make_reflect_orchestrator(opts); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr(std::string("could not load agents: ") + e.what());
        write_json_response(fd, 500, err);
        return;
    }

    auto arr = jarr();
    auto& a = arr->as_array_mut();
    for (auto& id : orch->list_agents_all()) {
        try {
            a.push_back(constitution_to_json(id, orch->get_constitution(id)));
        } catch (...) { /* skip unreadable */ }
    }
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["agents"] = arr;
    m["count"]  = jnum(static_cast<double>(a.size()));
    write_json_response(fd, 200, body);
}

void handle_agent_get(int fd, const std::string& agent_id,
                       const ApiServerOptions& opts) {
    std::unique_ptr<Orchestrator> orch;
    try { orch = make_reflect_orchestrator(opts); }
    catch (const std::exception& e) {
        auto err = jobj();
        err->as_object_mut()["error"] =
            jstr(std::string("could not load agents: ") + e.what());
        write_json_response(fd, 500, err);
        return;
    }

    try {
        auto& c = orch->get_constitution(agent_id);
        write_json_response(fd, 200, constitution_to_json(agent_id, c));
    } catch (const std::out_of_range&) {
        auto err = jobj();
        err->as_object_mut()["error"] = jstr("no agent '" + agent_id + "'");
        write_json_response(fd, 404, err);
    }
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

void handle_memory_list(int fd, const ApiServerOptions& opts,
                         const Tenant& tenant) {
    const std::string dir = tenant_memory_dir(opts, tenant);
    auto arr = jarr();
    auto& a = arr->as_array_mut();
    if (!dir.empty() && fs::exists(dir)) {
        std::error_code ec;
        for (auto& ent : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            std::string name = ent.path().filename().string();
            // Only .md files — memory is markdown-formatted.
            if (name.size() < 4 || name.substr(name.size() - 3) != ".md") continue;

            auto entry = jobj();
            auto& m = entry->as_object_mut();
            const std::string id = name.substr(0, name.size() - 3);
            m["agent_id"] = jstr(id == "shared" ? "" : id);
            m["kind"]     = jstr(id == "shared" ? "shared" : "agent");
            m["size"]     = jnum(static_cast<double>(ent.file_size(ec)));
            auto ts = fs::last_write_time(ent.path(), ec);
            if (!ec) {
                // filesystem::file_time_type → epoch seconds.  C++20 has
                // file_clock; avoid that to keep the toolchain compatibility
                // matrix narrow — duration_cast via system_clock works for
                // our observational precision (seconds).
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ts - decltype(ts)::clock::now() + std::chrono::system_clock::now());
                m["modified_at"] = jnum(static_cast<double>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        sctp.time_since_epoch()).count()));
            }
            a.push_back(std::move(entry));
        }
    }
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["tenant_id"] = jnum(static_cast<double>(tenant.id));
    m["entries"]   = arr;
    m["count"]     = jnum(static_cast<double>(a.size()));
    write_json_response(fd, 200, body);
}

void handle_memory_read(int fd, const std::string& agent_id,
                         const ApiServerOptions& opts, const Tenant& tenant) {
    if (!agent_id_is_safe(agent_id)) {
        auto e = jobj();
        e->as_object_mut()["error"] = jstr("invalid agent id");
        write_json_response(fd, 400, e);
        return;
    }
    const std::string dir = tenant_memory_dir(opts, tenant);
    if (dir.empty()) {
        auto e = jobj();
        e->as_object_mut()["error"] = jstr("memory not configured on this server");
        write_json_response(fd, 503, e);
        return;
    }
    const std::string path = dir + "/" + agent_id + ".md";
    auto content = read_small_file(path);
    if (!content) {
        // Missing memory is not an error — the agent has simply never
        // written anything yet.  Return 200 with empty content so the
        // sibling UI can render "(no memory yet)" without a special case.
        auto body = jobj();
        auto& m = body->as_object_mut();
        m["agent_id"] = jstr(agent_id == "shared" ? "" : agent_id);
        m["kind"]     = jstr(agent_id == "shared" ? "shared" : "agent");
        m["content"]  = jstr("");
        m["size"]     = jnum(0);
        m["exists"]   = jbool(false);
        write_json_response(fd, 200, body);
        return;
    }
    auto body = jobj();
    auto& m = body->as_object_mut();
    m["agent_id"] = jstr(agent_id == "shared" ? "" : agent_id);
    m["kind"]     = jstr(agent_id == "shared" ? "shared" : "agent");
    m["content"]  = jstr(*content);
    m["size"]     = jnum(static_cast<double>(content->size()));
    m["exists"]   = jbool(true);
    write_json_response(fd, 200, body);
}

// Two entry points funnel here: /v1/orchestrate (agent_override == "", read
// from body) and /v1/agents/:id/chat (agent_override == path :id).  Body
// parsing + dispatch is otherwise identical.
void handle_orchestrate(int fd, const HttpRequest& req,
                        const ApiServerOptions& opts,
                        TenantStore& tenants,
                        const Tenant& tenant_in,
                        const std::string& agent_override = "") {
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

    // Per-request Orchestrator.  Concurrent API calls don't share agent
    // history or callback state — each request is a fresh universe.  The
    // only cost is reloading agent JSON files from disk, which is cheap
    // relative to LLM call latency.
    std::unique_ptr<Orchestrator> orch;
    try {
        orch = std::make_unique<Orchestrator>(opts.api_keys);
    } catch (const std::exception& e) {
        emit_error(sse, std::string("orchestrator init failed: ") + e.what());
        return;
    }
    // Memory is tenant-scoped so /mem commands can never leak between
    // accounts.  Directory created on first write; empty until then.
    if (!opts.memory_root.empty()) {
        orch->set_memory_dir(opts.memory_root + "/t" +
                              std::to_string(tenant.id));
    }
    if (!opts.agents_dir.empty()) orch->load_agents(opts.agents_dir);

    // Install the inline agent definition (pre-validated above, so this
    // can't throw the user-visible errors — any failure now is internal
    // and surfaces as an SSE `error` event).
    if (parsed_cfg) {
        if (orch->has_agent(agent_id)) orch->remove_agent(agent_id);
        orch->create_agent(agent_id, std::move(*parsed_cfg));
    }

    auto* orch_ptr = orch.get();

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
        [&sse, &bytes_captured, cap, stamp](const std::string& path,
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
        sse.emit("file", p);

        return "OK: captured " + std::to_string(size) +
               " bytes for '" + path + "' (streamed to client, not persisted)";
    };
    orch->set_write_interceptor(write_interceptor);
    orch->set_exec_disabled(opts.exec_disabled);

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
        [&sse](const std::string& agent, int sid, const std::string& delta) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(agent);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["delta"]     = jstr(delta);
            sse.emit("text", p);
        });

    // agent_start still fires for delegated turns (just before the API call).
    // Keep it so consumers can distinguish "turn opened" (stream_start) from
    // "API call about to go out" (agent_start) — different timings once
    // we add pre-call checks.
    orch->set_agent_start_callback([&sse, stamp](const std::string&) {
        auto p = jobj();
        stamp(p);
        sse.emit("agent_start", p);
    });
    orch->set_tool_status_callback(
        [&sse, stamp](const std::string& kind, bool ok) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["tool"] = jstr(kind);
            m["ok"]   = jbool(ok);
            stamp(p);
            sse.emit("tool_call", p);
        });
    // Per-turn billing.  Every agent turn (master + delegated) records:
    //   provider_uc  — our actual cost to the LLM vendor, in µ¢
    //   markup_uc    — 20% over that, rounded up to the nearest µ¢
    // Total charged to the tenant = provider_uc + markup_uc.  If the new
    // MTD exceeds the tenant's monthly cap, cancel the in-flight
    // orchestration so the rest of this request stops generating cost.
    std::atomic<int64_t> req_provider_uc{0};
    std::atomic<int64_t> req_markup_uc{0};
    std::atomic<bool>    cap_exceeded{false};

    orch->set_cost_callback(
        [&sse, &tenants, &tenant, &req_provider_uc, &req_markup_uc,
         &cap_exceeded, orch_ptr, stamp](const std::string& id,
                                          const std::string& model,
                                          const ApiResponse& resp) {
            // Breakdown is captured at write time so historical rows don't
            // drift when the pricing table updates later.  Sum equals
            // provider_uc; markup is 20% of that, rounded up.
            const auto bd = CostTracker::compute_cost_breakdown(model, resp);
            TenantStore::CostParts parts;
            parts.input_uc        = usd_to_uc(bd.input);
            parts.output_uc       = usd_to_uc(bd.output);
            parts.cache_read_uc   = usd_to_uc(bd.cache_read);
            parts.cache_create_uc = usd_to_uc(bd.cache_create);
            const int64_t provider_uc = parts.input_uc + parts.output_uc +
                                        parts.cache_read_uc + parts.cache_create_uc;
            const int64_t markup      = markup_uc(provider_uc);
            req_provider_uc.fetch_add(provider_uc);
            req_markup_uc.fetch_add(markup);

            int64_t mtd = 0;
            try {
                mtd = tenants.record_usage(
                    tenant.id, model,
                    resp.input_tokens, resp.output_tokens,
                    resp.cache_read_tokens,
                    resp.cache_creation_tokens,
                    parts, markup);
            } catch (...) {
                // Accounting failure is not fatal to the request itself —
                // the turn already ran, the cost already landed.  Emit an
                // error event so the operator sees it; keep streaming.
                auto e = jobj();
                e->as_object_mut()["message"] =
                    jstr("usage accounting failed — this turn is not billed");
                sse.emit("error", e);
            }

            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]                  = jstr(id);
            m["model"]                  = jstr(model);
            m["input_tokens"]           = jnum(static_cast<double>(resp.input_tokens));
            m["output_tokens"]          = jnum(static_cast<double>(resp.output_tokens));
            if (resp.cache_read_tokens > 0)
                m["cache_read_tokens"]   = jnum(static_cast<double>(resp.cache_read_tokens));
            if (resp.cache_creation_tokens > 0)
                m["cache_create_tokens"] = jnum(static_cast<double>(resp.cache_creation_tokens));

            // Per-token-type cost breakdown.  Same units as provider_micro_cents.
            // Sums to provider_micro_cents; consumers can render a stacked-bar
            // chart of input/output/cache spend without re-pricing.
            m["input_micro_cents"]        = jnum(static_cast<double>(parts.input_uc));
            m["output_micro_cents"]       = jnum(static_cast<double>(parts.output_uc));
            m["cache_read_micro_cents"]   = jnum(static_cast<double>(parts.cache_read_uc));
            m["cache_create_micro_cents"] = jnum(static_cast<double>(parts.cache_create_uc));

            m["provider_micro_cents"]   = jnum(static_cast<double>(provider_uc));
            m["billed_micro_cents"]     = jnum(static_cast<double>(provider_uc + markup));
            m["mtd_micro_cents"]        = jnum(static_cast<double>(mtd));
            m["stream_id"]              = jnum(static_cast<double>(orch_ptr->current_stream_id()));
            m["depth"]                  = jnum(static_cast<double>(orch_ptr->current_stream_depth()));
            sse.emit("token_usage", p);

            // Cap enforcement.  0 = unlimited.  If exceeded, cancel the
            // orchestrator to abort any in-flight stream and the next
            // turn's API call.
            if (tenant.monthly_cap_uc > 0 && mtd > tenant.monthly_cap_uc &&
                !cap_exceeded.exchange(true)) {
                auto e = jobj();
                auto& em = e->as_object_mut();
                em["message"] = jstr("monthly usage cap exceeded");
                em["mtd_micro_cents"] = jnum(static_cast<double>(mtd));
                em["cap_micro_cents"] = jnum(static_cast<double>(tenant.monthly_cap_uc));
                sse.emit("error", e);
                if (orch_ptr) orch_ptr->cancel();
            }
        });
    // progress_callback fires at depth>0 with the sub-agent's full turn
    // output — the "completed delegation" signal for the caller.  Still
    // useful alongside streamed text: if the consumer wants to show the
    // final assistant message cleanly (no token-by-token reconstruction),
    // this event delivers the full assembled body once per turn.
    orch->set_progress_callback(
        [&sse, orch_ptr](const std::string& id, const std::string& content) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(orch_ptr->current_stream_id()));
            m["depth"]     = jnum(static_cast<double>(orch_ptr->current_stream_depth()));
            m["content"]   = jstr(content);
            sse.emit("sub_agent_response", p);
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
    // consumers can correlate streams in logs.
    {
        auto p = jobj();
        auto& m = p->as_object_mut();
        m["agent"]     = jstr(agent_id);
        m["tenant_id"] = jnum(static_cast<double>(tenant.id));
        m["tenant"]    = jstr(tenant.name);
        m["message"]   = jstr(message.size() > 200
                              ? message.substr(0, 200) + "…"
                              : message);
        sse.emit("request_received", p);
    }

    // Pre-flight cap check.  The per-turn callback catches the limit mid-
    // request; this catches the case where the tenant was already over cap
    // before this request even started (e.g., cap lowered by admin).
    if (tenant.monthly_cap_uc > 0 &&
        tenant.month_to_date_uc >= tenant.monthly_cap_uc) {
        auto e = jobj();
        auto& em = e->as_object_mut();
        em["message"] = jstr("monthly usage cap already reached — no work done");
        em["mtd_micro_cents"] = jnum(static_cast<double>(tenant.month_to_date_uc));
        em["cap_micro_cents"] = jnum(static_cast<double>(tenant.monthly_cap_uc));
        sse.emit("error", e);
        sse.close();
        return;
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
        [&sse, master_sid, master_agent](const std::string& id, int sid, int depth) {
            if (depth == 0) {
                *master_sid   = sid;
                *master_agent = id;
            }
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["depth"]     = jnum(static_cast<double>(depth));
            sse.emit("stream_start", p);
        });

    Config cfg;   // cfg.verbose defaults to false, which is what we want
    StreamFilter filter(cfg,
        [&sse, master_sid, master_agent](const std::string& chunk) {
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(*master_agent);
            m["stream_id"] = jnum(static_cast<double>(*master_sid));
            m["depth"]     = jnum(0);
            m["delta"]     = jstr(chunk);
            sse.emit("text", p);
        });

    // Wired here (not further up) so the handler can drain the master's
    // line buffer before stream_end lands on the wire.  Non-master streams
    // don't use this filter; flushing it for them is a no-op.
    orch->set_stream_end_callback(
        [&sse, &filter, master_sid](const std::string& id, int sid, bool ok) {
            if (sid == *master_sid) filter.flush();
            auto p = jobj();
            auto& m = p->as_object_mut();
            m["agent"]     = jstr(id);
            m["stream_id"] = jnum(static_cast<double>(sid));
            m["ok"]        = jbool(ok);
            sse.emit("stream_end", p);
        });

    try {
        auto resp = orch->send_streaming(agent_id, message,
            [&filter](const std::string& chunk) { filter.feed(chunk); });
        filter.flush();

        auto done = jobj();
        auto& m = done->as_object_mut();
        m["ok"]      = jbool(resp.ok);
        if (!resp.ok) m["error"] = jstr(resp.error);
        m["content"] = jstr(resp.content);
        m["input_tokens"]  = jnum(static_cast<double>(resp.input_tokens));
        m["output_tokens"] = jnum(static_cast<double>(resp.output_tokens));
        m["files_bytes"]   = jnum(static_cast<double>(bytes_captured.load()));

        // Billing summary for the whole request.  provider_micro_cents is
        // what we paid the LLM vendor; billed_micro_cents is what we bill
        // the tenant (provider + 20% markup).  Both are sums across all
        // turns in this request (master + delegated sub-agents).
        const int64_t prov  = req_provider_uc.load();
        const int64_t mk    = req_markup_uc.load();
        m["provider_micro_cents"] = jnum(static_cast<double>(prov));
        m["billed_micro_cents"]   = jnum(static_cast<double>(prov + mk));
        m["markup_micro_cents"]   = jnum(static_cast<double>(mk));
        m["tenant_id"]            = jnum(static_cast<double>(tenant.id));
        m["cap_exceeded"]         = jbool(cap_exceeded.load());

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        m["duration_ms"] = jnum(static_cast<double>(elapsed_ms));
        sse.emit("done", done);
    } catch (const std::exception& e) {
        emit_error(sse, std::string("orchestration failed: ") + e.what());
    }

    sse.close();
}

} // namespace

// ─── ApiServer public API ───────────────────────────────────────────────────

ApiServer::ApiServer(ApiServerOptions opts, TenantStore& tenants)
    : opts_(std::move(opts)), tenants_(tenants) {}

ApiServer::~ApiServer() { stop(); }

void ApiServer::start() {
    if (running_.load()) return;

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

    // Health check — no auth, tiny response, useful for liveness probes.
    if (req.method == "GET" && req.path == "/v1/health") {
        write_plain_response(fd, 200, "OK", "ok\n");
        return;
    }

    // Admin routes have their own auth (admin token, not tenant tokens).
    // Matched by prefix so /v1/admin, /v1/admin/tenants, /v1/admin/usage?…
    // all funnel into handle_admin, which sub-dispatches.
    if (req.path.rfind("/v1/admin", 0) == 0) {
        handle_admin(fd, req, tenants_, opts_);
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

    if (req.method == "POST" && req.path == "/v1/orchestrate") {
        handle_orchestrate(fd, req, opts_, tenants_, *tenant);
        return;
    }

    // ── Agent discovery + direct chat ────────────────────────────────────
    // GET /v1/agents               — list all loadable agents (master + children)
    // GET /v1/agents/:id           — one agent's constitution
    // POST /v1/agents/:id/chat     — orchestrate, but agent_id comes from path
    {
        const auto segs = split_path(req.path);
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "agents") {
            if (req.method == "GET" && segs.size() == 2) {
                handle_agents_list(fd, opts_);
                return;
            }
            if (req.method == "GET" && segs.size() == 3) {
                handle_agent_get(fd, segs[2], opts_);
                return;
            }
            if (req.method == "POST" && segs.size() == 4 && segs[3] == "chat") {
                handle_orchestrate(fd, req, opts_, tenants_, *tenant, segs[2]);
                return;
            }
            write_plain_response(fd, 404, "Not Found", "agents route not found\n");
            return;
        }

        // ── Memory ────────────────────────────────────────────────────────
        // GET /v1/memory            — list this tenant's memory files
        // GET /v1/memory/shared     — the shared scratchpad
        // GET /v1/memory/:agent_id  — one agent's persistent memory
        if (segs.size() >= 2 && segs[0] == "v1" && segs[1] == "memory") {
            if (req.method != "GET") {
                write_plain_response(fd, 405, "Method Not Allowed",
                                     "memory endpoints are read-only\n");
                return;
            }
            if (segs.size() == 2) {
                handle_memory_list(fd, opts_, *tenant);
                return;
            }
            if (segs.size() == 3) {
                handle_memory_read(fd, segs[2], opts_, *tenant);
                return;
            }
            write_plain_response(fd, 404, "Not Found", "memory route not found\n");
            return;
        }
    }

    write_plain_response(fd, 404, "Not Found", "endpoint not found\n");
}

} // namespace index_ai
