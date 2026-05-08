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

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using namespace arbiter;

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

// ─── FTS5 + BM25 ranking ────────────────────────────────────────────────────
//
// The search path of list_entries (when EntryFilter::q is set) goes through
// SQLite's FTS5 index with bm25() ranking, plus type/tag score multipliers.
// These tests pin the *ordering* contract; substring-only or filter-only
// behaviour is covered by the test cases above.

TEST_CASE("BM25 ranks title-match above content-only match") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "fts");

    // Two entries — same query token ("kubernetes") appears in different
    // fields.  Title is a high-information field per token; the BM25
    // weights (10.0 for title, 4.0 for content) should put the title-
    // match above the body-only match regardless of insert order.
    auto body_hit = s.create_entry(tid, "reference",
        "Generic notes",
        "Some long preamble that mentions kubernetes once and then "
        "drifts off into other deployment topics for a while.",
        "", "[]");
    auto title_hit = s.create_entry(tid, "reference",
        "Kubernetes deployment runbook",
        "Notes on rollout strategy.",
        "", "[]");

    TenantStore::EntryFilter f;
    f.q = "kubernetes";
    auto rows = s.list_entries(tid, f);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == title_hit.id);
    CHECK(rows[1].id == body_hit.id);
}

TEST_CASE("type acts as a boost, not a hard filter, when q is set") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "fts-type");

    // Same query word in identical positions across two entries with
    // different `type`.  Without the type boost they'd tie (or order
    // arbitrarily).  With a `types=["project"]` boost, the project row
    // must come first, *and* the reference row must still appear —
    // that's the "signal not gate" property.
    auto ref = s.create_entry(tid, "reference",
        "Auth notes", "Discussion of auth flow nuances.", "", "[]");
    auto proj = s.create_entry(tid, "project",
        "Auth flow", "Project plan covering the auth flow.", "", "[]");

    TenantStore::EntryFilter f;
    f.q     = "auth";
    f.types = {"project"};
    auto rows = s.list_entries(tid, f);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == proj.id);
    CHECK(rows[1].id == ref.id);
}

TEST_CASE("tag match boosts rank without excluding non-matching rows") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "fts-tag");

    // Same content, only difference is the tags array.  Tag-match boost
    // should reorder them.
    auto plain = s.create_entry(tid, "project",
        "Cache writeback", "Notes on cache writeback semantics.",
        "", R"(["misc"])");
    auto tagged = s.create_entry(tid, "project",
        "Cache writeback details", "Notes on cache writeback edge cases.",
        "", R"(["urgent","ops"])");

    TenantStore::EntryFilter f;
    f.q   = "writeback";
    f.tag = "urgent";
    auto rows = s.list_entries(tid, f);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == tagged.id);  // boosted
    CHECK(rows[1].id == plain.id);   // present, just lower
}

TEST_CASE("FTS triggers keep the index in sync on insert / update / delete") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "fts-sync");

    // Insert: searchable immediately.
    auto e = s.create_entry(tid, "project", "Original title",
        "Body mentions raptor.", "", "[]");
    {
        TenantStore::EntryFilter f;
        f.q = "raptor";
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].id == e.id);
    }

    // Update: new title indexed; old title no longer matches.
    REQUIRE(s.update_entry(tid, e.id,
        /*title=*/std::string("Renamed phoenix"),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    {
        TenantStore::EntryFilter f;
        f.q = "phoenix";
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].id == e.id);
    }
    {
        TenantStore::EntryFilter f;
        f.q = "original";
        auto rows = s.list_entries(tid, f);
        // "Original" was the old title; after the update the FTS index
        // no longer holds it.  "raptor" is still in the body, but "original"
        // is gone.
        CHECK(rows.empty());
    }

    // Delete: gone from the index.
    REQUIRE(s.delete_entry(tid, e.id));
    {
        TenantStore::EntryFilter f;
        f.q = "raptor";
        auto rows = s.list_entries(tid, f);
        CHECK(rows.empty());
    }
}

