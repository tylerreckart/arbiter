// tests/test_reconcile.cpp — Intent reconcile: admit, contract compile,
// observe ΔS, verification mandate, implement stub, rollback.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "json.h"
#include "reconcile.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
