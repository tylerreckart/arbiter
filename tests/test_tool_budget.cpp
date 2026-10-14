// tests/test_tool_budget.cpp — Tool-loop cap keeps partial work.
//
// A scripted transport stands in for the provider so these cases stay
// hermetic. The production path only null-checks that hook.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "orchestrator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace arbiter;
namespace fs = std::filesystem;

namespace {

struct EnvGuard {
    std::string key;
    bool had = false;
    std::string prev;
    explicit EnvGuard(const char* k) : key(k) {
        if (const char* p = std::getenv(k)) {
            had = true;
            prev = p;
        }
    }
    void set(const char* v) { ::setenv(key.c_str(), v, 1); }
    void unset() { ::unsetenv(key.c_str()); }
    ~EnvGuard() {
        if (had) ::setenv(key.c_str(), prev.c_str(), 1);
        else ::unsetenv(key.c_str());
    }
};

struct CallLog {
    std::mutex mu;
    std::vector<std::string> models;
    std::vector<std::string> users;

    int count_model(const std::string& model) const {
        int n = 0;
        for (const auto& m : models)
            if (m == model) ++n;
        return n;
    }

    int count_user_has(const std::string& model, const std::string& needle) const {
        int n = 0;
        for (size_t i = 0; i < models.size(); ++i) {
            if (models[i] == model && users[i].find(needle) != std::string::npos)
                ++n;
        }
        return n;
    }

    bool any_user_has(const std::string& model, const std::string& needle) const {
        return count_user_has(model, needle) > 0;
    }
};

ApiResponse text_ok(std::string body) {
    ApiResponse r;
    r.ok = true;
    r.content = std::move(body);
    r.stop_reason = "end_turn";
    r.input_tokens = 2;
    r.output_tokens = 3;
    return r;
}

Constitution spec(const std::string& name, const std::string& model,
                  std::optional<int> turns = std::nullopt) {
    Constitution c;
    c.name = name;
    c.role = "worker";
    c.model = model;
    c.goal = "test fixture";
    c.intent.mode = "off";
    c.max_tokens = 256;
    c.max_turns = turns;
    return c;
}

struct Harness {
    EnvGuard compact{"ARBITER_COMPACT_DISABLED"};
    EnvGuard turns{"ARBITER_MAX_TURNS"};
    EnvGuard depth{"ARBITER_MAX_DELEGATION_DEPTH"};
    Orchestrator orch;
    CallLog log;
    std::mutex exec_mu;
    std::vector<std::string> execs;
    std::vector<Orchestrator::StreamEndStatus> ends;

    Harness() : orch(std::map<std::string, std::string>{}) {
        compact.set("1");
        turns.unset();
        depth.unset();
        orch.client().set_scripted_transport(
            [this](const ApiRequest& req, bool streaming, const StreamCallback* cb) {
                return on_call(req, streaming, cb);
            });
        orch.set_exec_invoker([this](const std::string& cmd) {
            std::lock_guard<std::mutex> lk(exec_mu);
            execs.push_back(cmd);
            return std::string("ran");
        });
        orch.set_stream_end_callback(
            [this](const std::string&, int, Orchestrator::StreamEndStatus st) {
                ends.push_back(st);
            });
    }

    // Overridden per test. Default refuses so a surprise call fails loud.
    std::function<ApiResponse(const ApiRequest&)> reply =
        [](const ApiRequest&) {
            ApiResponse r;
            r.ok = false;
            r.error_type = "test_unhandled";
            r.error = "no scripted reply";
            return r;
        };

    ApiResponse on_call(const ApiRequest& req, bool streaming, const StreamCallback* cb) {
        ApiResponse r = reply(req);
        if (r.ok && r.stop_reason.empty()) r.stop_reason = "end_turn";
        if (streaming && cb && *cb && !r.content.empty()) (*cb)(r.content);
        std::string last;
        for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
            if (it->role == "user") {
                last = it->content;
                break;
            }
        }
        std::lock_guard<std::mutex> lk(log.mu);
        log.models.push_back(req.model);
        log.users.push_back(std::move(last));
        return r;
    }

