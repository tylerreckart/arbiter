// tests/test_memory_entries.cpp — Unit tests for the structured memory
// store (memory_entries + memory_relations) on TenantStore.  Covers the
// SQLite-backed CRUD path that the HTTP handlers and the agent's
// /mem entries|entry|search reader sit on top of.
//
// HTTP-layer concerns (enum validation, self-loop rejection, cross-tenant
// id leak surfaces, JSON shape) are intentionally not exercised here —
// they're enforced inside api_server.cpp before the call ever reaches the
// store.  These tests pin the storage contract: tenant scoping, unique
// index, cascade delete, filter semantics.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tenant_store.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace index_ai;

namespace {

// Each TEST_CASE gets its own temp DB so they're independently runnable
// (and reorderable).  Returns a path that's valid for the lifetime of the
// returned guard's stack frame.
struct TempDb {
    fs::path path;
    TempDb() {
        const auto pid  = static_cast<long long>(::getpid());
        const auto now  = std::chrono::steady_clock::now()
                              .time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_memtest_" + std::to_string(pid) + "_" +
                std::to_string(now) + ".db");
    }
    ~TempDb() {
        std::error_code ec;
        fs::remove(path, ec);
        // SQLite WAL companions.
        fs::remove(path.string() + "-wal", ec);
        fs::remove(path.string() + "-shm", ec);
    }
};

// Convenience: make one tenant and return its id.
int64_t make_tenant(TenantStore& s, const std::string& name) {
    return s.create_tenant(name).tenant.id;
}

} // namespace

TEST_CASE("memory entry round-trip: create / get / update / delete") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto created = s.create_entry(tid, "project", "Frontend graph",
                                   "Force-directed view of memory.",
                                   "planning",
                                   R"(["scope","hub"])");
    CHECK(created.id > 0);
    CHECK(created.tenant_id == tid);
    CHECK(created.type == "project");
    CHECK(created.title == "Frontend graph");
    CHECK(created.content == "Force-directed view of memory.");
    CHECK(created.source == "planning");
    CHECK(created.tags_json == R"(["scope","hub"])");
    CHECK(created.created_at > 0);
    CHECK(created.updated_at == created.created_at);

    auto fetched = s.get_entry(tid, created.id);
    REQUIRE(fetched.has_value());
    CHECK(fetched->title == "Frontend graph");

    // PATCH a subset; other fields stay as-is.
    REQUIRE(s.update_entry(tid, created.id,
                           /*title=*/std::string("Graph hub"),
                           /*content=*/std::nullopt,
                           /*source=*/std::nullopt,
                           /*tags_json=*/std::nullopt,
                           /*type=*/std::nullopt));
    auto patched = s.get_entry(tid, created.id);
    REQUIRE(patched.has_value());
    CHECK(patched->title == "Graph hub");
    CHECK(patched->content == "Force-directed view of memory.");
    CHECK(patched->updated_at >= patched->created_at);

    // Delete returns true on success, false on second call.
    CHECK(s.delete_entry(tid, created.id));
    CHECK_FALSE(s.delete_entry(tid, created.id));
    CHECK_FALSE(s.get_entry(tid, created.id).has_value());
}

TEST_CASE("memory entries are tenant-isolated") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t a = make_tenant(s, "alice");
    const int64_t b = make_tenant(s, "bob");

    auto e_a = s.create_entry(a, "project", "Alice's note", "", "", "[]");

    // Tenant B can never read, patch, or delete tenant A's entry.
    CHECK_FALSE(s.get_entry(b, e_a.id).has_value());
    CHECK_FALSE(s.update_entry(b, e_a.id, std::string("hijack"),
                               std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    CHECK_FALSE(s.delete_entry(b, e_a.id));

    // Tenant A's view is unaffected.
    auto still = s.get_entry(a, e_a.id);
    REQUIRE(still.has_value());
    CHECK(still->title == "Alice's note");
}

TEST_CASE("memory entries: type / tag / q filters and cursor") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "filters");

    // Create a small mix.  Sleep one second between updates so updated_at
    // is strictly increasing — list_entries orders DESC, so the cursor
    // pagination test below depends on a deterministic order.
    auto e1 = s.create_entry(tid, "project",   "A", "alpha content",  "", R"(["red"])");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto e2 = s.create_entry(tid, "reference", "B", "beta content",   "", R"(["red","blue"])");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto e3 = s.create_entry(tid, "project",   "C", "gamma content",  "", R"(["blue"])");

    // No filter — newest first.
    {
        TenantStore::EntryFilter f;
        auto all = s.list_entries(tid, f);
        REQUIRE(all.size() == 3);
        CHECK(all[0].id == e3.id);
        CHECK(all[2].id == e1.id);
    }

    // Type filter.
    {
        TenantStore::EntryFilter f;
        f.types = {"project"};
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].id == e3.id);
        CHECK(rows[1].id == e1.id);
    }

    // Tag substring match — picks up rows that have "red" in their JSON.
    {
        TenantStore::EntryFilter f;
        f.tag = "red";
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 2);
    }

    // Free-text on title + content.
    {
        TenantStore::EntryFilter f;
        f.q = "gamma";
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].id == e3.id);
    }

    // Cursor: before_updated_at trims to entries strictly older.
    {
        TenantStore::EntryFilter f;
        f.before_updated_at = e3.updated_at;
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].id == e2.id);
        CHECK(rows[1].id == e1.id);
    }

    // Limit caps the page; default is 50, hard max 200.
    {
        TenantStore::EntryFilter f;
        f.limit = 1;
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].id == e3.id);
    }
}

