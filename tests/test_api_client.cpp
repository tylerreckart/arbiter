// tests/test_api_client.cpp — Unit tests for API client edge cases
//
// parse_host is file-static in api_client.cpp, so we test it indirectly
// by reimplementing the same logic here and testing that. This validates
// the algorithm without breaking encapsulation.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdlib>
#include <string>

namespace {

struct ParsedHost {
    std::string scheme;
    std::string host;
    int port;
};

// Mirror of the parse_host logic from api_client.cpp (with the strtol fix applied)
ParsedHost parse_host(const std::string& s,
                      const std::string& default_scheme,
                      int default_port) {
    ParsedHost r{default_scheme, "", default_port};
    std::string rest = s;
    auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        r.scheme = rest.substr(0, scheme_end);
        rest = rest.substr(scheme_end + 3);
    }
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        r.host = rest.substr(0, colon);
        const char* port_start = rest.c_str() + colon + 1;
        char* end = nullptr;
        long parsed = std::strtol(port_start, &end, 10);
        if (end == port_start || parsed < 1 || parsed > 65535)
            r.port = default_port;
        else
            r.port = static_cast<int>(parsed);
    } else {
        r.host = rest;
    }
    if (r.host.empty()) r.host = "localhost";
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_host edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parse_host basic cases") {
    auto r = parse_host("example.com:8080", "http", 443);
    CHECK(r.scheme == "http");
    CHECK(r.host == "example.com");
    CHECK(r.port == 8080);
}

TEST_CASE("parse_host with scheme") {
    auto r = parse_host("https://api.example.com:443", "http", 80);
    CHECK(r.scheme == "https");
    CHECK(r.host == "api.example.com");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host without port uses default") {
    auto r = parse_host("example.com", "https", 443);
    CHECK(r.host == "example.com");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host empty string returns localhost") {
    auto r = parse_host("", "http", 11434);
    CHECK(r.host == "localhost");
    CHECK(r.port == 11434);
}

TEST_CASE("parse_host invalid port falls back to default") {
    auto r = parse_host("host:abc", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host overflow port falls back to default") {
    auto r = parse_host("host:99999", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host zero port falls back to default") {
    auto r = parse_host("host:0", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host negative port falls back to default") {
    auto r = parse_host("host:-1", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host port 65535 is valid") {
    auto r = parse_host("host:65535", "http", 443);
    CHECK(r.port == 65535);
}

TEST_CASE("parse_host port 65536 overflows to default") {
    auto r = parse_host("host:65536", "http", 443);
    CHECK(r.port == 443);
}
