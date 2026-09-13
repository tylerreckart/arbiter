// tests/test_http_util.cpp — query-string decode used by list handlers
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "api/http_util.h"

using namespace arbiter;

TEST_CASE("url_decode turns percent-escapes and plus into the raw value") {
    CHECK(url_decode("rate%20limit") == "rate limit");
    CHECK(url_decode("rate+limit") == "rate limit");
    CHECK(url_decode("in%5Fprogress") == "in_progress");
    CHECK(url_decode("plain") == "plain");
    CHECK(url_decode("%2Ftmp") == "/tmp");
    // Invalid % sequences stay literal (same as the previous splitters).
    CHECK(url_decode("100%") == "100%");
    CHECK(url_decode("%GG") == "%GG");
}

TEST_CASE("parse_query decodes list-handler filters the hand-rolled split missed") {
    // GET /v1/lessons?q=rate%20limit — browsers and curl --data-urlencode
    // send this; the old pair-splitter searched for the literal "%20".
    auto lessons = parse_query("/v1/lessons?agent_id=scout&q=rate%20limit&limit=20");
    CHECK(lessons["agent_id"] == "scout");
    CHECK(lessons["q"] == "rate limit");
    CHECK(lessons["limit"] == "20");

    auto plus = parse_query("/v1/lessons?q=rate+limit");
    CHECK(plus["q"] == "rate limit");

    auto todos = parse_query("/v1/todos?status=in%5Fprogress&conversation_id=tenant");
    CHECK(todos["status"] == "in_progress");
    CHECK(todos["conversation_id"] == "tenant");

    auto schedules = parse_query("/v1/schedules?status=active");
    CHECK(schedules["status"] == "active");

    // Numeric filters: encoded digits must still parse with stoll/stoi.
    auto audit = parse_query("/v1/admin/audit?before_id=%31%30&limit=%35%30");
    CHECK(audit["before_id"] == "10");
    CHECK(audit["limit"] == "50");

    auto encoded_key = parse_query("/v1/lessons?q%3D=nope&q=ok");
    CHECK(encoded_key["q"] == "ok");
    CHECK(encoded_key["q="] == "nope");
}

TEST_CASE("parse_query last value wins and ignores valueless pairs") {
    auto qp = parse_query("/v1/lessons?q=first&q=second&flag&limit=5");
    CHECK(qp["q"] == "second");
    CHECK(qp["limit"] == "5");
    CHECK(qp.count("flag") == 0);
    CHECK(parse_query("/v1/lessons").empty());
}
