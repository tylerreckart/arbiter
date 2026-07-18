// tests/test_idempotency_cache.cpp — Pins the dedup contract the
// /v1/orchestrate replay path depends on.  Specifically: tenant
// scoping prevents one tenant's keys from masking another's, repeat
// inserts of the same triple are idempotent, races on different
// request_ids report false (caller falls back to get()), and TTL
// eviction fires lazily on get() / put() as well as on prune.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "idempotency_cache.h"

#include <chrono>
#include <thread>

using namespace arbiter;

TEST_CASE("missing key returns empty optional") {
    IdempotencyCache c;
    CHECK(!c.get(1, "abc").has_value());
}

TEST_CASE("empty key is never cached") {
    IdempotencyCache c;
    CHECK(!c.put(1, "", "req-1"));
    CHECK(!c.get(1, "").has_value());
    CHECK(c.size() == 0);
}

TEST_CASE("put then get returns the same request_id") {
    IdempotencyCache c;
    CHECK(c.put(42, "abc", "req-1"));
    auto e = c.get(42, "abc");
    REQUIRE(e.has_value());
    CHECK(e->request_id == "req-1");
}

TEST_CASE("tenants are scoped independently") {
    IdempotencyCache c;
    CHECK(c.put(1, "abc", "req-A"));
    CHECK(c.put(2, "abc", "req-B"));
    CHECK(c.get(1, "abc")->request_id == "req-A");
    CHECK(c.get(2, "abc")->request_id == "req-B");
}

TEST_CASE("repeat put with same request_id is idempotent (returns true)") {
    IdempotencyCache c;
    CHECK(c.put(1, "abc", "req-1"));
    CHECK(c.put(1, "abc", "req-1"));   // same triple → ok
    CHECK(c.size() == 1);
}

TEST_CASE("racing put with a different request_id loses") {
    IdempotencyCache c;
    CHECK(c.put(1, "abc", "req-A"));
    CHECK(!c.put(1, "abc", "req-B"));  // race: caller falls back to get()
    auto e = c.get(1, "abc");
    REQUIRE(e.has_value());
    CHECK(e->request_id == "req-A");   // first put wins
}

TEST_CASE("ttl evicts on get") {
    IdempotencyCache c(std::chrono::milliseconds(50));
    CHECK(c.put(1, "abc", "req-1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    CHECK(!c.get(1, "abc").has_value());
    CHECK(c.size() == 0);
}

TEST_CASE("prune_expired removes only expired rows") {
    IdempotencyCache c(std::chrono::milliseconds(50));
    CHECK(c.put(1, "old", "req-old"));
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    CHECK(c.put(1, "fresh", "req-fresh"));
    c.prune_expired();
    CHECK(c.size() == 1);
    CHECK(c.get(1, "fresh").has_value());
    CHECK(!c.get(1, "old").has_value());
}

TEST_CASE("unique keys don't accumulate past the TTL (amortized prune)") {
    // Regression: clients mint a fresh key per request, so expired rows
    // were never revisited by get() and the table grew for the life of
    // the process.  put() now sweeps every 512 inserts.
    IdempotencyCache c(std::chrono::milliseconds(20));
    for (int i = 0; i < 600; ++i) {
        CHECK(c.put(1, "warmup-" + std::to_string(i), "req"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    // All 600 are now expired; the next amortized sweep (inside the
    // following batch of puts) must clear them.
    for (int i = 0; i < 600; ++i) {
        CHECK(c.put(1, "second-" + std::to_string(i), "req"));
    }
    CHECK(c.size() <= 600 + 512);
    CHECK(c.size() < 1200);
}
