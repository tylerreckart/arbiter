#pragma once
// arbiter/include/api/http_transport.h
//
// Low-level HTTP socket writes and per-request CORS Origin TLS.

#include "cors.h"

#include <cstddef>
#include <string>

namespace arbiter {

void write_all(int fd, const char* data, size_t n);
void write_all(int fd, const std::string& s);

void init_api_cors_policy_from_env();
const CorsPolicy& api_cors_policy();
std::string current_cors_headers();

// Stash the inbound Origin header for CORS response builders on this
// connection thread.  Cleared on scope exit.
class RequestOriginScope {
public:
    explicit RequestOriginScope(const std::string& origin);
    ~RequestOriginScope();
    RequestOriginScope(const RequestOriginScope&)            = delete;
    RequestOriginScope& operator=(const RequestOriginScope&) = delete;
};

} // namespace arbiter