TEST_CASE("FTS index is rebuilt on first open after a stale-index migration") {
    TempDb db;

    // 1) Populate normally, then simulate the pre-FTS-migration state
    //    by dropping the FTS table and resetting PRAGMA user_version
    //    back to 0.  An older DB literally has neither — that's the
    //    state on first open after the migration ships.
    {
        TenantStore s;
        s.open(db.path.string());
        const int64_t tid = make_tenant(s, "rebuild");
        s.create_entry(tid, "project", "Quokka kingdom",
            "Notes on quokka habitats.", "", "[]");
        s.create_entry(tid, "reference", "Habitat survey",
            "Body mentions quokka twice.  quokka quokka.", "", "[]");

        // No public DB-handle accessor; use a fresh connection to issue
        // the maintenance commands.  Drop the triggers as well as the
        // table — a true pre-migration DB has neither, and leftover
        // triggers referencing a missing virtual table would cause the
        // backfill UPDATE on memory_entries to fail before the rebuild
        // guard ever runs.
        sqlite3* raw_handle = nullptr;
        REQUIRE(sqlite3_open(db.path.string().c_str(), &raw_handle) == SQLITE_OK);
        char* err = nullptr;
        REQUIRE(sqlite3_exec(raw_handle,
            "DROP TRIGGER IF EXISTS memory_entries_fts_ai;"
            "DROP TRIGGER IF EXISTS memory_entries_fts_ad;"
            "DROP TRIGGER IF EXISTS memory_entries_fts_au;"
            "DROP TABLE   IF EXISTS memory_entries_fts;"
            "PRAGMA user_version = 0;",
            nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_close(raw_handle);
    }

    // 2) Re-open through the normal path — the migration recreates the
    //    FTS table, the user_version<1 guard fires the rebuild, and the
    //    pre-existing rows become searchable.
    {
        TenantStore s;
        s.open(db.path.string());
        const auto tenants = s.list_tenants();
        REQUIRE(tenants.size() == 1);
        TenantStore::EntryFilter f;
        f.q = "quokka";
        auto rows = s.list_entries(tenants[0].id, f);
        REQUIRE(rows.size() == 2);  // both pre-existing rows are searchable
    }
}

// ─── Temporal validity windows ─────────────────────────────────────────────
//
// Soft-delete via invalidate_entry; default reads filter to active rows;
// EntryFilter::as_of selects historical state.  Hard delete via
// delete_entry continues to cascade (covered by the existing
// "deleting an entry cascades to its relations" test).

TEST_CASE("invalidated entries are hidden from default reads") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "temporal");

    auto e = s.create_entry(tid, "project", "Stale fact",
        "Once was true, no longer.", "", "[]");

    // Active before invalidation: visible via list, get, search.
    REQUIRE(s.get_entry(tid, e.id).has_value());
    {
        TenantStore::EntryFilter f;
        CHECK(s.list_entries(tid, f).size() == 1);
        f.q = "stale";
        CHECK(s.list_entries(tid, f).size() == 1);
    }

    // Invalidate at created_at + 1000 so as_of windowing in the next
    // test is deterministic relative to the entry's own clock.
    const int64_t valid_until = e.created_at + 1000;
    REQUIRE(s.invalidate_entry(tid, e.id, valid_until));

    // Hidden from default reads: get returns None, list returns empty,
    // search returns empty.
    CHECK_FALSE(s.get_entry(tid, e.id).has_value());
    {
        TenantStore::EntryFilter f;
        CHECK(s.list_entries(tid, f).empty());
        f.q = "stale";
        CHECK(s.list_entries(tid, f).empty());
    }
}

TEST_CASE("as_of restores rows that were active at the given timestamp") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "as-of");

    auto e = s.create_entry(tid, "project", "Vanishing fact",
        "Body of the fact.", "", "[]");

    // Use timestamps relative to the entry's own creation time so the
    // test doesn't depend on wall-clock.  valid window is then
    // [created_at, created_at + 1000).
    const int64_t valid_until = e.created_at + 1000;
    REQUIRE(s.invalidate_entry(tid, e.id, valid_until));

    // as_of inside the window → entry is visible.
    {
        TenantStore::EntryFilter f;
        f.as_of = e.created_at + 500;
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].id == e.id);
    }
    // as_of at the exact invalidation moment → NOT in the window
    // (valid_to > as_of is required, not valid_to >= as_of).
    {
        TenantStore::EntryFilter f;
        f.as_of = valid_until;
        auto rows = s.list_entries(tid, f);
        CHECK(rows.empty());
    }
    // Same temporal contract on the FTS search path.
    {
        TenantStore::EntryFilter f;
        f.as_of = e.created_at + 500;
        f.q = "vanishing";
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 1);
    }
}

TEST_CASE("invalidate_entry rejects unknown ids and double-invalidate") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "reject");

    // Unknown id.
    CHECK_FALSE(s.invalidate_entry(tid, /*id=*/9999));

    auto e = s.create_entry(tid, "project", "Once", "Body.", "", "[]");
    REQUIRE(s.invalidate_entry(tid, e.id));
    // Already invalidated → idempotent rejection (false), valid_to from
    // the first call stays put.
    CHECK_FALSE(s.invalidate_entry(tid, e.id));

    // Cross-tenant: another tenant can't invalidate someone else's row.
    const int64_t other = make_tenant(s, "other");
    auto e2 = s.create_entry(tid, "project", "Mine", "Mine.", "", "[]");
    CHECK_FALSE(s.invalidate_entry(other, e2.id));
    // Original tenant still sees it as active.
    CHECK(s.get_entry(tid, e2.id).has_value());
}

TEST_CASE("update_entry refuses to mutate invalidated rows") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "update-block");

    auto e = s.create_entry(tid, "project", "Before", "Body.", "", "[]");
    REQUIRE(s.invalidate_entry(tid, e.id));

    // Editing an invalidated entry returns false — historical records
    // are immutable through this path.  Caller can hard-delete + re-create
    // if they really need the change.
    CHECK_FALSE(s.update_entry(tid, e.id,
        /*title=*/std::string("After"),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt));
}

