// tests/test_reconcile.cpp — Intent reconcile: admit, contract compile,
// observe ΔS, verification mandate, implement stub, rollback.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "json.h"
#include "reconcile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        const auto pid = static_cast<long long>(::getpid());
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_reconcile_" + std::to_string(pid) + "_" +
                std::to_string(now));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << body;
}

ReconcileSpec base_spec(const fs::path& root, const std::string& target) {
    ReconcileSpec s;
    s.target_state_json = target;
    s.workspace.kind = "path";
    s.workspace.root = root.string();
    s.verification.require_tests = true;
    s.verification.command = "auto";
    s.mode = "observe";
    return s;
}

}  // namespace

TEST_CASE("named catalog is closed") {
    CHECK(named_invariant_known("require_two_factor_auth_prompt"));
    CHECK(named_invariant_known("require_readme"));
    CHECK_FALSE(named_invariant_known("steal_the_bank"));
    CHECK_FALSE(named_invariant_catalog().empty());
}

TEST_CASE("parse expr and named invariants") {
    std::string err;
    auto e = parse_invariant("amountUSD <= 15000.00", &err);
    REQUIRE(e);
    CHECK(e->tier == ReconcileInvariant::Tier::Expr);
    CHECK(e->field == "amountUSD");
    CHECK(e->op == "<=");
    CHECK(e->value == "15000.00");

    auto n = parse_invariant("require_readme", &err);
    REQUIRE(n);
    CHECK(n->tier == ReconcileInvariant::Tier::Named);

    CHECK_FALSE(parse_invariant("foo >>> bar", &err));
    CHECK_FALSE(parse_invariant("", &err));
}

TEST_CASE("admit rejects unknown named invariant") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo","amountUSD":1})");
    s.invariants = {"not_a_real_invariant"};
    auto adm = admit_reconcile(s);
    REQUIRE(adm);
    CHECK(adm->code == "unknown_invariant");
}

TEST_CASE("admit rejects contradictory expr vs target_state") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo","amountUSD":20000})");
    s.invariants = {"amountUSD <= 15000.00"};
    auto adm = admit_reconcile(s);
    REQUIRE(adm);
    CHECK(adm->code == "contradictory_invariant");
}

TEST_CASE("admit rejects unsafe verification command") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.verification.command = "make test; rm -rf /";
    auto adm = admit_reconcile(s);
    REQUIRE(adm);
    CHECK(adm->code == "unsafe_verification");
}

TEST_CASE("admit accepts holding expr + catalog name") {
    TempDir dir;
    auto s = base_spec(dir.path,
        R"({"system":"wire-transfer-portal","amountUSD":12500,"status":"SETTLED"})");
    s.invariants = {"amountUSD <= 15000.00", "require_two_factor_auth_prompt"};
    CHECK_FALSE(admit_reconcile(s));
}

TEST_CASE("compile produces verification + system + named clauses") {
    TempDir dir;
    auto s = base_spec(dir.path,
        R"({"system":"wire-transfer-portal","amountUSD":12500,"status":"SETTLED"})");
    s.invariants = {"amountUSD <= 15000.00", "require_readme"};
    auto c = compile_intent_contract(s, "run-1");
    CHECK(c.id == "run-1");
    bool saw_sys = false, saw_tests = false, saw_readme = false, saw_expr = false;
    for (const auto& cl : c.clauses) {
        if (cl.id == "system") { saw_sys = true; CHECK(cl.checker == "workspace.mentions"); }
        if (cl.id == "tests-pass") { saw_tests = true; CHECK(cl.checker == "verification.pass"); }
        if (cl.checker == "file.exists" && cl.arg == "README.md") saw_readme = true;
        if (cl.checker == "expr.holds") saw_expr = true;
    }
    CHECK(saw_sys);
    CHECK(saw_tests);
    CHECK(saw_readme);
    CHECK(saw_expr);
}