    bool ran_exec(const std::string& needle) const {
        for (const auto& e : execs)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
};

// Tools until the budget prompt, then a final answer that also names a
// command. The command on the final turn must not run.
ApiResponse tools_until_budget(const ApiRequest& req, const std::string& tag) {
    std::string last;
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
        if (it->role == "user") {
            last = it->content;
            break;
        }
    }
    if (last.find("Tool budget exhausted") != std::string::npos)
        return text_ok(tag + "-FINAL\n/exec " + tag + "-FINAL-CMD");
    return text_ok(tag + "-PARTIAL\n/exec " + tag + "-WORK");
}

}  // namespace

TEST_CASE("a turn inside the budget does not take a final-answer call") {
    Harness h;
    h.turns.set("9");
    auto cfg = spec("lead", "ollama/lead", 2);
    h.orch.create_agent("lead", cfg);
    h.reply = [](const ApiRequest&) { return text_ok("plain answer"); };

    auto resp = h.orch.send("lead", "hello");
    CHECK(resp.ok);
    CHECK_FALSE(resp.budget_exhausted);
    CHECK(resp.content == "plain answer");
    CHECK(h.log.count_model("ollama/lead") == 1);
    CHECK(h.log.count_user_has("ollama/lead", "Tool budget exhausted") == 0);
    CHECK(h.execs.empty());
}

TEST_CASE("final-answer turn fires once and runs no tools") {
    Harness h;
    auto cfg = spec("lead", "ollama/lead", 2);
    h.orch.create_agent("lead", cfg);
    h.reply = [](const ApiRequest& req) {
        return tools_until_budget(req, "STREAM");
    };

    std::string streamed;
    std::vector<Orchestrator::StreamEndStatus> top;
    h.orch.set_stream_end_callback(
        [&](const std::string& id, int, Orchestrator::StreamEndStatus st) {
            if (id == "lead") top.push_back(st);
        });

    auto resp = h.orch.send_streaming("lead", "go",
        [&](const std::string& chunk) { streamed += chunk; });

    CHECK(resp.ok);
    CHECK(resp.budget_exhausted);
    CHECK(resp.error_type.empty());
    CHECK(resp.content.find("STREAM-PARTIAL") != std::string::npos);
    CHECK(resp.content.find("STREAM-FINAL") != std::string::npos);
    CHECK(h.log.count_model("ollama/lead") == 3);
    CHECK(h.log.count_user_has("ollama/lead", "1 tool round left; finish up") == 1);
    CHECK(h.log.count_user_has("ollama/lead", "Tool budget exhausted") == 1);
    CHECK(h.ran_exec("STREAM-WORK"));
    CHECK_FALSE(h.ran_exec("STREAM-FINAL-CMD"));
    REQUIRE(top.size() == 1);
    CHECK(top.front() == Orchestrator::StreamEndStatus::Ok);
    CHECK(streamed.find("STREAM-FINAL") != std::string::npos);
}

TEST_CASE("capped subagent returns partial content to the parent") {
    Harness h;
    h.orch.create_agent("lead", spec("lead", "ollama/lead", 4));
    h.orch.create_agent("scout", spec("scout", "ollama/scout", 2));
    h.reply = [](const ApiRequest& req) {
        if (req.model == "ollama/lead") {
            std::string last;
            for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
                if (it->role == "user") { last = it->content; break; }
            }
            if (last.find("SCOUT-PARTIAL") != std::string::npos)
                return text_ok("parent folded scout");
            return text_ok("/agent scout find the leak");
        }
        return tools_until_budget(req, "SCOUT");
    };

    auto resp = h.orch.send("lead", "investigate");
    CHECK(resp.ok);
    CHECK(resp.content.find("parent folded scout") != std::string::npos);
    CHECK(h.log.any_user_has("ollama/lead", "SCOUT-PARTIAL"));
    CHECK(h.log.any_user_has("ollama/lead", "SCOUT-FINAL"));
    CHECK_FALSE(h.log.any_user_has("ollama/lead", "ERR: tool loop"));
    CHECK(h.ran_exec("SCOUT-WORK"));
    CHECK_FALSE(h.ran_exec("SCOUT-FINAL-CMD"));
}

