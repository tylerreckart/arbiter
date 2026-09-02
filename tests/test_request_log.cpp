// tests/test_request_log.cpp — Unit tests for the durable request log
// (request_status + request_events).  Pins the contract that the SSE
// writer + resubscribe handler depend on: tenant scoping, monotonic
// seq, since_seq replay slice, recovery sweep behavior.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "scheduled_task_recovery.h"
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

TEST_CASE("scheduled task stores conversation_id for scoped fires") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t now = 1'700'000'000;
    auto conv = s.create_conversation(tid, "daily digest", "index", "");
    auto task = s.create_scheduled_task(tid, "index", conv.id, "summarize",
        "every day", "recurring", 0, R"({"every":"day"})", now);
    CHECK(task.conversation_id == conv.id);

    s.append_message(tid, conv.id, "user", "prior context", 0, 0, "");
    auto tail = s.list_messages_tail(tid, conv.id, 10);
    REQUIRE(tail.size() == 1);
    CHECK(tail[0].content == "prior context");
}

TEST_CASE("delete_latest_conversation_message rolls back only the newest row") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t other = make_tenant(s, "other");
    auto conv = s.create_conversation(tid, "daily digest", "index", "");

    auto prior = s.append_message(tid, conv.id, "user", "prior", 0, 0, "");
    auto asst  = s.append_message(tid, conv.id, "assistant", "ok", 0, 0, "");
    auto orphan = s.append_message(tid, conv.id, "user", "summarize", 0, 0, "req-1");

    auto conv_before = s.get_conversation(tid, conv.id);
    REQUIRE(conv_before);
    CHECK(conv_before->message_count == 3);

    // Not latest — must not punch a hole in history.
    CHECK_FALSE(s.delete_latest_conversation_message(tid, conv.id, prior.id));
    CHECK_FALSE(s.delete_latest_conversation_message(tid, conv.id, asst.id));
    // Wrong tenant / missing ids.
    CHECK_FALSE(s.delete_latest_conversation_message(other, conv.id, orphan.id));
    CHECK_FALSE(s.delete_latest_conversation_message(tid, conv.id, 0));
    CHECK_FALSE(s.delete_latest_conversation_message(tid, conv.id, 999999));

    CHECK(s.delete_latest_conversation_message(tid, conv.id, orphan.id));
    auto tail = s.list_messages_tail(tid, conv.id, 10);
    REQUIRE(tail.size() == 2);
    CHECK(tail[0].id == prior.id);
    CHECK(tail[1].id == asst.id);

    auto conv_after = s.get_conversation(tid, conv.id);
    REQUIRE(conv_after);
    CHECK(conv_after->message_count == 2);

    // Idempotent: already not latest / gone.
    CHECK_FALSE(s.delete_latest_conversation_message(tid, conv.id, orphan.id));

    // A retry can append the same prompt once the orphan is gone.
    auto retry = s.append_message(tid, conv.id, "user", "summarize", 0, 0, "req-2");
    CHECK(s.delete_latest_conversation_message(tid, conv.id, retry.id));
    auto tail2 = s.list_messages_tail(tid, conv.id, 10);
    REQUIRE(tail2.size() == 2);
}

TEST_CASE("trailing unmatched user prompt is the latest row a retry would reuse") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    auto conv = s.create_conversation(tid, "daily digest", "index", "");
    s.append_message(tid, conv.id, "user", "prior", 0, 0, "");
    s.append_message(tid, conv.id, "assistant", "ok", 0, 0, "");
    auto orphan = s.append_message(tid, conv.id, "user", "summarize", 0, 0, "req-crash");

    auto tail = s.list_messages_tail(tid, conv.id, 10);
    REQUIRE(tail.size() == 3);
    CHECK(tail.back().role == "user");
    CHECK(tail.back().content == "summarize");
    CHECK(tail.back().id == orphan.id);
    // Completing the turn (assistant persist) makes the user row not latest,
    // so a later failed retry cannot delete history behind a success.
    auto done = s.append_message(tid, conv.id, "assistant", "done", 0, 0, "req-ok");
    CHECK_FALSE(s.delete_latest_conversation_message(tid, conv.id, orphan.id));
    CHECK(s.delete_latest_conversation_message(tid, conv.id, done.id));
}

TEST_CASE("scheduled task claim: in-flight lease without moving next_fire_at") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t now = 1'700'000'000;
    const int64_t due_at = now - 10;

    auto task = s.create_scheduled_task(tid, "index", 0, "hello", "every hour",
        "recurring", 0, R"({"every":"hour"})", due_at);

    CHECK(s.try_claim_scheduled_task(tid, task.id, now));
    CHECK_FALSE(s.try_claim_scheduled_task(tid, task.id, now));

    auto row = s.get_scheduled_task(tid, task.id);
    REQUIRE(row);
    CHECK(row->status == "running");
    CHECK(row->next_fire_at == due_at);

    // Still due by the clock, but the running lease hides it from the
    // tick query so a second scheduler cannot overlap the first run.
    auto due = s.list_due_scheduled_tasks(now + 86400, 10);
    CHECK(due.empty());
}

