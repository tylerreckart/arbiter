// tests/test_tenant_primary.cpp — resolve_primary_tenant kill-switch semantics.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tenant_store.h"

#include <chrono>
#include <filesystem>

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
               ("arbiter_primarytest_" + std::to_string(pid) + "_" +
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

TEST_CASE("resolve_primary_tenant: empty list returns nullopt") {
    CHECK_FALSE(resolve_primary_tenant({}).has_value());
}

TEST_CASE("resolve_primary_tenant: picks lowest enabled id") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());

    const int64_t t1 = store.create_tenant("alpha").tenant.id;
    const int64_t t2 = store.create_tenant("beta").tenant.id;
    REQUIRE(t1 < t2);

    auto primary = resolve_primary_tenant(store.list_tenants());
    REQUIRE(primary.has_value());
    CHECK(primary->id == t1);
}

TEST_CASE("resolve_primary_tenant: skips disabled tenants") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());

    const int64_t t1 = store.create_tenant("alpha").tenant.id;
    const int64_t t2 = store.create_tenant("beta").tenant.id;
    REQUIRE(t1 < t2);
    REQUIRE(store.set_disabled(std::to_string(t1), true));

    auto primary = resolve_primary_tenant(store.list_tenants());
    REQUIRE(primary.has_value());
    CHECK(primary->id == t2);
    CHECK_FALSE(primary->disabled);
}

TEST_CASE("resolve_primary_tenant: all disabled returns nullopt") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());

    const int64_t t1 = store.create_tenant("alpha").tenant.id;
    REQUIRE(store.set_disabled(std::to_string(t1), true));

    CHECK_FALSE(resolve_primary_tenant(store.list_tenants()).has_value());
}

TEST_CASE("resolve_primary_tenant: re-enable restores lowest id") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());

    const int64_t t1 = store.create_tenant("alpha").tenant.id;
    const int64_t t2 = store.create_tenant("beta").tenant.id;
    REQUIRE(store.set_disabled(std::to_string(t1), true));
    REQUIRE(store.set_disabled(std::to_string(t1), false));

    auto primary = resolve_primary_tenant(store.list_tenants());
    REQUIRE(primary.has_value());
    CHECK(primary->id == t1);
}

// Issue #90: --disable-tenant must survive API restart and block traffic.
TEST_CASE("tenant kill-switch: disabled flag persists across store reopen") {
    TempDb db;
    {
        TenantStore store;
        store.open(db.path.string());
        const int64_t tid = store.create_tenant("primary").tenant.id;
        REQUIRE(store.set_disabled(std::to_string(tid), true));
    }
    {
        TenantStore store;
        store.open(db.path.string());
        auto tenants = store.list_tenants();
        REQUIRE(tenants.size() == 1);
        CHECK(tenants[0].disabled);
        CHECK_FALSE(resolve_primary_tenant(tenants).has_value());
    }
}

TEST_CASE("tenant kill-switch: find_by_token rejects disabled tenant") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());

    const auto created = store.create_tenant("primary");
    const std::string token = created.token;
    REQUIRE(store.set_disabled(std::to_string(created.tenant.id), true));

    CHECK_FALSE(store.find_by_token(token).has_value());
}

TEST_CASE("find_by_token: isolates tenants by API key") {
    TempDb db;
    TenantStore store;
    store.open(db.path.string());

    const auto a = store.create_tenant("alice");
    const auto b = store.create_tenant("bob");
    REQUIRE(a.token != b.token);

    auto ta = store.find_by_token(a.token);
    auto tb = store.find_by_token(b.token);
    REQUIRE(ta);
    REQUIRE(tb);
    CHECK(ta->id == a.tenant.id);
    CHECK(tb->id == b.tenant.id);
    CHECK(ta->name == "alice");
    CHECK(tb->name == "bob");

    CHECK_FALSE(store.find_by_token("").has_value());
    CHECK_FALSE(store.find_by_token("atr_not_a_real_token").has_value());
    CHECK_FALSE(store.find_by_token(a.token + "x").has_value());
}
