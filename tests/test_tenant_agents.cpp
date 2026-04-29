// tests/test_tenant_agents.cpp — Unit tests for the per-tenant agent
// catalog on TenantStore.  Pins the storage contract that backs:
//   POST   /v1/agents
//   GET    /v1/agents
//   GET    /v1/agents/:id
//   PATCH  /v1/agents/:id
//   DELETE /v1/agents/:id
// and the per-request orchestrator's "install stored agents on every
// /v1/orchestrate call so /agent and /parallel can resolve siblings by
// id" wiring.
//
// HTTP-layer concerns (Constitution::from_json validation, agent_id
// safety, "index" being reserved) are enforced inside api_server.cpp
// before the call ever reaches the store and aren't exercised here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tenant_store.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace index_ai;

namespace {

struct TempDb {
    fs::path path;
    TempDb() {
        const auto pid = static_cast<long long>(::getpid());
        const auto now = std::chrono::steady_clock::now()
                             .time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_agentstest_" + std::to_string(pid) + "_" +
                std::to_string(now) + ".db");
    }
    ~TempDb() {
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path.string() + "-wal", ec);
        fs::remove(path.string() + "-shm", ec);
    }
};

int64_t make_tenant(TenantStore& s, const std::string& name) {
    return s.create_tenant(name, 0).tenant.id;
}

// The store treats agent_def_json as an opaque blob — any non-empty
// string round-trips.  Tests don't need real Constitution JSON.
const char* kBlobV1 = R"({"id":"researcher","name":"Researcher","role":"researcher","model":"claude-sonnet-4-6"})";
const char* kBlobV2 = R"({"id":"researcher","name":"Researcher 2","role":"reviewer","model":"claude-haiku-4-5"})";

} // namespace

TEST_CASE("tenant_agents round-trip: create / get / list / update / delete") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto created = s.create_agent_record(tid, "researcher",
                                          "Researcher", "researcher",
                                          "claude-sonnet-4-6", kBlobV1);
    REQUIRE(created.has_value());
    CHECK(created->id > 0);
    CHECK(created->tenant_id == tid);
    CHECK(created->agent_id == "researcher");
    CHECK(created->name == "Researcher");
    CHECK(created->role == "researcher");
    CHECK(created->model == "claude-sonnet-4-6");
    CHECK(created->agent_def_json == kBlobV1);
    CHECK(created->created_at > 0);
    CHECK(created->updated_at == created->created_at);

    auto fetched = s.get_agent_record(tid, "researcher");
    REQUIRE(fetched.has_value());
    CHECK(fetched->id == created->id);
    CHECK(fetched->agent_def_json == kBlobV1);

    // Wholesale replace.  All five denormalised + blob columns swap.
    REQUIRE(s.update_agent_record(tid, "researcher",
                                   "Researcher 2", "reviewer",
                                   "claude-haiku-4-5", kBlobV2));
    auto after = s.get_agent_record(tid, "researcher");
    REQUIRE(after.has_value());
    CHECK(after->name           == "Researcher 2");
    CHECK(after->role           == "reviewer");
    CHECK(after->model          == "claude-haiku-4-5");
    CHECK(after->agent_def_json == kBlobV2);
    CHECK(after->created_at == created->created_at);  // immutable
    CHECK(after->updated_at >= created->updated_at);

    // List surfaces the row.
    auto rows = s.list_agent_records(tid, /*limit=*/50);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].agent_id == "researcher");

    REQUIRE(s.delete_agent_record(tid, "researcher"));
    CHECK_FALSE(s.get_agent_record(tid, "researcher").has_value());
    CHECK(s.list_agent_records(tid, 50).empty());
}

TEST_CASE("tenant_agents reject duplicate agent_id within a tenant") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    REQUIRE(s.create_agent_record(tid, "researcher", "R", "researcher",
                                   "claude", kBlobV1).has_value());
    auto dup = s.create_agent_record(tid, "researcher", "R-dup",
                                      "researcher", "claude", kBlobV2);
    CHECK_FALSE(dup.has_value());

    // The original is still intact — failed insert didn't overwrite.
    auto live = s.get_agent_record(tid, "researcher");
    REQUIRE(live.has_value());
    CHECK(live->name == "R");
    CHECK(live->agent_def_json == kBlobV1);
}

TEST_CASE("tenant_agents are tenant-isolated") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t a = make_tenant(s, "tenant-a");
    const int64_t b = make_tenant(s, "tenant-b");

    // Same agent_id on both tenants — must not collide.
    REQUIRE(s.create_agent_record(a, "researcher", "A", "researcher",
                                   "claude", kBlobV1).has_value());
    REQUIRE(s.create_agent_record(b, "researcher", "B", "reviewer",
                                   "haiku", kBlobV2).has_value());

    auto from_a = s.get_agent_record(a, "researcher");
    auto from_b = s.get_agent_record(b, "researcher");
    REQUIRE(from_a.has_value());
    REQUIRE(from_b.has_value());
    CHECK(from_a->name == "A");
    CHECK(from_b->name == "B");
    CHECK(from_a->id != from_b->id);

    // Cross-tenant lookups return nullopt — never another tenant's row.
    CHECK_FALSE(s.get_agent_record(a, "nope").has_value());

    // Delete on tenant a leaves tenant b's row alive.
    REQUIRE(s.delete_agent_record(a, "researcher"));
    CHECK_FALSE(s.get_agent_record(a, "researcher").has_value());
    CHECK(s.get_agent_record(b, "researcher").has_value());

    // Update across tenants does nothing — UPDATE WHERE tenant_id = ? AND
    // agent_id = ? matches zero rows when the (tenant, id) pair is wrong.
    CHECK_FALSE(s.update_agent_record(a, "researcher", "x", "x", "x", "{}"));
}

TEST_CASE("list_agent_records returns newest-updated first") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    // SQLite's now_epoch is second resolution — sleep is the cleanest
    // way to force a strict ordering without touching system clocks.
    REQUIRE(s.create_agent_record(tid, "a1", "A1", "r", "m", kBlobV1).has_value());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(s.create_agent_record(tid, "a2", "A2", "r", "m", kBlobV1).has_value());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(s.create_agent_record(tid, "a3", "A3", "r", "m", kBlobV1).has_value());

    auto rows = s.list_agent_records(tid, 50);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].agent_id == "a3");
    CHECK(rows[1].agent_id == "a2");
    CHECK(rows[2].agent_id == "a1");

    // Update bumps a1 to the front.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(s.update_agent_record(tid, "a1", "A1!", "r", "m", kBlobV2));
    rows = s.list_agent_records(tid, 50);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].agent_id == "a1");
}

TEST_CASE("list_agent_records caps limit at 200") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    REQUIRE(s.create_agent_record(tid, "only", "O", "r", "m", kBlobV1).has_value());

    // Out-of-range limits fall back to defaults — store should never
    // honor a 1M limit even if the HTTP handler fails to clamp.
    auto rows_huge = s.list_agent_records(tid, 1'000'000);
    CHECK(rows_huge.size() == 1);

    auto rows_zero = s.list_agent_records(tid, 0);
    CHECK(rows_zero.size() == 1);

    auto rows_negative = s.list_agent_records(tid, -5);
    CHECK(rows_negative.size() == 1);
}
