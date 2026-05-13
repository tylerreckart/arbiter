// tests/test_metrics.cpp — Pins the Prometheus exposition-format
// contract for the /metrics endpoint: every registered metric gets
// HELP + TYPE headers even with zero observations, increments
// accumulate per labels, label escaping handles the troublesome bytes,
// and gauges go up and down.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "metrics.h"

#include <string>

using namespace arbiter;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("fresh registry emits HELP and TYPE for every metric") {
    Metrics m;
    auto body = m.render();
    CHECK(contains(body, "# HELP arbiter_requests_started_total"));
    CHECK(contains(body, "# TYPE arbiter_requests_started_total counter"));
    CHECK(contains(body, "# TYPE arbiter_in_flight gauge"));
    CHECK(contains(body, "# TYPE arbiter_sandbox_containers_running gauge"));
}

TEST_CASE("counters accumulate per label tuple") {
    Metrics m;
    m.inc_request_started(1, "orchestrate");
    m.inc_request_started(1, "orchestrate");
    m.inc_request_started(2, "orchestrate");
    m.inc_request_started(1, "messages");
    auto body = m.render();
    CHECK(contains(body,
        "arbiter_requests_started_total{tenant=\"1\",route=\"orchestrate\"} 2"));
    CHECK(contains(body,
        "arbiter_requests_started_total{tenant=\"2\",route=\"orchestrate\"} 1"));
    CHECK(contains(body,
        "arbiter_requests_started_total{tenant=\"1\",route=\"messages\"} 1"));
}

TEST_CASE("completed counter distinguishes ok=true and ok=false") {
    Metrics m;
    m.inc_request_completed(7, "orchestrate", true);
    m.inc_request_completed(7, "orchestrate", true);
    m.inc_request_completed(7, "orchestrate", false);
    auto body = m.render();
    CHECK(contains(body,
        "arbiter_requests_completed_total{tenant=\"7\",route=\"orchestrate\",ok=\"true\"} 2"));
    CHECK(contains(body,
        "arbiter_requests_completed_total{tenant=\"7\",route=\"orchestrate\",ok=\"false\"} 1"));
}

TEST_CASE("in_flight gauge moves up and down") {
    Metrics m;
    m.inc_in_flight(3);
    m.inc_in_flight(3);
    m.inc_in_flight(3);
    m.dec_in_flight(3);
    auto body = m.render();
    CHECK(contains(body, "arbiter_in_flight{tenant=\"3\"} 2"));
}

TEST_CASE("sandbox gauge can be set absolutely") {
    Metrics m;
    m.set_sandbox_containers_running(5);
    CHECK(contains(m.render(), "arbiter_sandbox_containers_running 5"));
    m.set_sandbox_containers_running(2);
    CHECK(contains(m.render(), "arbiter_sandbox_containers_running 2"));
}

TEST_CASE("label values are escaped per text format") {
    Metrics m;
    m.inc_rate_limited(1, "rate\"limit\\funky");
    auto body = m.render();
    CHECK(contains(body,
        "arbiter_rate_limited_total{tenant=\"1\",reason=\"rate\\\"limit\\\\funky\"}"));
}

TEST_CASE("unlabeled metrics render without braces") {
    Metrics m;
    m.inc_sandbox_exec();
    m.inc_sandbox_exec();
    auto body = m.render();
    CHECK(contains(body, "arbiter_sandbox_exec_total 2"));
    // Negative: no labels braces on the unlabeled metric line.
    CHECK(!contains(body, "arbiter_sandbox_exec_total{"));
}

TEST_CASE("provider counters track per provider") {
    Metrics m;
    m.inc_provider_call("anthropic");
    m.inc_provider_call("anthropic");
    m.inc_provider_call("openai");
    m.inc_provider_5xx("anthropic");
    auto body = m.render();
    CHECK(contains(body,
        "arbiter_provider_calls_total{provider=\"anthropic\"} 2"));
    CHECK(contains(body,
        "arbiter_provider_calls_total{provider=\"openai\"} 1"));
    CHECK(contains(body,
        "arbiter_provider_5xx_total{provider=\"anthropic\"} 1"));
}
