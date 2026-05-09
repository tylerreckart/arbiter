// tests/test_todos.cpp — Unit tests for the todos store (TenantStore).
// Pins the persistence contract that the /todo writ and HTTP endpoints
// depend on: tenant scoping, conversation scope semantics, position
// ordering, status transitions, and the OR-NULL conversation fallback.

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
               ("arbiter_todotest_" + std::to_string(pid) + "_" +
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
    return s.create_tenant(name).tenant.id;
}

} // namespace

TEST_CASE("todo round-trip: create / get / update / delete") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto t = s.create_todo(tid, /*conv=*/0, "research",
                            "review the deploy", "check logs + metrics");
    CHECK(t.id > 0);
    CHECK(t.tenant_id == tid);
    CHECK(t.agent_id == "research");
    CHECK(t.subject == "review the deploy");
    CHECK(t.description == "check logs + metrics");
    CHECK(t.status == "pending");
    CHECK(t.position == 1);
    CHECK(t.completed_at == 0);
    CHECK(t.created_at > 0);

    auto got = s.get_todo(tid, t.id);
    REQUIRE(got);
    CHECK(got->subject == "review the deploy");

    SUBCASE("update transitions through statuses and stamps completed_at") {
        bool ok = s.update_todo(tid, t.id, std::nullopt, std::nullopt,
            std::optional<std::string>("in_progress"),
            std::nullopt, std::nullopt);
        CHECK(ok);
        auto u = s.get_todo(tid, t.id);
        REQUIRE(u);
        CHECK(u->status == "in_progress");
        CHECK(u->completed_at == 0);   // not terminal yet

        ok = s.update_todo(tid, t.id, std::nullopt, std::nullopt,
            std::optional<std::string>("completed"),
            std::nullopt, std::nullopt);
        CHECK(ok);
        auto v = s.get_todo(tid, t.id);
        REQUIRE(v);
        CHECK(v->status == "completed");
        CHECK(v->completed_at > 0);    // auto-stamped
    }

    SUBCASE("update edits subject/description without touching status") {
        bool ok = s.update_todo(tid, t.id,
            std::optional<std::string>("new subject"),
            std::optional<std::string>("revised body"),
            std::nullopt, std::nullopt, std::nullopt);
        CHECK(ok);
        auto u = s.get_todo(tid, t.id);
        REQUIRE(u);
        CHECK(u->subject == "new subject");
        CHECK(u->description == "revised body");
        CHECK(u->status == "pending");
        CHECK(u->completed_at == 0);
    }

    SUBCASE("delete removes the row; subsequent gets miss") {
        CHECK(s.delete_todo(tid, t.id));
        CHECK(!s.get_todo(tid, t.id));
        CHECK(!s.delete_todo(tid, t.id));    // idempotent
    }
}

TEST_CASE("position increments within the pending bucket per (tenant, conv)") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto a = s.create_todo(tid, /*conv=*/0, "x", "task A", "");
    auto b = s.create_todo(tid, /*conv=*/0, "x", "task B", "");
    auto c = s.create_todo(tid, /*conv=*/0, "x", "task C", "");
    CHECK(a.position == 1);
    CHECK(b.position == 2);
    CHECK(c.position == 3);

    // Conversation 7 has its own counter.
    auto d = s.create_todo(tid, /*conv=*/7, "x", "task D", "");
    CHECK(d.position == 1);
}

TEST_CASE("list ordering: in_progress first, then pending by position") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto a = s.create_todo(tid, 0, "x", "first",  "");
    auto b = s.create_todo(tid, 0, "x", "second", "");
    auto c = s.create_todo(tid, 0, "x", "third",  "");

    // Promote the middle one.
    s.update_todo(tid, b.id, std::nullopt, std::nullopt,
        std::optional<std::string>("in_progress"),
        std::nullopt, std::nullopt);

    TenantStore::TodoFilter f;
    f.limit = 100;
    auto rows = s.list_todos(tid, f);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].subject == "second");      // in_progress floats to top
    CHECK(rows[1].subject == "first");       // pending in append order
    CHECK(rows[2].subject == "third");
}

TEST_CASE("conversation scope: positive id sees pinned + unscoped") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    s.create_todo(tid, 0, "x", "tenant-wide",  "");   // unscoped
    s.create_todo(tid, 7, "x", "conv 7 only",   "");   // pinned to 7
    s.create_todo(tid, 9, "x", "conv 9 only",   "");   // pinned to 9

    TenantStore::TodoFilter f7; f7.conversation_id = 7;
    auto a = s.list_todos(tid, f7);
    CHECK(a.size() == 2);
    bool saw_unscoped = false, saw_seven = false;
    for (auto& r : a) {
        if (r.subject == "tenant-wide") saw_unscoped = true;
        if (r.subject == "conv 7 only") saw_seven    = true;
    }
    CHECK(saw_unscoped);
    CHECK(saw_seven);

    TenantStore::TodoFilter f0;     // 0 = no filter, full list
    auto all = s.list_todos(tid, f0);
    CHECK(all.size() == 3);
}

TEST_CASE("tenant scoping: ids never leak across tenants") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t a = make_tenant(s, "alpha");
    const int64_t b = make_tenant(s, "beta");

    auto ta = s.create_todo(a, 0, "x", "alpha task", "");

    CHECK(s.get_todo(a, ta.id));
    CHECK(!s.get_todo(b, ta.id));            // cross-tenant 404 (nullopt)
    CHECK(!s.delete_todo(b, ta.id));         // cross-tenant delete is a miss
    CHECK(s.get_todo(a, ta.id));             // and original survives

    bool ok = s.update_todo(b, ta.id,
        std::optional<std::string>("hijacked"),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt);
    CHECK(!ok);
    auto still = s.get_todo(a, ta.id);
    REQUIRE(still);
    CHECK(still->subject == "alpha task");
}

TEST_CASE("filter: status + agent_id") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto a = s.create_todo(tid, 0, "research", "a", "");
    s.create_todo(tid, 0, "research", "b", "");
    s.create_todo(tid, 0, "writer",   "c", "");
    s.update_todo(tid, a.id, std::nullopt, std::nullopt,
        std::optional<std::string>("completed"),
        std::nullopt, std::nullopt);

    TenantStore::TodoFilter f1; f1.status_filter = "pending";
    auto p = s.list_todos(tid, f1);
    CHECK(p.size() == 2);

    TenantStore::TodoFilter f2; f2.agent_id_filter = "research";
    auto r = s.list_todos(tid, f2);
    CHECK(r.size() == 2);

    TenantStore::TodoFilter f3; f3.agent_id_filter = "writer";
    auto w = s.list_todos(tid, f3);
    REQUIRE(w.size() == 1);
    CHECK(w[0].subject == "c");
}
