// tests/test_json_numbers.cpp — RFC 8259 number tokenizer.
//
// Distinct from #342 (UTF-16 surrogate pairs in parse_string). This
// pins frac/exp completeness: incomplete tokens like `1.` / `1e` were
// accepted because strtod parses a prefix of the tokenizer's capture.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "json.h"

#include <stdexcept>
#include <string>

using namespace arbiter;

namespace {

double parse_num(const char* s) {
    auto v = json_parse(s);
    REQUIRE(v);
    REQUIRE(v->is_number());
    return v->as_number();
}

void require_throws(const char* s) {
    CHECK_THROWS_AS(json_parse(s), std::runtime_error);
}

} // namespace

TEST_CASE("json_parse accepts well-formed JSON numbers") {
    CHECK(parse_num("0") == 0.0);
    CHECK(parse_num("-0") == 0.0);
    CHECK(parse_num("1") == 1.0);
    CHECK(parse_num("-42") == -42.0);
    CHECK(parse_num("1.0") == 1.0);
    CHECK(parse_num("0.5") == 0.5);
    CHECK(parse_num("-0.25") == -0.25);
    CHECK(parse_num("1e2") == 100.0);
    CHECK(parse_num("1E2") == 100.0);
    CHECK(parse_num("1e+2") == 100.0);
    CHECK(parse_num("1e-2") == 0.01);
    CHECK(parse_num("1.5e1") == 15.0);
    CHECK(parse_num("6.02e23") == 6.02e23);
}

TEST_CASE("json_parse rejects incomplete fraction and exponent") {
    require_throws("1.");
    require_throws("-1.");
    require_throws("1e");
    require_throws("1E");
    require_throws("1e+");
    require_throws("1e-");
    require_throws("1.e2");
    require_throws("1.E+3");
    require_throws("-.5");
}

TEST_CASE("json_parse rejects incomplete numbers inside composites") {
    require_throws("[1.,2]");
    require_throws("[1e]");
    require_throws("{\"n\":1e}");
    require_throws("{\"n\":1.}");
    // A well-formed neighbour still parses; only the incomplete token fails.
    auto ok = json_parse("[1.0,2]");
    REQUIRE(ok);
    REQUIRE(ok->is_array());
    CHECK(ok->as_array().size() == 2);
}

TEST_CASE("json_parse overflow still fails closed") {
    require_throws("1e9999");
}
