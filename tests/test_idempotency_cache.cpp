// tests/test_idempotency_cache.cpp — Pins the dedup contract the
// /v1/orchestrate replay path depends on.  Specifically: tenant
// scoping prevents one tenant's keys from masking another's, repeat
// inserts of the same triple are idempotent, races on different
// request_ids report false (caller falls back to get()), and TTL
// eviction fires lazily on get() / put() as well as on prune.
// With a TenantStore bound, mappings also survive a process-restart
// simulation (new cache instance, same DB).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "idempotency_cache.h"
#include "tenant_store.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

struct TempDb {
    fs::path path;
    TempDb() {
        const auto pid = static_cast<long long>(::getpid());
        const auto now = std::chrono::steady_clock::now()
                              .time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_idemp_" + std::to_string(pid) + "_" +
                std::to_string(now) + ".db");
    }
    ~TempDb() {
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path.string() + "-wal", ec);
        fs::remove(path.string() + "-shm", ec);
    }
};

} // namespace

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

TEST_CASE("durable store: mapping survives cache rebuild (restart)") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());
    const int64_t tid = store.create_tenant("acme").tenant.id;

    {
        IdempotencyCache c;
        c.bind_store(&store);
        CHECK(c.put(tid, "client-key", "req-original"));
        REQUIRE(c.get(tid, "client-key").has_value());
        CHECK(c.get(tid, "client-key")->request_id == "req-original");
    }

    // Simulate process restart: fresh L1, same SQLite file.
    IdempotencyCache after;
    after.bind_store(&store);
    CHECK(after.size() == 0);
    auto e = after.get(tid, "client-key");
    REQUIRE(e.has_value());
    CHECK(e->request_id == "req-original");
    CHECK(e->wall_created_at > 0);

    // A post-restart put with a different request_id must lose.
    CHECK(!after.put(tid, "client-key", "req-new"));
    CHECK(after.get(tid, "client-key")->request_id == "req-original");
}

TEST_CASE("durable store: L1 rehydration preserves wall-clock TTL") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());
    const int64_t tid = store.create_tenant("acme").tenant.id;
    const int64_t ttl = 2;

    // Seed a durable row that is already near expiry.
    const int64_t created = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 1;
    CHECK(store.put_idempotency_key(tid, "near-expiry", "req-old", ttl, created));

    IdempotencyCache c{std::chrono::seconds(ttl)};
    c.bind_store(&store);
    auto hit = c.get(tid, "near-expiry");
    REQUIRE(hit);
    CHECK(hit->request_id == "req-old");
    CHECK(hit->wall_created_at == created);

    // After the wall TTL elapses, L1 must not keep serving the mapping
    // just because it was rehydrated recently on the steady clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(!c.get(tid, "near-expiry").has_value());
}
