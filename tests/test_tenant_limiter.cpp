// tests/test_tenant_limiter.cpp — Unit tests for the per-tenant
// concurrent + rate limiter.  Pins the contract that the API server
// dispatch sites depend on: 0-config means unlimited (zero overhead),
// concurrent caps fail closed with a Retry-After hint, the token
// bucket refills over wall-clock seconds, per-tenant overrides
// supersede defaults but a 0 in any field falls back to defaults.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tenant_limiter.h"

#include <thread>
#include <chrono>

using namespace arbiter;

TEST_CASE("zero defaults grant every acquire") {
    TenantLimiter lim;   // both fields = 0 → unlimited
    for (int i = 0; i < 100; ++i) {
        auto r = lim.acquire(/*tenant_id=*/1);
        CHECK(r.granted());
    }
    // Outstanding guards from the loop above all destructed → no leaks.
}

TEST_CASE("concurrent cap rejects surplus with retry hint") {
    TenantLimits d;
    d.max_concurrent = 2;
    TenantLimiter lim(d);

    auto a = lim.acquire(7);
    auto b = lim.acquire(7);
    CHECK(a.granted());
    CHECK(b.granted());

    auto c = lim.acquire(7);
    CHECK(!c.granted());
    CHECK(c.kind == TenantLimiter::Result::Kind::ConcurrentExceeded);
    CHECK(c.retry_after_seconds >= 1);

    // Releasing one frees the slot.
    a.guard.release();
    auto d2 = lim.acquire(7);
    CHECK(d2.granted());
}

TEST_CASE("guard releases on destruction") {
    TenantLimits d;
    d.max_concurrent = 1;
    TenantLimiter lim(d);

    {
        auto a = lim.acquire(11);
        CHECK(a.granted());
        auto b = lim.acquire(11);
        CHECK(!b.granted());
    } // a's destructor releases.

    auto c = lim.acquire(11);
    CHECK(c.granted());
}

TEST_CASE("token bucket: burst granted, then surplus rejected, then refill") {
    TenantLimits d;
    d.rate_per_min = 60;     // 1/sec
    d.burst        = 3;
    TenantLimiter lim(d);

    auto r1 = lim.acquire(1);
    auto r2 = lim.acquire(1);
    auto r3 = lim.acquire(1);
    CHECK(r1.granted());
    CHECK(r2.granted());
    CHECK(r3.granted());
    // Release immediately so concurrency cap (unlimited here) doesn't
    // confound the test — only the rate bucket is consumed.
    r1.guard.release();
    r2.guard.release();
    r3.guard.release();

    auto r4 = lim.acquire(1);
    CHECK(!r4.granted());
    CHECK(r4.kind == TenantLimiter::Result::Kind::RateExceeded);
    CHECK(r4.retry_after_seconds >= 1);

    // Wait for one second of refill — at 1 token/sec we expect roughly
    // one available.  Sleep just over a second to absorb scheduler jitter.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto r5 = lim.acquire(1);
    CHECK(r5.granted());
}

TEST_CASE("per-tenant override only affects that tenant") {
    TenantLimits d;
    d.max_concurrent = 5;
    TenantLimiter lim(d);

    TenantLimits ov;
    ov.max_concurrent = 1;       // tenant 7: stricter
    lim.set_tenant_override(7, ov);

    auto a = lim.acquire(7);
    auto b = lim.acquire(7);
    CHECK(a.granted());
    CHECK(!b.granted());

    // Tenant 8 still uses the looser default.
    auto x = lim.acquire(8);
    auto y = lim.acquire(8);
    CHECK(x.granted());
    CHECK(y.granted());
}

TEST_CASE("tenants are isolated from each other") {
    TenantLimits d;
    d.max_concurrent = 1;
    TenantLimiter lim(d);

    auto a = lim.acquire(1);
    auto b = lim.acquire(2);     // different tenant — has its own slot
    CHECK(a.granted());
    CHECK(b.granted());
}

TEST_CASE("clear_tenant_override falls back to defaults") {
    TenantLimits d;
    d.max_concurrent = 5;
    TenantLimiter lim(d);

    TenantLimits ov;
    ov.max_concurrent = 1;
    lim.set_tenant_override(9, ov);
    CHECK(lim.effective(9).max_concurrent == 1);

    lim.clear_tenant_override(9);
    CHECK(lim.effective(9).max_concurrent == 5);
}

TEST_CASE("per-tenant override with 0 field inherits the default") {
    TenantLimits d;
    d.max_concurrent = 10;
    d.rate_per_min   = 60;
    TenantLimiter lim(d);

    TenantLimits ov;
    ov.max_concurrent = 0;       // inherit default (10)
    ov.rate_per_min   = 30;      // override (30/min)
    lim.set_tenant_override(3, ov);

    auto eff = lim.effective(3);
    CHECK(eff.max_concurrent == 10);
    CHECK(eff.rate_per_min   == 30);
}
