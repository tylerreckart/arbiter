// tests/test_http_request.cpp — inbound HTTP/1.1 request framing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "api/http_request.h"

#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace arbiter;

namespace {

bool parse_from_bytes(const std::string& raw, HttpRequest& req) {
    int sp[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return false;
    const ssize_t w = ::write(sp[0], raw.data(), raw.size());
    ::shutdown(sp[0], SHUT_WR);
    const bool ok = parse_http_request(sp[1], req);
    ::close(sp[0]);
    ::close(sp[1]);
    return ok && w == static_cast<ssize_t>(raw.size());
}

std::string request_with(const std::string& headers, const std::string& body) {
    return "POST /v1/orchestrate HTTP/1.1\r\n" + headers + "\r\n" + body;
}

} // namespace

TEST_CASE("exact Content-Length leftover is the body") {
    HttpRequest req;
    REQUIRE(parse_from_bytes(
        request_with("Content-Length: 5\r\n", "hello"), req));
    CHECK(req.method == "POST");
    CHECK(req.path == "/v1/orchestrate");
    CHECK(req.body == "hello");
}

TEST_CASE("body is capped to Content-Length when leftover overshoots") {
    HttpRequest req;
    REQUIRE(parse_from_bytes(
        request_with("Content-Length: 5\r\n", "helloXXXXEXTRA"), req));
    CHECK(req.body == "hello");
}

TEST_CASE("Content-Length 0 does not adopt leftover as body") {
    HttpRequest req;
    REQUIRE(parse_from_bytes(
        request_with("Content-Length: 0\r\n",
                     "{\"agent\":\"smuggled\"}"), req));
    CHECK(req.body.empty());
}

TEST_CASE("GET without Content-Length drops leftover bytes") {
    HttpRequest req;
    REQUIRE(parse_from_bytes(
        "GET /v1/health HTTP/1.1\r\nHost: x\r\n\r\nPOST /v1/orchestrate HTTP/1.1\r\n\r\n",
        req));
    CHECK(req.method == "GET");
    CHECK(req.path == "/v1/health");
    CHECK(req.body.empty());
}

TEST_CASE("short body after Content-Length is rejected") {
    HttpRequest req;
    CHECK_FALSE(parse_from_bytes(
        request_with("Content-Length: 10\r\n", "short"), req));
}

TEST_CASE("duplicate Content-Length is rejected") {
    HttpRequest req;
    CHECK_FALSE(parse_from_bytes(
        request_with("Content-Length: 5\r\nContent-Length: 5\r\n", "hello"),
        req));
}

TEST_CASE("Transfer-Encoding is rejected") {
    HttpRequest req;
    CHECK_FALSE(parse_from_bytes(
        request_with("Transfer-Encoding: chunked\r\nContent-Length: 5\r\n",
                     "hello"),
        req));
}
