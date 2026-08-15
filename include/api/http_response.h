#pragma once
// arbiter/include/api/http_response.h
//
// Non-SSE HTTP response writers shared across API route handlers.

#include "json.h"
#include "metrics.h"

#include <cstdint>
#include <memory>
#include <string>

namespace arbiter {

void write_plain_response(int fd, int code, const std::string& reason,
                          const std::string& body);
void write_json_response(int fd, int code, std::shared_ptr<JsonValue> body);
void write_429_response(int fd, int retry_after_seconds, const char* reason,
                         Metrics* metrics = nullptr, int64_t tenant_id = 0);
void write_preflight_response(int fd);

} // namespace arbiter