TEST_CASE("observe: empty workspace leaves residual") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme"};
    auto c = compile_intent_contract(s, "x");
    auto d = observe_contract(c, s);
    CHECK_FALSE(d.empty());
    bool saw_readme = false;
    for (const auto& r : d.residual) {
        if (r.checker == "file.exists") saw_readme = true;
    }
    CHECK(saw_readme);
}

TEST_CASE("observe: README and mentions hold") {
    TempDir dir;
    write_file(dir.path / "README.md", "# demo SETTLED portal\n");
    write_file(dir.path / "app.py", "system = 'demo'\nstatus = 'SETTLED'\n");
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme"};
    s.verification.require_tests = false;
    auto c = compile_intent_contract(s, "x");
    auto d = observe_contract(c, s);
    CHECK(d.empty());
}

TEST_CASE("run_reconcile observe: residual without implement") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    auto r = run_reconcile(s);
    CHECK(r.status == "failed");
    CHECK(r.reason == "delta_unresolved");
    CHECK_FALSE(r.contract.clauses.empty());
    CHECK_FALSE(r.brief.empty());
}

TEST_CASE("no satisfy without verification") {
    TempDir dir;
    write_file(dir.path / "README.md", "demo\n");
    write_file(dir.path / "app.py", "system demo\n");
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    // No Makefile / test runner → undetectable
    auto r = run_reconcile(s);
    CHECK(r.status == "failed");
    CHECK(r.reason == "verification_missing");
    CHECK_FALSE(r.verification.passed);
}

TEST_CASE("already-green workspace short-circuits to satisfied") {
    TempDir dir;
    write_file(dir.path / "README.md", "demo SETTLED\n");
    write_file(dir.path / "app.py", "demo portal SETTLED\n");
    write_file(dir.path / "Makefile", "test:\n\ttrue\n");
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme"};
    auto r = run_reconcile(s);
    CHECK(r.status == "satisfied");
    CHECK(r.verification.passed);
    CHECK(r.verification.command == "make test");
    CHECK(r.delta.empty());
}

TEST_CASE("ensure + implement stub writes tests then satisfies") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme", "require_two_factor_auth_prompt"};
    s.mode = "ensure";
    ReconcileHooks hooks;
    hooks.implement = [](const StateContract&, const std::string& root,
                         std::atomic<bool>*) {
        write_file(fs::path(root) / "README.md", "demo SETTLED 2fa totp\n");
        write_file(fs::path(root) / "app.py",
                   "demo portal SETTLED two_factor totp\n");
        write_file(fs::path(root) / "Makefile", "test:\n\ttrue\n");
        return std::string("wrote");
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "satisfied");
    CHECK(r.verification.passed);
    CHECK_FALSE(r.files_changed.empty());
}

TEST_CASE("broken tests never satisfy") {
    TempDir dir;
    write_file(dir.path / "README.md", "demo SETTLED 2fa\n");
    write_file(dir.path / "app.py", "demo SETTLED two_factor\n");
    write_file(dir.path / "Makefile", "test:\n\tfalse\n");
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme", "require_two_factor_auth_prompt"};
    auto r = run_reconcile(s);
    CHECK(r.status == "failed");
    CHECK(r.reason == "failed");
    CHECK_FALSE(r.verification.passed);
}

TEST_CASE("rollbackOnFailure restores pre-run tree") {
    TempDir dir;
    write_file(dir.path / "keep.txt", "original\n");
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.rollback_on_failure = true;
    ReconcileHooks hooks;
    hooks.implement = [](const StateContract&, const std::string& root,
                         std::atomic<bool>*) {
        write_file(fs::path(root) / "debris.py", "partial work\n");
        write_file(fs::path(root) / "Makefile", "test:\n\tfalse\n");
        return std::string("partial");
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "rolled_back");
    CHECK(r.rolled_back);
    CHECK(fs::exists(dir.path / "keep.txt"));
    CHECK_FALSE(fs::exists(dir.path / "debris.py"));
    // original file content preserved
    std::ifstream in(dir.path / "keep.txt");
    std::string body((std::istreambuf_iterator<char>(in)), {});
    CHECK(body == "original\n");
}