TEST_CASE("scheduled task completion: paused mid-run is not overwritten") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t now = 1'700'000'000;
    const int64_t due_at = now - 10;

    auto task = s.create_scheduled_task(tid, "index", 0, "hello", "every hour",
        "recurring", 0, R"({"every":"hour"})", due_at);
    REQUIRE(s.try_claim_scheduled_task(tid, task.id, now));

    // Operator pauses while the run is still in flight.
    CHECK(s.update_scheduled_task(tid, task.id,
        std::optional<std::string>("paused"),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt));

    // Scheduler completion must not clobber the pause.
    CHECK_FALSE(s.update_scheduled_task(tid, task.id,
        std::optional<std::string>("active"),
        std::optional<int64_t>(now + 3600),
        std::optional<int64_t>(now),
        std::nullopt,
        std::optional<int64_t>(1),
        std::optional<std::string>("running")));

    auto row = s.get_scheduled_task(tid, task.id);
    REQUIRE(row);
    CHECK(row->status == "paused");
    CHECK(row->next_fire_at == due_at);
}

TEST_CASE("scheduled task: clearing running lease via active would allow re-claim") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t now = 1'700'000'000;

    auto task = s.create_scheduled_task(tid, "index", 0, "hello", "once",
        "once", now - 1, "", now - 1);
    REQUIRE(s.try_claim_scheduled_task(tid, task.id, now));

    // Store layer allows this; API PATCH/resume must reject it (409).
    CHECK(s.update_scheduled_task(tid, task.id,
        std::optional<std::string>("active"),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    CHECK(s.try_claim_scheduled_task(tid, task.id, now));
}

TEST_CASE("scheduled task recovery: releases claimed one-shot back to active") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t now = 1'700'000'000;

    auto task = s.create_scheduled_task(tid, "index", 0, "hello", "once",
        "once", now - 1, "", now - 1);
    REQUIRE(s.try_claim_scheduled_task(tid, task.id, now));

    auto orphans = s.recover_running_task_runs("failed", 9999, "interrupted");
    finalize_orphaned_scheduled_task_leases(s, 9999);
    CHECK(orphans.empty());

    auto row = s.get_scheduled_task(tid, task.id);
    REQUIRE(row);
    CHECK(row->status == "active");
    CHECK(s.list_due_scheduled_tasks(now, 10).size() == 1);
}

TEST_CASE("scheduled task recovery: succeeded run with stranded lease is finalized") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t now = 1'700'000'000;
    const int64_t due_at = now - 5;

    auto task = s.create_scheduled_task(tid, "index", 0, "hello", "once",
        "once", due_at, "", due_at);
    REQUIRE(s.try_claim_scheduled_task(tid, task.id, now));
    auto run = s.create_task_run(tid, task.id, "running", now, "req-done");
    s.update_task_run(tid, run.id,
        std::optional<std::string>("succeeded"),
        std::optional<int64_t>(now + 10),
        std::optional<std::string>("done"),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt);

    auto orphans = s.recover_running_task_runs("failed", 9999, "interrupted");
    finalize_orphaned_scheduled_task_leases(s, 9999);
    CHECK(orphans.empty());

    auto row = s.get_scheduled_task(tid, task.id);
    REQUIRE(row);
    CHECK(row->status == "completed");
    CHECK(row->last_run_id == run.id);
    CHECK(s.list_due_scheduled_tasks(now, 10).empty());
}

TEST_CASE("task_run recovery sweep: marks orphaned running rows failed") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    auto task = s.create_scheduled_task(tid, "index", 0, "hello", "once",
        "once", 0, "", 0);

    auto run1 = s.create_task_run(tid, task.id, "running", 1000, "req-a");
    auto run2 = s.create_task_run(tid, task.id, "running", 1100, "req-b");
    s.update_task_run(tid, run1.id,
        std::optional<std::string>("succeeded"),
        std::optional<int64_t>(1200),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);

    auto orphans = s.recover_running_task_runs("failed", 9999, "interrupted");
    CHECK(orphans.size() == 1);
    CHECK(orphans[0].first == tid);
    CHECK(orphans[0].second == run2.id);

    auto got = s.get_task_run(tid, run2.id);
    REQUIRE(got);
    CHECK(got->status == "failed");
    CHECK(got->completed_at == 9999);
    CHECK(got->error_message == "interrupted");

    auto settled = s.get_task_run(tid, run1.id);
    REQUIRE(settled);
    CHECK(settled->status == "succeeded");
}

