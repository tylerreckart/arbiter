#pragma once
// arbiter/include/api/http_request.h
//
// Parsed HTTP/1.1 request for the minimal API server.

#include <map>
#include <string>

namespace arbiter {

struct HttpRequest {
    std::string method;     // "GET", "POST"
    std::string path;       // "/v1/orchestrate"
    std::string version;    // "HTTP/1.1"
    std::map<std::string, std::string> headers; // canonical lowercase keys
    std::string body;
};

bool parse_http_request(int fd, HttpRequest& req);

} // namespace arbiter
