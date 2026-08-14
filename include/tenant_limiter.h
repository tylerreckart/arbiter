#pragma once
// arbiter/include/tenant_limiter.h
//
// Per-tenant in-flight cap + token-bucket rate limit for the expensive
// HTTP routes (orchestrate, conversation messages, agent chat, A2A
// dispatch, event ingest, advisor gate).  Catches runaway agent loops
// and prevents one tenant from starving every other tenant of provider
// quota or accept-queue slots.
//
// Design:
//
//   • A single LimiterState per tenant_id holds (a) a counter of
//     concurrent in-flight requests and (b) a token bucket with
//     `capacity` tokens and `refill_per_sec` refill rate.  Both are
//     guarded by one mutex; the lock window is microseconds.
//
//   • acquire(tenant_id) atomically (1) checks concurrent count
//     against the cap, (2) consumes one token from the bucket.
//     Returns Result::Granted on success, with an RAII Guard that
//     releases the concurrent slot on destruction.  On failure the
//     Result carries `retry_after_seconds` (largest of: time to next
//     concurrent free, time to next token).
//
//   • Defaults come from env vars (ARBITER_TENANT_MAX_CONCURRENT,
//     ARBITER_TENANT_RATE_PER_MIN, ARBITER_TENANT_RATE_BURST) when
//     ApiServerOptions is loaded; per-tenant overrides on the
//     `tenants` row supersede the defaults at acquire time.
//
//   • Disabled (max_concurrent == 0 AND rate_per_min == 0) ⇒ acquire
//     grants without inserting a State (lock is still taken to read
//     overrides).  Idle State entries (in_flight == 0 and a full
//     token bucket, or a bucket that refill would fill) are evicted
//     on release / opportunistic acquire sweeps so distinct tenant
//     IDs cannot grow `states_` without bound.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace arbiter {

class TenantStore;

struct TenantLimits {
    // 0 = unlimited.  Hard caps; surplus requests get 429.
    int max_concurrent  = 0;
    int rate_per_min    = 0;
    int burst           = 0;        // bucket capacity; defaults to rate_per_min
};

class TenantLimiter {
public:
    struct Guard {
        TenantLimiter* parent = nullptr;
        int64_t        tenant_id = 0;
        Guard() = default;
        Guard(TenantLimiter* p, int64_t t) : parent(p), tenant_id(t) {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& o) noexcept : parent(o.parent), tenant_id(o.tenant_id) {
            o.parent = nullptr; o.tenant_id = 0;
        }
        Guard& operator=(Guard&& o) noexcept {
            if (this != &o) { release(); parent = o.parent; tenant_id = o.tenant_id;
                              o.parent = nullptr; o.tenant_id = 0; }
            return *this;
        }
        ~Guard() { release(); }
        void release();
        bool active() const { return parent != nullptr; }
    };

    struct Result {
        enum class Kind { Granted, ConcurrentExceeded, RateExceeded };
        Kind   kind                = Kind::Granted;
        int    retry_after_seconds = 0;
        Guard  guard;                  // active() iff kind == Granted

        bool granted() const { return kind == Kind::Granted; }
    };

    // Defaults applied to tenants without per-tenant overrides.
    explicit TenantLimiter(TenantLimits defaults = {});

    // Per-tenant override.  Set max_concurrent / rate_per_min to 0 on a
    // field to fall back to defaults for that field; both 0 ⇒ that
    // tenant is exempt from limiting on that axis.
    void set_tenant_override(int64_t tenant_id, TenantLimits limits);
    void clear_tenant_override(int64_t tenant_id);
    TenantLimits effective(int64_t tenant_id) const;

    // The acquire path the request handler calls.  Pass tenant_id; the
    // returned Result owns a Guard that releases on destruction.
    Result acquire(int64_t tenant_id);

    // Update the defaults at runtime (e.g. when the operator reloads
    // config).  Existing per-tenant overrides are preserved.
    void set_defaults(TenantLimits d);

    // Live State entries.  Used by tests to pin idle eviction; also a
    // cheap ops signal for how many tenants currently occupy a slot.
    std::size_t tracked_tenant_count() const;

private:
    struct State {
        TenantLimits limits;
        int          in_flight    = 0;
        // Token bucket: real-valued tokens with continuous refill.
        double       tokens       = 0.0;
        std::chrono::steady_clock::time_point last_refill;
        std::chrono::steady_clock::time_point last_used;
    };

    State& state_for(int64_t tenant_id);   // lazy-init under mu_
    void   refill_locked(State& s);
    TenantLimits effective_locked(int64_t tenant_id) const;
    bool   bucket_at_capacity_locked(State& s);
    void   maybe_erase_idle_locked(std::map<int64_t, State>::iterator it);
    void   sweep_idle_locked(int64_t keep_id);

    TenantLimits             defaults_;
    mutable std::mutex       mu_;
    std::map<int64_t, State> states_;
    // Per-tenant overrides registered via admin API.  Read-write under mu_.
    std::map<int64_t, TenantLimits> overrides_;
};

// Parse the three env vars into a TenantLimits.  Missing values stay at
// the embedded defaults (0 = unlimited).
TenantLimits load_tenant_limits_from_env();

// Always-on cap for POST /v1/intent LLM classify (hybrid/llm).  Independent
// of ARBITER_TENANT_RATE_PER_MIN, which defaults to unlimited.  A 0 rate
// disables this extra bucket.
TenantLimits load_intent_llm_limits_from_env();

} // namespace arbiter
