// arbiter/src/circuit_breaker.cpp

#include "circuit_breaker.h"
#include "metrics.h"

namespace arbiter {

ProviderCircuitBreaker::Entry&
ProviderCircuitBreaker::entry_locked(const std::string& provider) {
    auto it = entries_.find(provider);
    if (it == entries_.end()) {
        it = entries_.emplace(provider, Entry{}).first;
    }
    return it->second;
}

bool ProviderCircuitBreaker::allow(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = entry_locked(provider);
    switch (e.state) {
        case State::Closed:
            return true;
        case State::Open: {
            auto now = std::chrono::steady_clock::now();
            auto since = std::chrono::duration_cast<std::chrono::seconds>(
                now - e.opened_at).count();
            if (since < cfg_.cooldown_seconds) return false;
            // Cooldown elapsed — admit this call as the half-open probe.
            e.state           = State::HalfOpen;
            e.probe_in_flight = true;
            return true;
        }
        case State::HalfOpen:
            // Already a probe in flight; reject parallel callers.  When
            // the probe completes, record_success or record_failure
            // moves us out of HalfOpen and clears probe_in_flight.
            if (e.probe_in_flight) return false;
            e.probe_in_flight = true;
            return true;
    }
    return true;
}

void ProviderCircuitBreaker::record_success(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = entry_locked(provider);
    e.consecutive_failures = 0;
    if (e.state == State::HalfOpen) {
        e.state           = State::Closed;
        e.probe_in_flight = false;
    }
}

void ProviderCircuitBreaker::record_failure(const std::string& provider) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = entry_locked(provider);
    if (e.state == State::HalfOpen) {
        // Probe failed — back to Open, restart the cooldown.
        e.state           = State::Open;
        e.opened_at       = std::chrono::steady_clock::now();
        e.probe_in_flight = false;
        if (metrics_) metrics_->inc_provider_circuit_open(provider);
        return;
    }
    e.consecutive_failures++;
    if (e.state == State::Closed &&
        e.consecutive_failures >= cfg_.failure_threshold) {
        e.state     = State::Open;
        e.opened_at = std::chrono::steady_clock::now();
        if (metrics_) metrics_->inc_provider_circuit_open(provider);
    }
}

ProviderCircuitBreaker::State
ProviderCircuitBreaker::state(const std::string& provider) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(provider);
    if (it == entries_.end()) return State::Closed;
    return it->second.state;
}

} // namespace arbiter