TEST_CASE("provider error after partial work is returned as PARTIAL") {
    Harness h;
    h.orch.create_agent("lead", spec("lead", "ollama/lead", 4));
    h.orch.create_agent("scout", spec("scout", "ollama/scout", 6));
    int scout_calls = 0;
    h.reply = [&](const ApiRequest& req) {
        if (req.model == "ollama/lead") {
            std::string last;
            for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
                if (it->role == "user") { last = it->content; break; }
            }
            if (last.find("SCOUT-KEPT") != std::string::npos)
                return text_ok("parent kept partial");
            return text_ok("/agent scout find the leak");
        }
        ++scout_calls;
        if (scout_calls == 1)
            return text_ok("SCOUT-KEPT\n/exec echo scout-kept");
        ApiResponse fail;
        fail.ok = false;
        fail.error_type = "rate_limit_error";
        fail.error = "slow down";
        return fail;
    };

    auto resp = h.orch.send("lead", "investigate");
    CHECK(resp.ok);
    CHECK(resp.content.find("parent kept partial") != std::string::npos);
    CHECK(h.log.any_user_has(
        "ollama/lead", "[PARTIAL: rate_limit_error: slow down]"));
    CHECK(h.log.any_user_has("ollama/lead", "SCOUT-KEPT"));
    CHECK_FALSE(h.log.any_user_has("ollama/lead", "ERR: slow down"));
}

TEST_CASE("parallel worker returns partial content") {
    Harness h;
    h.orch.create_agent("lead", spec("lead", "ollama/lead", 4));
    h.orch.create_agent("scout", spec("scout", "ollama/scout", 1));
    h.reply = [](const ApiRequest& req) {
        if (req.model == "ollama/lead") {
            std::string last;
            for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
                if (it->role == "user") { last = it->content; break; }
            }
            if (last.find("PAR-PARTIAL") != std::string::npos)
                return text_ok("parent folded parallel");
            return text_ok("/parallel\n/agent scout look\n/endparallel");
        }
        return tools_until_budget(req, "PAR");
    };

    auto resp = h.orch.send("lead", "fan out");
    CHECK(resp.ok);
    CHECK(resp.content.find("parent folded parallel") != std::string::npos);
    CHECK(h.log.any_user_has("ollama/lead", "PAR-PARTIAL"));
    CHECK(h.log.any_user_has("ollama/lead", "PAR-FINAL"));
    CHECK_FALSE(h.ran_exec("PAR-WORK"));
    CHECK_FALSE(h.ran_exec("PAR-FINAL-CMD"));
}

