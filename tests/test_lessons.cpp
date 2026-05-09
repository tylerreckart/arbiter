// tests/test_lessons.cpp — Unit tests for the agent-scoped lesson store.
// Pins the persistence contract that the /lesson writ + pre-turn
// injection + HTTP endpoints depend on.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tenant_store.h"

#include <chrono>
#include <filesystem>
#include <thread>

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
               ("arbiter_lessontest_" + std::to_string(pid) + "_" +
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

TEST_CASE("lesson round-trip: create / get / update / delete") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto l = s.create_lesson(tid, "research",
                              "/fetch behind cloudflare",
                              "use a header-bearing fetch instead of plain GET");
    CHECK(l.id > 0);
    CHECK(l.tenant_id == tid);
    CHECK(l.agent_id == "research");
    CHECK(l.signature == "/fetch behind cloudflare");
    CHECK(l.hit_count == 0);
    CHECK(l.created_at > 0);
    CHECK(l.last_seen_at == l.created_at);

    auto got = s.get_lesson(tid, l.id);
    REQUIRE(got);
    CHECK(got->lesson_text == "use a header-bearing fetch instead of plain GET");

    SUBCASE("update edits signature/text and bumps updated_at") {
        bool ok = s.update_lesson(tid, l.id,
            std::optional<std::string>("/fetch new sig"),
            std::optional<std::string>("revised lesson body"));
        CHECK(ok);
        auto u = s.get_lesson(tid, l.id);
        REQUIRE(u);
        CHECK(u->signature == "/fetch new sig");
        CHECK(u->lesson_text == "revised lesson body");
    }

    SUBCASE("delete removes the row") {
        CHECK(s.delete_lesson(tid, l.id));
        CHECK(!s.get_lesson(tid, l.id));
        CHECK(!s.delete_lesson(tid, l.id));     // idempotent
    }
}

TEST_CASE("agent scoping: list filters by agent_id") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    s.create_lesson(tid, "research", "sig1", "research lesson");
    s.create_lesson(tid, "research", "sig2", "another research lesson");
    s.create_lesson(tid, "writer",   "sig3", "writer lesson");

    auto r = s.list_lessons(tid, "research", 100);
    CHECK(r.size() == 2);

    auto w = s.list_lessons(tid, "writer", 100);
    REQUIRE(w.size() == 1);
    CHECK(w[0].lesson_text == "writer lesson");

    auto all = s.list_lessons(tid, "", 100);
    CHECK(all.size() == 3);
}

TEST_CASE("substring search hits signature OR lesson_text, case-insensitive") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    s.create_lesson(tid, "x", "Cloudflare blocks plain GET",
                     "use Authorization-bearing fetch");
    s.create_lesson(tid, "x", "/exec rm -rf misuse",
                     "rm -rf with no path argument is destructive");

    auto a = s.search_lessons(tid, "x", "cloudflare", 50);
    CHECK(a.size() == 1);

    auto b = s.search_lessons(tid, "x", "destructive", 50);
    CHECK(b.size() == 1);

    auto c = s.search_lessons(tid, "x", "PLAIN", 50);
    CHECK(c.size() == 1);                  // case-insensitive

    auto d = s.search_lessons(tid, "x", "no match here", 50);
    CHECK(d.empty());
}

TEST_CASE("hit bump tracks last_seen_at and ranks by hit_count") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    auto a = s.create_lesson(tid, "x", "siga", "low hits");
    auto b = s.create_lesson(tid, "x", "sigb", "high hits");
    auto c = s.create_lesson(tid, "x", "sigc", "medium hits");

    s.bump_lesson_hit(tid, b.id);
    s.bump_lesson_hit(tid, b.id);
    s.bump_lesson_hit(tid, b.id);
    s.bump_lesson_hit(tid, c.id);
    s.bump_lesson_hit(tid, c.id);
    // a stays at 0

    auto rows = s.search_lessons(tid, "x", "hits", 10);
    REQUIRE(rows.size() == 3);
    // Search orders by hit_count DESC, so b first then c then a.
    CHECK(rows[0].lesson_text == "high hits");
    CHECK(rows[1].lesson_text == "medium hits");
    CHECK(rows[2].lesson_text == "low hits");

    auto fresh = s.get_lesson(tid, b.id);
    REQUIRE(fresh);
    CHECK(fresh->hit_count == 3);
    CHECK(fresh->last_seen_at >= b.last_seen_at);
}

TEST_CASE("tenant isolation") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t a = make_tenant(s, "alpha");
    const int64_t b = make_tenant(s, "beta");

    auto la = s.create_lesson(a, "x", "sig", "alpha's lesson");

    CHECK(s.get_lesson(a, la.id));
    CHECK(!s.get_lesson(b, la.id));            // cross-tenant 404
    CHECK(!s.delete_lesson(b, la.id));
    CHECK(s.get_lesson(a, la.id));             // still there
    CHECK(s.list_lessons(b, "", 100).empty()); // beta sees no rows
}