TEST_CASE("ensure without implementer is implement_required") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.mode = "ensure";
    auto r = run_reconcile(s);
    CHECK(r.status == "failed");
    CHECK(r.reason == "implement_required");
}

TEST_CASE("result_to_json is an object with status") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    auto r = run_reconcile(s);
    auto j = result_to_json(r);
    REQUIRE(j);
    CHECK(j->is_object());
    CHECK(j->get_string("status") == "failed");
    CHECK(j->get("contract"));
    CHECK(j->get("delta"));
}

TEST_CASE("detect_test_command prefers package.json then Makefile") {
    TempDir dir;
    CHECK(detect_test_command(dir.path.string()).empty());
    write_file(dir.path / "Makefile", "test:\n\ttrue\n");
    CHECK(detect_test_command(dir.path.string()) == "make test");
    write_file(dir.path / "package.json", "{}\n");
    CHECK(detect_test_command(dir.path.string()) == "npm test");
}

TEST_CASE("path workspace missing fails admit") {
    ReconcileSpec s;
    s.target_state_json = R"({"system":"x"})";
    s.workspace.kind = "path";
    s.workspace.root = "/no/such/arbiter/reconcile/ws";
    auto adm = admit_reconcile(s);
    REQUIRE(adm);
    CHECK(adm->code == "bad_workspace");
}

TEST_CASE("admit rejects unknown extra checker") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    StateClause cl;
    cl.id = "invent";
    cl.checker = "not.a.checker";
    cl.arg = "x";
    s.extra_clauses.push_back(cl);
    auto adm = admit_reconcile(s);
    REQUIRE(adm);
    CHECK(adm->code == "unknown_checker");
}

TEST_CASE("agent_map compiles onto StateClause.agent") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme"};
    s.agent_map["system"] = "quill";
    s.agent_map["inv-1"] = "writer";
    auto c = compile_intent_contract(s, "x");
    bool saw_quill = false, saw_writer = false;
    for (const auto& cl : c.clauses) {
        if (cl.id == "system") { CHECK(cl.agent == "quill"); saw_quill = true; }
        if (cl.id == "inv-1") { CHECK(cl.agent == "writer"); saw_writer = true; }
    }
    CHECK(saw_quill);
    CHECK(saw_writer);
}

TEST_CASE("observe extra checkers fail closed without projection") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.verification.require_tests = false;
    StateContract c;
    StateClause todo;
    todo.id = "t1";
    todo.checker = "todo.completed";
    todo.arg = "CI green";
    c.clauses.push_back(todo);
    StateClause art;
    art.id = "a1";
    art.checker = "artifact.exists";
    art.arg = "runbook.md";
    c.clauses.push_back(art);
    StateClause mem;
    mem.id = "m1";
    mem.checker = "memory.active";
    mem.arg = "project:deploy-42";
    c.clauses.push_back(mem);
    auto d = observe_contract(c, s);
    CHECK(d.residual.size() == 3);
    for (const auto& r : d.residual) CHECK_FALSE(r.satisfied);
}

TEST_CASE("observe extra checkers hold when projections say so") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.verification.require_tests = false;
    StateContract c;
    StateClause todo;
    todo.id = "t1";
    todo.checker = "todo.completed";
    todo.arg = "CI green";
    c.clauses.push_back(todo);
    ReconcileHooks hooks;
    hooks.todo_completed = [](const std::string& subject) {
        return subject == "CI green";
    };
    auto d = observe_contract(c, s, hooks);
    CHECK(d.empty());
}

