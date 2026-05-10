// tests/test_request_log.cpp — Unit tests for the durable request log
// (request_status + request_events).  Pins the contract that the SSE
// writer + resubscribe handler depend on: tenant scoping, monotonic
// seq, since_seq replay slice, recovery sweep behavior.

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
               ("arbiter_reqtest_" + std::to_string(pid) + "_" +
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

TEST_CASE("request_status round-trip: create / get / update / list") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    s.create_request_status(tid, "req-001", "research",
                             /*conversation_id=*/0, /*started_at=*/1000);

    auto got = s.get_request_status(tid, "req-001");
    REQUIRE(got);
    CHECK(got->state == "running");
    CHECK(got->agent_id == "research");
    CHECK(got->started_at == 1000);
    CHECK(got->completed_at == 0);

    SUBCASE("update transitions to terminal") {
        bool ok = s.update_request_status("req-001",
            std::optional<std::string>("completed"),
            std::optional<int64_t>(1500),
            std::nullopt,
            std::optional<int64_t>(42));
        CHECK(ok);
        auto u = s.get_request_status(tid, "req-001");
        REQUIRE(u);
        CHECK(u->state == "completed");
        CHECK(u->completed_at == 1500);
        CHECK(u->last_seq == 42);
    }

    SUBCASE("list orders by started_at DESC") {
        s.create_request_status(tid, "req-002", "writer", 0, 2000);
        s.create_request_status(tid, "req-003", "writer", 0, 1500);
        auto rows = s.list_request_status(tid, 100);
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].request_id == "req-002");   // newest first
        CHECK(rows[1].request_id == "req-003");
        CHECK(rows[2].request_id == "req-001");
    }
}

TEST_CASE("request_events: append + replay slice + monotonic seq") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    s.create_request_status(tid, "req-A", "x", 0, 1000);

    s.append_request_event(tid, "req-A", 1, "request_received",
                            R"({"ts":1})", 100);
    s.append_request_event(tid, "req-A", 2, "stream_start",
                            R"({"agent":"x"})", 110);
    s.append_request_event(tid, "req-A", 3, "text",
                            R"({"delta":"hello"})", 120);
    s.append_request_event(tid, "req-A", 4, "done",
                            R"({"ok":true})", 200);

    SUBCASE("from seq 0: full replay") {
        auto events = s.list_request_events(tid, "req-A", 0, 1000);
        REQUIRE(events.size() == 4);
        CHECK(events[0].seq == 1);
        CHECK(events[0].event_kind == "request_received");
        CHECK(events[3].seq == 4);
        CHECK(events[3].event_kind == "done");
    }

    SUBCASE("since_seq slice") {
        auto events = s.list_request_events(tid, "req-A", 2, 1000);
        REQUIRE(events.size() == 2);
        CHECK(events[0].seq == 3);
        CHECK(events[1].seq == 4);
    }

    SUBCASE("limit caps the result") {
        auto events = s.list_request_events(tid, "req-A", 0, 2);
        CHECK(events.size() == 2);
        CHECK(events[0].seq == 1);
        CHECK(events[1].seq == 2);
    }
}

TEST_CASE("duplicate seq throws (unique index)") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    s.create_request_status(tid, "req-X", "x", 0, 1000);

    s.append_request_event(tid, "req-X", 1, "text", "{}", 0);
    CHECK_THROWS(s.append_request_event(tid, "req-X", 1, "text", "{}", 0));
}

TEST_CASE("recovery sweep: marks orphaned running rows failed") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");

    s.create_request_status(tid, "orphan-1", "x", 0, 1000);
    s.create_request_status(tid, "orphan-2", "x", 0, 1100);
    s.create_request_status(tid, "settled",  "x", 0, 1200);
    s.update_request_status("settled",
        std::optional<std::string>("completed"),
        std::optional<int64_t>(1300),
        std::nullopt, std::nullopt);

    auto orphans = s.recover_running_requests("failed", 9999, "interrupted");
    CHECK(orphans.size() == 2);
    bool saw1 = false, saw2 = false;
    for (auto& id : orphans) {
        if (id == "orphan-1") saw1 = true;
        if (id == "orphan-2") saw2 = true;
    }
    CHECK(saw1);
    CHECK(saw2);

    auto u1 = s.get_request_status(tid, "orphan-1");
    REQUIRE(u1);
    CHECK(u1->state == "failed");
    CHECK(u1->completed_at == 9999);
    CHECK(u1->error_message == "interrupted");

    // Already-terminal rows are untouched.
    auto se = s.get_request_status(tid, "settled");
    REQUIRE(se);
    CHECK(se->state == "completed");
    CHECK(se->completed_at == 1300);

    SUBCASE("recovery is idempotent — second sweep finds nothing") {
        auto more = s.recover_running_requests("failed", 10000, "again");
        CHECK(more.empty());
    }
}

TEST_CASE("tenant isolation: events never cross") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t a = make_tenant(s, "alpha");
    const int64_t b = make_tenant(s, "beta");

    s.create_request_status(a, "req-A", "x", 0, 1000);
    s.append_request_event(a, "req-A", 1, "text", "{}", 0);

    CHECK(s.get_request_status(a, "req-A"));
    CHECK(!s.get_request_status(b, "req-A"));
    CHECK(s.list_request_events(b, "req-A", 0, 100).empty());
}

TEST_CASE("event seqs preserve insert order on read") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    s.create_request_status(tid, "req-O", "x", 0, 1000);

    // Interleaved seq numbers — non-monotonic insert order — should
    // still come back in seq-asc order.
    s.append_request_event(tid, "req-O", 5, "text", "{}", 0);
    s.append_request_event(tid, "req-O", 1, "text", "{}", 0);
    s.append_request_event(tid, "req-O", 3, "text", "{}", 0);
    s.append_request_event(tid, "req-O", 2, "text", "{}", 0);
    s.append_request_event(tid, "req-O", 4, "text", "{}", 0);

    auto events = s.list_request_events(tid, "req-O", 0, 100);
    REQUIRE(events.size() == 5);
    for (size_t i = 0; i < events.size(); ++i) {
        CHECK(events[i].seq == static_cast<int64_t>(i + 1));
    }
}
