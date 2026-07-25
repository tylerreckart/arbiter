#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "context_compaction.h"
#include "json.h"
#include "model_context.h"

#include <string>
#include <vector>

using namespace arbiter;

static std::vector<Message> make_history(size_t n) {
    std::vector<Message> h;
    h.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (i % 2 == 0)
            h.push_back(Message{"user", "user-" + std::to_string(i)});
        else
            h.push_back(Message{"assistant", "asst-" + std::to_string(i)});
    }
    return h;
}

TEST_CASE("build_model_messages: empty summary returns full history") {
    auto hist = make_history(4);
    CompactionState st;
    auto view = build_model_messages(hist, st);
    REQUIRE(view.size() == 4);
    CHECK(view[0].content == "user-0");
    CHECK(view[3].content == "asst-3");
}

TEST_CASE("build_model_messages: summary envelope + recent tail") {
    auto hist = make_history(6);
    CompactionState st;
    st.summary = "We decided to use SQLite.";
    st.covered_until = 4;
    st.generation = 1;

    auto view = build_model_messages(hist, st);
    REQUIRE(view.size() == 4);  // envelope user+asst + 2 tail msgs
    CHECK(view[0].role == "user");
    CHECK(view[0].content.find("[CONVERSATION SUMMARY]") != std::string::npos);
    CHECK(view[0].content.find("SQLite") != std::string::npos);
    CHECK(view[0].content.find("[END SUMMARY]") != std::string::npos);
    CHECK(view[1].role == "assistant");
    CHECK(view[1].content.find("Understood") != std::string::npos);
    CHECK(view[2].content == "user-4");
    CHECK(view[3].content == "asst-5");
}

TEST_CASE("build_model_messages: inconsistent covered_until clears summary") {
    auto hist = make_history(2);
    CompactionState st;
    st.summary = "stale";
    st.covered_until = 99;
    auto view = build_model_messages(hist, st);
    REQUIRE(view.size() == 2);
    CHECK(view[0].content == "user-0");
    CHECK(view[0].content.find("SUMMARY") == std::string::npos);
}

TEST_CASE("compute_cut_index keeps last N and snaps to user turn") {
    auto hist = make_history(20);  // user,asst,... alternating, starts at user
    CHECK(compute_cut_index(hist, 16) == 4);
    CHECK(hist[4].role == "user");

    // Cut that would land on assistant: walk back to prior user.
    // keep=15 → raw cut=5 (assistant); snap to 4 (user).
    CHECK(compute_cut_index(hist, 15) == 4);
    CHECK(hist[compute_cut_index(hist, 15)].role == "user");
}

TEST_CASE("compute_cut_index skips TOOL RESULTS as start of kept tail") {
    std::vector<Message> hist = {
        {"user", "start"},
        {"assistant", "ok"},
        {"user", "[TOOL RESULTS]\nfoo"},
        {"assistant", "done"},
        {"user", "next"},
        {"assistant", "y"},
    };
    // keep=3 → raw cut=3 (assistant). Walk back: index 2 is TOOL RESULTS
    // (skip), index 0 is real user.
    CHECK(compute_cut_index(hist, 3) == 0);
}

TEST_CASE("should_auto_compact: threshold and cooldown") {
    CompactionConfig cfg;
    cfg.threshold_pct = 75;
    cfg.keep_messages = 16;
    cfg.enabled = true;

    // 200k window sonnet: 150k tokens = 75%
    CHECK(should_auto_compact(cfg, 150'000, "claude-sonnet-4-6",
                              /*history_len=*/40, /*chars=*/1000,
                              /*already=*/false));
    CHECK_FALSE(should_auto_compact(cfg, 150'000, "claude-sonnet-4-6",
                                     40, 1000, /*already=*/true));
    CHECK_FALSE(should_auto_compact(cfg, 100'000, "claude-sonnet-4-6",
                                     40, 1000, false));
    CHECK_FALSE(should_auto_compact(cfg, 150'000, "claude-sonnet-4-6",
                                     /*history_len=*/10, 1000, false));
}

