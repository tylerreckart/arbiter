// arbiter/src/api/http_response.cpp

#include "api/http_response.h"
#include "api/http_transport.h"

#include "json.h"

#include <sstream>
#include <string>

namespace arbiter {

void write_plain_response(int fd, int code, const std::string& reason,
                          const std::string& body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << reason << "\r\n"
       << "Content-Type: text/plain; charset=utf-8\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << current_cors_headers()
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
       << current_cors_headers()
       << "Connection: close\r\n\r\n"
       << payload;
    write_all(fd, ss.str());
}

void write_429_response(int fd, int retry_after_seconds, const char* reason,
                         Metrics* metrics, int64_t tenant_id) {
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
       << current_cors_headers()
       << "Connection: close\r\n\r\n"
       << payload;
    write_all(fd, ss.str());
}

void write_preflight_response(int fd) {
    std::ostringstream ss;
    ss << "HTTP/1.1 204 No Content\r\n"
       << current_cors_headers()
       << "Content-Length: 0\r\n"
       << "Connection: close\r\n\r\n";
    write_all(fd, ss.str());
}

} // namespace arbiter