TEST_CASE("memory relations: create / list / unique conflict / delete") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "rel");

    auto a = s.create_entry(tid, "project",   "A", "", "", "[]");
    auto b = s.create_entry(tid, "reference", "B", "", "", "[]");

    auto r = s.create_relation(tid, a.id, b.id, "supports");
    REQUIRE(r.has_value());
    CHECK(r->source_id == a.id);
    CHECK(r->target_id == b.id);
    CHECK(r->relation == "supports");

    // Unique index — second identical triple returns nullopt.
    auto dup = s.create_relation(tid, a.id, b.id, "supports");
    CHECK_FALSE(dup.has_value());
    auto found = s.find_relation(tid, a.id, b.id, "supports");
    REQUIRE(found.has_value());
    CHECK(found->id == r->id);

    // Different relation kind on the same pair is allowed.
    auto r2 = s.create_relation(tid, a.id, b.id, "refines");
    REQUIRE(r2.has_value());

    // Reversed direction is also allowed (relations are directed).
    auto r3 = s.create_relation(tid, b.id, a.id, "supports");
    REQUIRE(r3.has_value());

    // List with no filter — newest first (DESC by id).
    auto all = s.list_relations(tid, 0, 0, std::string{}, 100);
    CHECK(all.size() == 3);

    // Filter by source_id only.
    auto from_a = s.list_relations(tid, a.id, 0, std::string{}, 100);
    CHECK(from_a.size() == 2);

    // Delete one relation; others survive.
    REQUIRE(s.delete_relation(tid, r->id));
    CHECK_FALSE(s.delete_relation(tid, r->id));
    auto remaining = s.list_relations(tid, 0, 0, std::string{}, 100);
    CHECK(remaining.size() == 2);
}

TEST_CASE("deleting an entry cascades to its relations") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "cascade");

    auto a = s.create_entry(tid, "project", "A", "", "", "[]");
    auto b = s.create_entry(tid, "project", "B", "", "", "[]");
    auto c = s.create_entry(tid, "project", "C", "", "", "[]");
    REQUIRE(s.create_relation(tid, a.id, b.id, "supports").has_value());
    REQUIRE(s.create_relation(tid, c.id, a.id, "refines").has_value());
    REQUIRE(s.create_relation(tid, b.id, c.id, "supports").has_value());

    // Drop A — both edges touching A go with it; B↔C survives.
    REQUIRE(s.delete_entry(tid, a.id));
    auto rows = s.list_relations(tid, 0, 0, std::string{}, 100);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].source_id == b.id);
    CHECK(rows[0].target_id == c.id);
}

TEST_CASE("memory relations are tenant-isolated") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t a = make_tenant(s, "alice");
    const int64_t b = make_tenant(s, "bob");

    auto a1 = s.create_entry(a, "project", "Alice 1", "", "", "[]");
    auto a2 = s.create_entry(a, "project", "Alice 2", "", "", "[]");
    auto rel = s.create_relation(a, a1.id, a2.id, "supports");
    REQUIRE(rel.has_value());

    // Tenant B sees nothing in either entry list or relation list.
    CHECK(s.list_entries(b, {}).empty());
    CHECK(s.list_relations(b, 0, 0, std::string{}, 100).empty());
    CHECK_FALSE(s.find_relation(b, a1.id, a2.id, "supports").has_value());
    CHECK_FALSE(s.delete_relation(b, rel->id));

    // Tenant A still sees its own.
    CHECK(s.list_entries(a, {}).size() == 2);
    CHECK(s.list_relations(a, 0, 0, std::string{}, 100).size() == 1);
}

TEST_CASE("entries are immediately visible to reads after creation") {
    // The proposal-queue model has been retired — agents and HTTP
    // callers both write straight into the curated graph, and reads
    // surface every row they see.
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "direct-write");

    auto a = s.create_entry(tid, "project", "First",  "", "", "[]");
    auto b = s.create_entry(tid, "project", "Second", "", "", "[]");
    CHECK(a.id > 0);
    CHECK(b.id > 0);

    TenantStore::EntryFilter f;
    auto rows = s.list_entries(tid, f);
    CHECK(rows.size() == 2);
}

TEST_CASE("schema migrations are idempotent across re-opens") {
    TempDb db;
    {
        TenantStore s;
        s.open(db.path.string());
        const int64_t tid = make_tenant(s, "migrate");
        auto e = s.create_entry(tid, "project", "First", "", "", "[]");
        CHECK(e.id > 0);
    }
    // Re-open and confirm the row survives and schema is reusable.
    {
        TenantStore s;
        s.open(db.path.string());
        const auto tenants = s.list_tenants();
        REQUIRE(tenants.size() == 1);
        TenantStore::EntryFilter f;
        auto rows = s.list_entries(tenants[0].id, f);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].title == "First");
    }
}
