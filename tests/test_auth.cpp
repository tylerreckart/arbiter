// tests/test_auth.cpp — RFC 6750 / RFC 9110 Bearer Authorization parsing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "api/http_util.h"

using namespace arbiter;

TEST_CASE("canonical Bearer scheme extracts the token") {
    CHECK(parse_bearer_authorization("Bearer atr_secret") == "atr_secret");
}

TEST_CASE("auth-scheme is case-insensitive") {
    CHECK(parse_bearer_authorization("bearer atr_secret") == "atr_secret");
    CHECK(parse_bearer_authorization("BEARER atr_secret") == "atr_secret");
    CHECK(parse_bearer_authorization("BeArEr atr_secret") == "atr_secret");
}

TEST_CASE("one or more spaces after the scheme are skipped") {
    CHECK(parse_bearer_authorization("Bearer  atr_secret") == "atr_secret");
    CHECK(parse_bearer_authorization("bearer   atr_secret") == "atr_secret");
}

TEST_CASE("trailing OWS is not part of the token") {
    CHECK(parse_bearer_authorization("Bearer atr_secret ") == "atr_secret");
    CHECK(parse_bearer_authorization("Bearer atr_secret\t") == "atr_secret");
    CHECK(parse_bearer_authorization("Bearer atr_secret \t ") == "atr_secret");
}

TEST_CASE("non-Bearer schemes and malformed fields are rejected") {
    CHECK(parse_bearer_authorization("").empty());
    CHECK(parse_bearer_authorization("Bearer").empty());
    CHECK(parse_bearer_authorization("Bearer ").empty());
    CHECK(parse_bearer_authorization("Bearer    ").empty());
    CHECK(parse_bearer_authorization("Bearer\tatr_secret").empty());
    CHECK(parse_bearer_authorization("Basic atr_secret").empty());
    CHECK(parse_bearer_authorization("XBearer atr_secret").empty());
    CHECK(parse_bearer_authorization("Bearerat_secret").empty());
}