TEST_CASE("should_auto_compact: char budget fallback when window unknown") {
    CompactionConfig cfg;
    cfg.keep_messages = 16;
    cfg.char_budget_fallback = 1000;

    CHECK(should_auto_compact(cfg, /*tokens=*/0, "ollama/llama3",
                              40, /*chars=*/1000, false));
    CHECK_FALSE(should_auto_compact(cfg, 0, "ollama/llama3",
                                     40, /*chars=*/999, false));
}

TEST_CASE("resolve_summarize_model prefers advisor") {
    CHECK(resolve_summarize_model("claude-opus-4-7", "claude-sonnet-4-6")
          == "claude-opus-4-7");
    CHECK(resolve_summarize_model("", "claude-sonnet-4-6")
          == "claude-sonnet-4-6");
}

TEST_CASE("run_compaction fail-open leaves state unchanged on empty model") {
    auto hist = make_history(40);
    CompactionState st;
    CompactionConfig cfg;
    cfg.keep_messages = 16;
    ApiClient client({});
    CHECK_FALSE(run_compaction(client, /*model=*/"", hist, st, cfg));
    CHECK(st.summary.empty());
    CHECK(st.covered_until == 0);
    CHECK(st.generation == 0);
}

TEST_CASE("compaction JSON round-trip") {
    CompactionState st;
    st.summary = "hello \"world\"";
    st.covered_until = 12;
    st.generation = 3;
    auto j = compaction_to_json(st);
    auto back = compaction_from_json(j.get());
    CHECK(back.summary == st.summary);
    CHECK(back.covered_until == st.covered_until);
    CHECK(back.generation == st.generation);
}

TEST_CASE("session v2 compaction map round-trip shape") {
    auto root = jobj();
    auto& m = root->as_object_mut();
    m["version"] = jnum(2);
    m["index"] = jarr();
    m["agents"] = jobj();

    CompactionState a;
    a.summary = "agent summary";
    a.covered_until = 8;
    a.generation = 1;
    CompactionState idx;
    idx.summary = "index summary";
    idx.covered_until = 4;
    idx.generation = 2;

    auto compaction = jobj();
    compaction->as_object_mut()["index"] = compaction_to_json(idx);
    compaction->as_object_mut()["worker"] = compaction_to_json(a);
    m["compaction"] = compaction;

    const std::string blob = json_serialize(*root);
    auto parsed = json_parse(blob);
    REQUIRE(parsed);
    CHECK(parsed->get_int("version") == 2);
    auto c = parsed->get("compaction");
    REQUIRE(c);
    REQUIRE(c->is_object());
    auto worker = compaction_from_json(c->get("worker").get());
    CHECK(worker.summary == "agent summary");
    CHECK(worker.covered_until == 8);
    auto index = compaction_from_json(c->get("index").get());
    CHECK(index.generation == 2);
}

TEST_CASE("sanitize_compaction_state clears when past history") {
    CompactionState st;
    st.summary = "x";
    st.covered_until = 10;
    st.generation = 1;
    sanitize_compaction_state(st, 5);
    CHECK(st.summary.empty());
    CHECK(st.covered_until == 0);
    CHECK(st.generation == 0);

    CompactionState ok;
    ok.summary = "y";
    ok.covered_until = 5;
    ok.generation = 2;
    sanitize_compaction_state(ok, 5);
    CHECK(ok.summary == "y");
    CHECK(ok.covered_until == 5);
}

TEST_CASE("is_tool_results_message detects prefix") {
    CHECK(is_tool_results_message(Message{"user", "[TOOL RESULTS]\nok"}));
    CHECK_FALSE(is_tool_results_message(Message{"user", "hello"}));
    CHECK_FALSE(is_tool_results_message(Message{"assistant", "[TOOL RESULTS]"}));
}

TEST_CASE("context_window helpers match prior sidebar behavior") {
    CHECK(context_window_for_model("claude-sonnet-4-6") == 200'000);
    CHECK(context_pct_value(40'000, "claude-sonnet-4-6") == 20);
    CHECK(context_pct_value(10'000, "ollama/llama3") == -1);
}