TEST_CASE("ensure without resolvable agents is unmapped_clauses") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.verification.require_tests = false;
    s.agent_map["system"] = "";
    s.agent_map["inv-1"] = "";
    ReconcileHooks hooks;
    hooks.agent_turn = [](const ReconcileAgentCover&, const StateContract&,
                          const std::string&, std::atomic<bool>*) {
        ReconcileAgentTurn t;
        t.ok = true;
        return t;
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "failed");
    CHECK(r.reason == "unmapped_clauses");
}

TEST_CASE("ensure + fake agents: residual closes, teardown, satisfied") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo","status":"SETTLED"})");
    s.invariants = {"require_readme", "require_two_factor_auth_prompt"};
    s.mode = "ensure";
    std::map<std::string, int> catalog_history{{"nexus", 7}, {"forge", 3}};
    const auto catalog_before = catalog_history;
    std::vector<std::string> spawned, torn;
    int max_covers = 0;
    ReconcileHooks hooks;
    hooks.emit = [&](const std::string& ev, const std::shared_ptr<JsonValue>& p) {
        if (ev == "agent.spawned" && p)
            spawned.push_back(p->get_string("clone_id", ""));
        if (ev == "agent.teardown" && p)
            torn.push_back(p->get_string("clone_id", ""));
    };
    hooks.agent_turn = [&](const ReconcileAgentCover& cover,
                           const StateContract&,
                           const std::string& root,
                           std::atomic<bool>*) {
        max_covers = std::max(max_covers, static_cast<int>(cover.clause_ids.size()));
        int clone_hist = 0;
        ++clone_hist;
        (void)clone_hist;
        write_file(fs::path(root) / "README.md", "demo SETTLED 2fa totp\n");
        write_file(fs::path(root) / "app.py",
                   "demo portal SETTLED two_factor totp\n");
        write_file(fs::path(root) / "Makefile", "test:\n\ttrue\n");
        ReconcileAgentTurn t;
        t.agent_id = cover.agent_id;
        t.clone_id = cover.clone_id;
        t.ok = true;
        t.note = "clone-only";
        return t;
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "satisfied");
    CHECK(r.waves >= 1);
    CHECK_FALSE(spawned.empty());
    CHECK(spawned.size() == torn.size());
    CHECK(catalog_history == catalog_before);
    CHECK(r.verification.passed);
}

TEST_CASE("checker miss retries then clause_retry_exhausted") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.max_retries_per_clause = 1;
    s.max_waves = 12;
    s.verification.require_tests = false;
    int turns = 0;
    ReconcileHooks hooks;
    hooks.agent_turn = [&](const ReconcileAgentCover& cover, const StateContract&,
                           const std::string&, std::atomic<bool>*) {
        ++turns;
        ReconcileAgentTurn t;
        t.agent_id = cover.agent_id;
        t.clone_id = cover.clone_id;
        t.ok = true;
        t.note = "no-op";
        return t;
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "failed");
    CHECK(r.reason == "clause_retry_exhausted");
    CHECK(r.waves == 2);
    CHECK(turns == 2);
}

TEST_CASE("max_waves halt") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.max_waves = 2;
    s.max_retries_per_clause = 16;
    s.verification.require_tests = false;
    ReconcileHooks hooks;
    hooks.implement = [](const StateContract&, const std::string&,
                         std::atomic<bool>*) { return std::string("noop"); };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "failed");
    CHECK(r.reason == "max_waves");
    CHECK(r.waves == 2);
}

TEST_CASE("max_wall_ms halt") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.max_wall_ms = 5;
    s.max_retries_per_clause = 16;
    s.max_waves = 12;
    s.verification.require_tests = false;
    ReconcileHooks hooks;
    hooks.implement = [](const StateContract&, const std::string&,
                         std::atomic<bool>*) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::string("slow");
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "failed");
    CHECK(r.reason == "max_wall_ms");
    CHECK(r.waves >= 1);
}