TEST_CASE("idempotency_keys: put / get / race / ttl / prune") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t ttl = 3600;

    CHECK(s.put_idempotency_key(tid, "k1", "req-A", ttl, /*created_at=*/1000));
    auto got = s.get_idempotency_key(tid, "k1", ttl, /*now=*/1500);
    REQUIRE(got);
    CHECK(got->request_id == "req-A");
    CHECK(got->created_at == 1000);

    SUBCASE("same request_id is idempotent") {
        CHECK(s.put_idempotency_key(tid, "k1", "req-A", ttl, 1500));
    }

    SUBCASE("different request_id loses while unexpired") {
        CHECK(!s.put_idempotency_key(tid, "k1", "req-B", ttl, 1500));
        CHECK(s.get_idempotency_key(tid, "k1", ttl, 1500)->request_id == "req-A");
    }

    SUBCASE("expired row is overwritten") {
        CHECK(s.put_idempotency_key(tid, "k1", "req-C", ttl, /*created_at=*/5000));
        CHECK(s.get_idempotency_key(tid, "k1", ttl, 5000)->request_id == "req-C");
    }

    SUBCASE("get lazily evicts expired") {
        CHECK(!s.get_idempotency_key(tid, "k1", ttl, /*now=*/1000 + ttl + 1));
        CHECK(!s.get_idempotency_key(tid, "k1", ttl, 1000 + ttl + 1));
    }

    SUBCASE("expired get does not delete a refreshed row") {
        // Put old, then refresh via expired overwrite.  A stale DELETE
        // targeting created_at=1000 must not remove the refreshed row.
        CHECK(s.put_idempotency_key(tid, "race", "old", ttl, 1000));
        CHECK(s.put_idempotency_key(tid, "race", "new", ttl, 1000 + ttl + 10));
        auto live = s.get_idempotency_key(tid, "race", ttl, 1000 + ttl + 20);
        REQUIRE(live);
        CHECK(live->request_id == "new");
        CHECK(live->created_at == 1000 + ttl + 10);
        CHECK(s.put_idempotency_key(tid, "sib", "keep", ttl, 9000));
        CHECK(!s.get_idempotency_key(tid, "race", ttl,
                                     /*now=*/1000 + ttl + 10 + ttl + 1));
        CHECK(s.get_idempotency_key(tid, "sib", ttl, 9000)->request_id == "keep");
    }

    SUBCASE("tenants are scoped independently") {
        const int64_t other = make_tenant(s, "other");
        CHECK(s.put_idempotency_key(other, "k1", "req-Z", ttl, 1000));
        CHECK(s.get_idempotency_key(tid, "k1", ttl, 1500)->request_id == "req-A");
        CHECK(s.get_idempotency_key(other, "k1", ttl, 1500)->request_id == "req-Z");
    }

    SUBCASE("prune removes only old rows") {
        CHECK(s.put_idempotency_key(tid, "fresh", "req-F", ttl, 9000));
        const int64_t n = s.prune_idempotency_keys(/*older_than=*/2000);
        CHECK(n >= 1);
        CHECK(!s.get_idempotency_key(tid, "k1", ttl, 9000));
        CHECK(s.get_idempotency_key(tid, "fresh", ttl, 9000)->request_id == "req-F");
    }
}

TEST_CASE("idempotency_keys survive across TenantStore reopen") {
    TempDb db;
    const int64_t ttl = 86400;
    int64_t tid = 0;
    {
        TenantStore s; s.open(db.path.string());
        tid = make_tenant(s, "acme");
        CHECK(s.put_idempotency_key(tid, "restart-key", "req-persist", ttl));
    }
    {
        TenantStore s; s.open(db.path.string());
        auto got = s.get_idempotency_key(tid, "restart-key", ttl);
        REQUIRE(got);
        CHECK(got->request_id == "req-persist");
    }
}

TEST_CASE("reconcile_runs: upsert / get is tenant-scoped") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t a = make_tenant(s, "acme");
    const int64_t b = make_tenant(s, "beta");

    TenantStore::ReconcileRun row;
    row.request_id = "rec-1";
    row.tenant_id = a;
    row.status = "running";
    row.target_state_json = R"({"system":"demo"})";
    row.invariants_json = R"(["require_readme"])";
    row.workspace_kind = "path";
    row.workspace_root = "/tmp/ws";
    s.upsert_reconcile_run(row);

    auto got = s.get_reconcile_run(a, "rec-1");
    REQUIRE(got);
    CHECK(got->status == "running");
    CHECK(got->target_state_json.find("demo") != std::string::npos);
    CHECK_FALSE(s.get_reconcile_run(b, "rec-1"));

    row.status = "satisfied";
    row.reason = "ok";
    s.upsert_reconcile_run(row);
    auto again = s.get_reconcile_run(a, "rec-1");
    REQUIRE(again);
    CHECK(again->status == "satisfied");
    CHECK(again->reason == "ok");
}

TEST_CASE("reconcile_runs recovery sweep flips running rows") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    TenantStore::ReconcileRun row;
    row.request_id = "rec-live";
    row.tenant_id = tid;
    row.status = "running";
    row.target_state_json = "{}";
    row.workspace_kind = "sandbox";
    s.upsert_reconcile_run(row);

    TenantStore::ReconcileRun done = row;
    done.request_id = "rec-done";
    done.status = "satisfied";
    s.upsert_reconcile_run(done);

    auto ids = s.recover_running_reconcile_runs(999, "interrupted");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "rec-live");
    auto live = s.get_reconcile_run(tid, "rec-live");
    REQUIRE(live);
    CHECK(live->status == "failed");
    CHECK(live->reason == "interrupted");
    auto kept = s.get_reconcile_run(tid, "rec-done");
    REQUIRE(kept);
    CHECK(kept->status == "satisfied");
}

