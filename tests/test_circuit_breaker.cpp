// tests/test_circuit_breaker.cpp — Pins the closed/open/half-open
// state-machine contract the ApiClient retry loop depends on.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "circuit_breaker.h"

#include <thread>
#include <chrono>

using namespace arbiter;

TEST_CASE("fresh breaker admits every call") {
    ProviderCircuitBreaker cb;
    for (int i = 0; i < 100; ++i) {
        CHECK(cb.allow("anthropic"));
        cb.record_success("anthropic");
    }
    CHECK(cb.state("anthropic") == ProviderCircuitBreaker::State::Closed);
}

TEST_CASE("threshold-many consecutive failures trip the breaker") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    cfg.cooldown_seconds  = 60;
    ProviderCircuitBreaker cb(cfg);

    // Two failures: still closed.
    CHECK(cb.allow("p"));
    cb.record_failure("p");
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::Closed);
    CHECK(cb.allow("p"));
    cb.record_failure("p");
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::Closed);

    // Third failure: trip.
    CHECK(cb.allow("p"));
    cb.record_failure("p");
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::Open);

    // Subsequent admit rejected (still inside cooldown).
    CHECK(!cb.allow("p"));
}

TEST_CASE("intermediate successes reset the consecutive counter") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    ProviderCircuitBreaker cb(cfg);

    cb.record_failure("p");
    cb.record_failure("p");
    cb.record_success("p");           // counter resets
    cb.record_failure("p");           // only 1 consecutive
    cb.record_failure("p");           // 2
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::Closed);
}

TEST_CASE("cooldown elapses → next allow is the half-open probe") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown_seconds  = 0;   // immediate cooldown for test
    ProviderCircuitBreaker cb(cfg);

    cb.record_failure("p");
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::Open);

    // cooldown=0 → first allow after Open transitions to HalfOpen.
    CHECK(cb.allow("p"));
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::HalfOpen);

    // Second concurrent probe rejected while the first is in flight.
    CHECK(!cb.allow("p"));
}

TEST_CASE("half-open probe success closes the breaker") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown_seconds  = 0;
    ProviderCircuitBreaker cb(cfg);

    cb.record_failure("p");
    CHECK(cb.allow("p"));   // → HalfOpen
    cb.record_success("p"); // probe ok → Closed
    CHECK(cb.state("p") == ProviderCircuitBreaker::State::Closed);

    // Closed again; admits and counter resets so prior failure doesn't
    // immediately trip the next round.
    CHECK(cb.allow("p"));
}

TEST_CASE("half-open probe failure reopens with fresh cooldown") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown_seconds  = 60;
    ProviderCircuitBreaker cb(cfg);

    cb.record_failure("p");
    // Force HalfOpen by cheating cooldown to 0 won't work post-construct,
    // so simulate the natural flow: shorten the config in a fresh
    // instance.
    ProviderCircuitBreaker cb2(CircuitBreakerConfig{1, 0});
    cb2.record_failure("q");
    CHECK(cb2.allow("q"));   // HalfOpen probe admitted
    cb2.record_failure("q"); // probe fails → reopen
    CHECK(cb2.state("q") == ProviderCircuitBreaker::State::Open);

    // Long cooldown: cb (above) stays Open through this assertion.
    CHECK(!cb.allow("p"));
}

TEST_CASE("providers are tracked independently") {
    ProviderCircuitBreaker cb(CircuitBreakerConfig{2, 60});
    cb.record_failure("anthropic");
    cb.record_failure("anthropic");
    CHECK(cb.state("anthropic") == ProviderCircuitBreaker::State::Open);
    CHECK(cb.state("openai")    == ProviderCircuitBreaker::State::Closed);
    CHECK(cb.allow("openai"));
}
