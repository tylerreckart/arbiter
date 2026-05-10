// arbiter/src/tenant_limiter.cpp

#include "tenant_limiter.h"

#include <algorithm>
#include <cstdlib>

namespace arbiter {

namespace {

int env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

} // namespace

TenantLimits load_tenant_limits_from_env() {
    TenantLimits d;
    d.max_concurrent = env_int("ARBITER_TENANT_MAX_CONCURRENT", 0);
    d.rate_per_min   = env_int("ARBITER_TENANT_RATE_PER_MIN",   0);
    d.burst          = env_int("ARBITER_TENANT_RATE_BURST",     0);
    if (d.burst <= 0) d.burst = d.rate_per_min;
    return d;
}

void TenantLimiter::Guard::release() {
    if (!parent) return;
    {
        std::lock_guard<std::mutex> lk(parent->mu_);
        auto it = parent->states_.find(tenant_id);
        if (it != parent->states_.end() && it->second.in_flight > 0) {
            --it->second.in_flight;
        }
    }
    parent = nullptr;
    tenant_id = 0;
}

TenantLimiter::TenantLimiter(TenantLimits defaults) : defaults_(defaults) {
    if (defaults_.burst <= 0) defaults_.burst = defaults_.rate_per_min;
}

void TenantLimiter::set_defaults(TenantLimits d) {
    if (d.burst <= 0) d.burst = d.rate_per_min;
    std::lock_guard<std::mutex> lk(mu_);
    defaults_ = d;
}

void TenantLimiter::set_tenant_override(int64_t tenant_id, TenantLimits limits) {
    std::lock_guard<std::mutex> lk(mu_);
    overrides_[tenant_id] = limits;
    // Push the override into the live state so subsequent acquires use it.
    auto it = states_.find(tenant_id);
    if (it != states_.end()) {
        TenantLimits eff = limits;
        if (eff.max_concurrent <= 0) eff.max_concurrent = defaults_.max_concurrent;
        if (eff.rate_per_min   <= 0) eff.rate_per_min   = defaults_.rate_per_min;
        if (eff.burst          <= 0) eff.burst          = eff.rate_per_min;
        it->second.limits = eff;
    }
}

void TenantLimiter::clear_tenant_override(int64_t tenant_id) {
    std::lock_guard<std::mutex> lk(mu_);
    overrides_.erase(tenant_id);
    auto it = states_.find(tenant_id);
    if (it != states_.end()) it->second.limits = defaults_;
}

TenantLimits TenantLimiter::effective(int64_t tenant_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto ov = overrides_.find(tenant_id);
    if (ov == overrides_.end()) return defaults_;
    TenantLimits eff = ov->second;
    if (eff.max_concurrent <= 0) eff.max_concurrent = defaults_.max_concurrent;
    if (eff.rate_per_min   <= 0) eff.rate_per_min   = defaults_.rate_per_min;
    if (eff.burst          <= 0) eff.burst          = eff.rate_per_min;
    return eff;
}

TenantLimiter::State& TenantLimiter::state_for(int64_t tenant_id) {
    auto it = states_.find(tenant_id);
    if (it != states_.end()) return it->second;

    State s;
    auto ov = overrides_.find(tenant_id);
    s.limits = (ov != overrides_.end()) ? ov->second : defaults_;
    if (s.limits.max_concurrent <= 0) s.limits.max_concurrent = defaults_.max_concurrent;
    if (s.limits.rate_per_min   <= 0) s.limits.rate_per_min   = defaults_.rate_per_min;
    if (s.limits.burst          <= 0) s.limits.burst          = s.limits.rate_per_min;
    s.tokens     = static_cast<double>(s.limits.burst);
    s.last_refill = std::chrono::steady_clock::now();

    auto [ins, _] = states_.emplace(tenant_id, std::move(s));
    return ins->second;
}

void TenantLimiter::refill_locked(State& s) {
    if (s.limits.rate_per_min <= 0) {
        s.tokens = static_cast<double>(s.limits.burst > 0
                                            ? s.limits.burst : 0);
        return;
    }
    auto now = std::chrono::steady_clock::now();
    double elapsed_s =
        std::chrono::duration<double>(now - s.last_refill).count();
    if (elapsed_s <= 0) return;
    double per_sec = static_cast<double>(s.limits.rate_per_min) / 60.0;
    double cap     = static_cast<double>(s.limits.burst > 0
                                              ? s.limits.burst
                                              : s.limits.rate_per_min);
    s.tokens = std::min(cap, s.tokens + elapsed_s * per_sec);
    s.last_refill = now;
}

TenantLimiter::Result TenantLimiter::acquire(int64_t tenant_id) {
    Result r;
    std::lock_guard<std::mutex> lk(mu_);

    State& s = state_for(tenant_id);

    // Concurrent cap.  Skipped when limit==0 (unlimited).
    if (s.limits.max_concurrent > 0 &&
        s.in_flight >= s.limits.max_concurrent) {
        r.kind = Result::Kind::ConcurrentExceeded;
        r.retry_after_seconds = 1;   // best-guess; in-flights typically clear within
                                     // single-digit seconds for orchestrate calls
        return r;
    }

    // Token bucket.  Skipped when rate==0.
    if (s.limits.rate_per_min > 0) {
        refill_locked(s);
        if (s.tokens < 1.0) {
            // Compute how many seconds until at least one token.
            double per_sec = static_cast<double>(s.limits.rate_per_min) / 60.0;
            double need    = 1.0 - s.tokens;
            int wait_s = static_cast<int>(need / per_sec) + 1;
            if (wait_s < 1) wait_s = 1;
            r.kind = Result::Kind::RateExceeded;
            r.retry_after_seconds = wait_s;
            return r;
        }
        s.tokens -= 1.0;
    }

    ++s.in_flight;
    r.kind  = Result::Kind::Granted;
    r.guard = Guard(this, tenant_id);
    return r;
}

} // namespace arbiter
