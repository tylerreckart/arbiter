#pragma once
// arbiter/include/circuit_breaker.h
//
// Per-provider circuit breaker.  Sits in front of the per-request
// retry loop in ApiClient and short-circuits subsequent calls when a
// provider is clearly unhealthy, so a 503 storm doesn't burn the
// retry budget on every parallel request.
//
// State machine:
//
//   ┌──────────┐  N consecutive failures  ┌──────┐
//   │  Closed  │  ───────────────────────► │ Open │
//   │ (normal) │                            └──┬───┘
//   └────▲─────┘                               │
//        │                                     │ cooldown elapsed
//        │   probe success                     ▼
//        │                                ┌──────────┐
//        └────────────────────────────────│ HalfOpen │
//                                         └────┬─────┘
//             probe failure                    │
//                ▲                              │
//                └──────────────────────────────┘
//
// Closed:   every call passes; failures accumulate a consecutive
//           counter, successes reset it to 0.
// Open:     calls reject immediately with `circuit_open`.  After the
//           cooldown elapses, the breaker flips to HalfOpen on the
//           next allow() check.
// HalfOpen: exactly one probe call is permitted at a time.  A
//           success closes the breaker; a failure reopens it.
//
// The breaker is process-wide (single shared instance owned by the
// ApiServer) so two concurrent requests against a flaky provider
// observe each other's failures and trip the breaker together.

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arbiter {

class Metrics;

struct CircuitBreakerConfig {
    // How many consecutive failures trip the breaker.  Default 5 —
    // tolerates transient hiccups, catches sustained outages.
    int failure_threshold = 5;

    // Cool-down once tripped, seconds.  After this many seconds in
    // Open, the next allow() probe transitions to HalfOpen.
    int cooldown_seconds  = 30;
};

class ProviderCircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    explicit ProviderCircuitBreaker(CircuitBreakerConfig cfg = {})
        : cfg_(cfg) {}

    // Decide whether to admit a call to `provider`.  Returns true in
    // Closed and (one probe at a time) HalfOpen.  Returns false in
    // Open until the cooldown elapses, at which point this call
    // becomes the probe (returns true, breaker → HalfOpen).
    bool allow(const std::string& provider);

    // Outcome of an admitted call.  In HalfOpen, success closes the
    // breaker, failure reopens it and restarts the cooldown.  In
    // Closed, failures accumulate; threshold-many in a row trip the
    // breaker.
    void record_success(const std::string& provider);
    void record_failure(const std::string& provider);

    // Inspection for /metrics + tests.  Returns Closed for unknown
    // providers (lazy-init on first allow/record).
    State state(const std::string& provider) const;

    // Optional metrics sink.  Increments arbiter_provider_circuit_open_total
    // when the breaker transitions to Open.
    void set_metrics(Metrics* m) { metrics_ = m; }

private:
    struct Entry {
        State                                 state = State::Closed;
        int                                   consecutive_failures = 0;
        std::chrono::steady_clock::time_point opened_at{};
        bool                                  probe_in_flight = false;
    };

    Entry& entry_locked(const std::string& provider);

    mutable std::mutex                              mu_;
    std::unordered_map<std::string, Entry>          entries_;
    CircuitBreakerConfig                            cfg_;
    Metrics*                                        metrics_ = nullptr;
};

} // namespace arbiter
