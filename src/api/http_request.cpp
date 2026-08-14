// arbiter/src/api/http_request.cpp

#include "api/http_request.h"

#include <cctype>
#include <sstream>
#include <string>

#include <sys/socket.h>

namespace arbiter {

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Read headers up to CRLFCRLF; excess bytes go into `leftover` for the body reader.
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

} // namespace

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

} // namespace arbiter