// ─── Conversation scoping + graduated search ───────────────────────────────
//
// Conversation_id pins entries to one conversation; reads filter with an
// OR-NULL fallback so unscoped (pre-migration / cross-conversation) rows
// stay visible everywhere.  search_entries_graduated layers a
// conversation-first ordering on top of FTS5 ranking.

TEST_CASE("conversation_id filter includes unscoped rows (OR-NULL fallback)") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "scope-fallback");
    auto conv = s.create_conversation(tid, "thread-1", "index", "");

    // One entry pinned to the conversation; one unscoped (pre-migration
    // analog); one pinned to a *different* conversation.
    auto pinned = s.create_entry(tid, "project", "scoped fact",
        "Body of scoped fact.", "", "[]", /*artifact=*/0,
        /*conversation_id=*/conv.id);
    auto unscoped = s.create_entry(tid, "project", "global fact",
        "Body of global fact.", "", "[]", /*artifact=*/0,
        /*conversation_id=*/0);
    auto other_conv = s.create_conversation(tid, "thread-2", "index", "");
    auto other = s.create_entry(tid, "project", "other thread fact",
        "Body of other thread fact.", "", "[]", /*artifact=*/0,
        /*conversation_id=*/other_conv.id);

    // Conversation-scoped browse: the pinned and the unscoped row
    // appear; the other-conversation row is filtered out.
    {
        TenantStore::EntryFilter f;
        f.conversation_id = conv.id;
        auto rows = s.list_entries(tid, f);
        REQUIRE(rows.size() == 2);
        bool saw_pinned   = false;
        bool saw_unscoped = false;
        bool saw_other    = false;
        for (auto& r : rows) {
            if (r.id == pinned.id)   saw_pinned = true;
            if (r.id == unscoped.id) saw_unscoped = true;
            if (r.id == other.id)    saw_other = true;
        }
        CHECK(saw_pinned);
        CHECK(saw_unscoped);
        CHECK_FALSE(saw_other);
    }

    // No conversation filter ⇒ all three rows.
    {
        TenantStore::EntryFilter f;
        auto rows = s.list_entries(tid, f);
        CHECK(rows.size() == 3);
    }
}

TEST_CASE("graduated search prefers conversation hits, fills from tenant-wide") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "graduated");
    auto conv = s.create_conversation(tid, "active", "index", "");

    // Two conversation-pinned hits and two tenant-wide hits all match
    // the same query word.  Graduated search puts the conversation hits
    // first regardless of BM25 score against the unscoped peers.
    auto wide_a = s.create_entry(tid, "project", "raptor flight survey",
        "Notes on raptor flight patterns.", "", "[]");
    auto wide_b = s.create_entry(tid, "reference", "raptor field guide",
        "Reference: raptor identification by silhouette.", "", "[]");
    auto pinned_a = s.create_entry(tid, "project", "raptor migration",
        "Pinned: raptor migration corridor analysis.", "", "[]",
        /*artifact=*/0, conv.id);
    auto pinned_b = s.create_entry(tid, "learning", "raptor handling",
        "Pinned: raptor handling protocols.", "", "[]",
        /*artifact=*/0, conv.id);

    TenantStore::EntryFilter f;
    f.q               = "raptor";
    f.conversation_id = conv.id;
    f.limit           = 10;

    auto rows = s.search_entries_graduated(tid, f);
    REQUIRE(rows.size() == 4);

    // First two slots must be the conversation-pinned rows (locality
    // bias).  Order between the two pinned rows depends on BM25; we
    // don't pin a specific permutation, only the first-2 / last-2 split.
    auto is_pinned = [&](int64_t id) {
        return id == pinned_a.id || id == pinned_b.id;
    };
    CHECK(is_pinned(rows[0].id));
    CHECK(is_pinned(rows[1].id));
    CHECK_FALSE(is_pinned(rows[2].id));
    CHECK_FALSE(is_pinned(rows[3].id));

    // No duplicates — pinned rows surface in pass 1 (with OR-NULL
    // fallback they'd reappear in pass 2 too if dedup were broken).
    std::set<int64_t> seen;
    for (auto& r : rows) seen.insert(r.id);
    CHECK(seen.size() == rows.size());
}

TEST_CASE("graduated search collapses to single-pass when no conversation context") {
    TempDb db;
    TenantStore s;
    s.open(db.path.string());
    const int64_t tid = make_tenant(s, "no-context");

    auto a = s.create_entry(tid, "project", "alpha entry",
        "alpha alpha alpha.", "", "[]");
    auto b = s.create_entry(tid, "project", "beta entry",
        "alpha mention.", "", "[]");
    (void)a; (void)b;

    TenantStore::EntryFilter f;
    f.q = "alpha";
    // f.conversation_id stays 0 → single tenant-wide pass.
    auto rows = s.search_entries_graduated(tid, f);
    CHECK(rows.size() == 2);
}
