// tests/test_cors.cpp — CORS allowlist policy for ARBITER_CORS_ORIGINS.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "cors.h"

using namespace arbiter;

TEST_CASE("empty CSV is permissive") {
    auto p = cors_policy_from_csv("");
    CHECK(p.allowed_origins.empty());
    CHECK(cors_origin_allowed(p, "https://example.com"));
    CHECK(cors_origin_allowed(p, ""));
    auto h = cors_headers_for(p, "https://example.com");
    CHECK(h.find("Access-Control-Allow-Origin: *\r\n") != std::string::npos);
    CHECK(h.find("Vary:") == std::string::npos);
}

TEST_CASE("CSV allowlist trims whitespace and matches exactly") {
    auto p = cors_policy_from_csv(
        " https://app.example.com ,http://localhost:5173, ");
    REQUIRE(p.allowed_origins.size() == 2);
    CHECK(p.allowed_origins[0] == "https://app.example.com");
    CHECK(p.allowed_origins[1] == "http://localhost:5173");

    CHECK(cors_origin_allowed(p, "https://app.example.com"));
    CHECK(cors_origin_allowed(p, "http://localhost:5173"));
    CHECK_FALSE(cors_origin_allowed(p, "https://evil.example"));
    CHECK_FALSE(cors_origin_allowed(p, ""));
}

TEST_CASE("allowlist echoes matching Origin and omits mismatches") {
    auto p = cors_policy_from_csv("https://app.example.com");
    auto ok = cors_headers_for(p, "https://app.example.com");
    CHECK(ok.find("Access-Control-Allow-Origin: https://app.example.com\r\n")
          != std::string::npos);
    CHECK(ok.find("Vary: Origin\r\n") != std::string::npos);
    CHECK(ok.find("Access-Control-Allow-Methods:") != std::string::npos);

    auto bad = cors_headers_for(p, "https://evil.example");
    CHECK(bad.find("Access-Control-Allow-Origin:") == std::string::npos);
    CHECK(bad.find("Access-Control-Allow-Methods:") != std::string::npos);

    auto missing = cors_headers_for(p, "");
    CHECK(missing.find("Access-Control-Allow-Origin:") == std::string::npos);
}
