// arbiter/src/api/http_transport.cpp

#include "api/http_transport.h"

#include "cors.h"

#include <string>

#include <sys/socket.h>

namespace arbiter {

namespace {

CorsPolicy g_cors_policy{};
thread_local std::string g_tls_request_origin;

} // namespace

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

void write_all(int fd, const std::string& s) {
    write_all(fd, s.data(), s.size());
}

void init_api_cors_policy_from_env() {
    g_cors_policy = load_cors_policy_from_env();
}

const CorsPolicy& api_cors_policy() {
    return g_cors_policy;
}

std::string current_cors_headers() {
    return cors_headers_for(g_cors_policy, g_tls_request_origin);
}

RequestOriginScope::RequestOriginScope(const std::string& origin) {
    g_tls_request_origin = origin;
}

RequestOriginScope::~RequestOriginScope() {
    g_tls_request_origin.clear();
}

} // namespace arbiter