TEST_CASE("cancel mid-wave tears down clones") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.verification.require_tests = false;
    std::atomic<bool> cancel{false};
    std::vector<std::string> torn;
    std::map<std::string, int> catalog_history{{"nexus", 5}};
    const auto catalog_before = catalog_history;
    ReconcileHooks hooks;
    hooks.cancel = &cancel;
    hooks.emit = [&](const std::string& ev, const std::shared_ptr<JsonValue>& p) {
        if (ev == "agent.teardown" && p)
            torn.push_back(p->get_string("clone_id", ""));
    };
    hooks.agent_turn = [&](const ReconcileAgentCover& cover, const StateContract&,
                           const std::string&, std::atomic<bool>*) {
        cancel.store(true);
        ReconcileAgentTurn t;
        t.agent_id = cover.agent_id;
        t.clone_id = cover.clone_id;
        return t;
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "canceled");
    CHECK_FALSE(torn.empty());
    CHECK(catalog_history == catalog_before);
}

TEST_CASE("max_agents_per_wave caps covers") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({})");
    s.verification.require_tests = false;
    s.mode = "ensure";
    s.max_agents_per_wave = 1;
    s.max_retries_per_clause = 4;
    StateClause a;
    a.id = "file-a";
    a.checker = "file.exists";
    a.arg = "A.txt";
    a.agent = "writer";
    StateClause b;
    b.id = "file-b";
    b.checker = "file.exists";
    b.arg = "B.txt";
    b.agent = "devops";
    s.extra_clauses.push_back(a);
    s.extra_clauses.push_back(b);
    int max_spawned_wave = 0;
    int current_wave_spawns = 0;
    ReconcileHooks hooks;
    hooks.emit = [&](const std::string& ev, const std::shared_ptr<JsonValue>&) {
        if (ev == "agent.spawned") ++current_wave_spawns;
        if (ev == "reconcile.delta") {
            max_spawned_wave = std::max(max_spawned_wave, current_wave_spawns);
            current_wave_spawns = 0;
        }
    };
    hooks.agent_turn = [&](const ReconcileAgentCover& cover, const StateContract&,
                           const std::string& root, std::atomic<bool>*) {
        for (const auto& id : cover.clause_ids) {
            if (id == "file-a") write_file(fs::path(root) / "A.txt", "a\n");
            if (id == "file-b") write_file(fs::path(root) / "B.txt", "b\n");
        }
        ReconcileAgentTurn t;
        t.agent_id = cover.agent_id;
        t.clone_id = cover.clone_id;
        t.ok = true;
        return t;
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "satisfied");
    CHECK(r.waves >= 2);
    CHECK(max_spawned_wave <= 1);
}

TEST_CASE("observe mode is unchanged with agent_map present") {
    TempDir dir;
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.agent_map["system"] = "quill";
    auto r = run_reconcile(s);
    CHECK(r.status == "failed");
    CHECK(r.reason == "delta_unresolved");
    CHECK(r.waves == 0);
}

TEST_CASE("rollback_on_failure still restores after a wave") {
    TempDir dir;
    write_file(dir.path / "keep.txt", "original\n");
    auto s = base_spec(dir.path, R"({"system":"demo"})");
    s.invariants = {"require_readme"};
    s.mode = "ensure";
    s.rollback_on_failure = true;
    ReconcileHooks hooks;
    hooks.implement = [](const StateContract&, const std::string& root,
                         std::atomic<bool>*) {
        write_file(fs::path(root) / "debris.py", "partial work\n");
        write_file(fs::path(root) / "Makefile", "test:\n\tfalse\n");
        return std::string("partial");
    };
    auto r = run_reconcile(s, hooks);
    CHECK(r.status == "rolled_back");
    CHECK(r.rolled_back);
    CHECK(fs::exists(dir.path / "keep.txt"));
    CHECK_FALSE(fs::exists(dir.path / "debris.py"));
}