TEST_CASE("config precedence is constitution, then env, then depth default") {
    auto count_capped = [](std::optional<int> turns, const char* env,
                           bool delegated) {
        Harness h;
        if (env) h.turns.set(env);
        else h.turns.unset();
        h.orch.create_agent("lead", spec("lead", "ollama/lead", 4));
        h.orch.create_agent("worker", spec("worker", "ollama/worker", turns));
        h.reply = [](const ApiRequest& req) {
            if (req.model == "ollama/lead") {
                std::string last;
                for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
                    if (it->role == "user") { last = it->content; break; }
                }
                if (last.find("WORKER-") != std::string::npos)
                    return text_ok("lead done");
                return text_ok("/agent worker go");
            }
            return tools_until_budget(req, "WORKER");
        };
        ApiResponse resp;
        if (delegated) {
            resp = h.orch.send("lead", "delegate");
        } else {
            h.reply = [](const ApiRequest& req) {
                return tools_until_budget(req, "WORKER");
            };
            resp = h.orch.send("worker", "go");
        }
        CHECK(resp.ok);
        return h.log.count_model("ollama/worker");
    };

    // Constitution beats the env var at depth 0.
    CHECK(count_capped(2, "9", false) == 3);
    // Env var beats the depth-0 default of 6 (+ the final-answer call).
    CHECK(count_capped(std::nullopt, "4", false) == 5);
    // Depth 0 default is 6 tool rounds plus one final-answer call.
    CHECK(count_capped(std::nullopt, nullptr, false) == 7);
    // Constitution beats env and the depth-1 default.
    CHECK(count_capped(3, "9", true) == 4);
    // Depth 1+ default is 12 tool rounds plus one final-answer call.
    CHECK(count_capped(std::nullopt, nullptr, true) == 13);
}

TEST_CASE("a plan continues past a capped phase") {
    Harness h;
    h.orch.create_agent("scout", spec("scout", "ollama/scout", 1));
    h.orch.create_agent("scribe", spec("scribe", "ollama/scribe", 4));
    h.reply = [](const ApiRequest& req) {
        if (req.model == "ollama/scribe") return text_ok("PHASE2-OK");
        return tools_until_budget(req, "PHASE1");
    };

    auto dir = fs::temp_directory_path() / "arbiter-plan-budget";
    fs::create_directories(dir);
    auto path = dir / "plan.md";
    {
        std::ofstream out(path);
        out << "### Phase 1: Research\n"
            << "**Agent:** scout\n"
            << "**Depends on:** none\n"
            << "**Task:** find the bug\n"
            << "\n"
            << "### Phase 2: Write\n"
            << "**Agent:** scribe\n"
            << "**Depends on:** 1\n"
            << "**Task:** write the fix\n";
    }

    auto result = h.orch.execute_plan(path.string());
    fs::remove_all(dir);

    CHECK(result.ok);
    CHECK(result.error.empty());
    REQUIRE(result.phases.size() == 2);
    CHECK(std::get<2>(result.phases[0]).find("PHASE1-PARTIAL") != std::string::npos);
    CHECK(std::get<2>(result.phases[1]) == "PHASE2-OK");
}

TEST_CASE("a plan continues past an iteration_limit phase") {
    Harness h;
    auto scout = spec("scout", "ollama/scout", 1);
    scout.advisor.mode = "gate";
    scout.advisor.model = "ollama/gate";
    scout.advisor_model = "ollama/gate";
    h.orch.create_agent("scout", scout);
    h.orch.create_agent("scribe", spec("scribe", "ollama/scribe", 4));
    h.reply = [](const ApiRequest& req) {
        if (req.model == "ollama/gate") {
            return text_ok("<signal>REDIRECT</signal><guidance>keep going</guidance>");
        }
        if (req.model == "ollama/scribe") return text_ok("PHASE2-OK");
        return text_ok("PHASE1-BODY");
    };

    auto dir = fs::temp_directory_path() / "arbiter-plan-limit";
    fs::create_directories(dir);
    auto path = dir / "plan.md";
    {
        std::ofstream out(path);
        out << "### Phase 1: Research\n"
            << "**Agent:** scout\n"
            << "**Depends on:** none\n"
            << "**Task:** find the bug\n"
            << "\n"
            << "### Phase 2: Write\n"
            << "**Agent:** scribe\n"
            << "**Depends on:** 1\n"
            << "**Task:** write the fix\n";
    }

    auto result = h.orch.execute_plan(path.string());
    fs::remove_all(dir);

    CHECK(result.ok);
    REQUIRE(result.phases.size() == 2);
    CHECK(std::get<2>(result.phases[0]).find("PHASE1-BODY") != std::string::npos);
    CHECK(std::get<2>(result.phases[1]) == "PHASE2-OK");
}
