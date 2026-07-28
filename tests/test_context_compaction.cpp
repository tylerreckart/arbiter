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

TEST_CASE("build_model_messages: covered_until without summary does not drop") {
    auto hist = make_history(6);
    CompactionState st;
    st.covered_until = 4;  // corrupt / partial JSON — no summary
    auto view = build_model_messages(hist, st);
    REQUIRE(view.size() == 6);
    CHECK(view[0].content == "user-0");
    CHECK(view[5].content == "asst-5");
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

TEST_CASE("compute_cut_index refuses unsafe cut when no user start exists") {
    // Only tool-result "user" frames after the opener — keep window that
    // would start mid-chain must not return a raw assistant/tool cut.
    std::vector<Message> hist = {
        {"assistant", "orphan-start"},
        {"user", "[TOOL RESULTS]\na"},
        {"assistant", "mid"},
        {"user", "[TOOL RESULTS]\nb"},
        {"assistant", "end"},
    };
    CHECK(compute_cut_index(hist, 2) == 0);
}

TEST_CASE("remap without boundary prefixes full hydrated history") {
    // Legacy blob / boundary fell off replay cap.
    auto hist = make_history(4);
    CompactionState st;
    st.summary = "older context beyond replay cap";
    st.covered_until = 99;
    st.generation = 2;
    remap_compaction_onto_history(st, hist, /*keep=*/16);
    CHECK(st.summary == "older context beyond replay cap");
    CHECK(st.covered_until == 0);
    CHECK(st.generation == 2);
    auto view = build_model_messages(hist, st);
    REQUIRE(view.size() == 6);  // summary pair + 4
    CHECK(view[0].content.find("older context") != std::string::npos);
    CHECK(view[2].content == "user-0");
}

TEST_CASE("remap with unique boundary restores cut") {
    auto hist = make_history(40);
    CompactionState st;
    st.summary = "prior compacted context";
    st.covered_until = 99;
    st.generation = 1;
    st.boundary_role = "user";
    st.boundary_content = "user-4";
    remap_compaction_onto_history(st, hist, /*keep=*/16);
    CHECK(st.covered_until == 4);
    auto view = build_model_messages(hist, st);
    REQUIRE(view.size() == 38);
    CHECK(view[2].content == "user-4");
}

TEST_CASE("remap with boundary_db_id prefers id over content") {
    auto hist = make_history(10);
    // Duplicate content at index 2 and 6.
    hist[2].content = "same";
    hist[6].content = "same";
    hist[2].role = "user";
    hist[6].role = "user";
    std::vector<int64_t> ids = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    CompactionState st;
    st.summary = "s";
    st.boundary_role = "user";
    st.boundary_content = "same";
    st.boundary_db_id = 16;  // index 6
    remap_compaction_onto_history(st, hist, /*keep=*/4, &ids);
    CHECK(st.covered_until == 6);
}

TEST_CASE("find_compaction_boundary refuses ambiguous content matches") {
    std::vector<Message> hist = {
        {"user", "yes"},
        {"assistant", "ok"},
        {"user", "yes"},
        {"assistant", "ok2"},
    };
    CompactionState st;
    st.boundary_role = "user";
    st.boundary_content = "yes";
    CHECK(find_compaction_boundary(hist, st) == kCompactionBoundaryNpos);
}

TEST_CASE("remap with missing boundary falls back to full-tail prefix") {
    auto hist = make_history(40);
    CompactionState st;
    st.summary = "prior compacted context";
    st.covered_until = 4;
    st.generation = 1;
    st.boundary_role = "user";
    st.boundary_content = "user-gone";
    remap_compaction_onto_history(st, hist, /*keep=*/16);
    CHECK(st.covered_until == 0);
}

TEST_CASE("resolve_boundary_db_id unique / ambiguous / none") {
    std::vector<std::string> roles = {"user", "assistant", "user", "assistant"};
    std::vector<std::string> contents = {"a", "b", "a", "c"};
    std::vector<int64_t> ids = {1, 2, 3, 4};
    CompactionState st;
    st.boundary_role = "user";
    st.boundary_content = "a";
    auto amb = resolve_boundary_db_id(roles, contents, ids, st);
    CHECK(amb.status == BoundaryResolve::Status::Ambiguous);
    st.boundary_role = "assistant";
    st.boundary_content = "c";
    auto ok = resolve_boundary_db_id(roles, contents, ids, st);
    CHECK(ok.status == BoundaryResolve::Status::Unique);
    CHECK(ok.id == 4);
    st.boundary_content = "unique-nope";
    auto miss = resolve_boundary_db_id(roles, contents, ids, st);
    CHECK(miss.status == BoundaryResolve::Status::None);
}

TEST_CASE("strip_compaction_preambles removes todos and QUERY wrapper") {
    const std::string raw =
        "[OPEN TODOS]\n1. x\n[END OPEN TODOS]\n\n"
        "status\n\nQUERY: real user text";
    CHECK(strip_compaction_preambles(raw) == "real user text");
    CHECK(boundary_content_matches("real user text",
                                   compaction_boundary_content(
                                       strip_compaction_preambles(raw))));
}

TEST_CASE("remap_compaction_onto_history clears empty summary") {
    auto hist = make_history(20);
    CompactionState st;
    st.covered_until = 10;
    remap_compaction_onto_history(st, hist, 16);
    CHECK(st.summary.empty());
    CHECK(st.covered_until == 0);
}

TEST_CASE("compaction JSON round-trip includes boundary") {
    CompactionState st;
    st.summary = "hello";
    st.covered_until = 8;
    st.generation = 2;
    st.boundary_role = "user";
    st.boundary_content = "keep-from-here";
    auto j = compaction_to_json(st);
    auto back = compaction_from_json(j.get());
    CHECK(back.boundary_role == "user");
    CHECK(back.boundary_content == "keep-from-here");
    CHECK(back.covered_until == 8);
}

TEST_CASE("should_auto_compact: threshold") {
    CompactionConfig cfg;
    cfg.threshold_pct = 75;
    cfg.keep_messages = 16;
    cfg.enabled = true;

    // Bare claude-sonnet-4-6 aliases OpenRouter 4.6 (1M window): 750k = 75%.
    CHECK(should_auto_compact(cfg, 750'000, "claude-sonnet-4-6",
                              /*history_len=*/40, /*chars=*/1000));
    CHECK_FALSE(should_auto_compact(cfg, 100'000, "claude-sonnet-4-6",
                                     40, 1000));
    CHECK_FALSE(should_auto_compact(cfg, 750'000, "claude-sonnet-4-6",
                                     /*history_len=*/10, 1000));
    // Haiku stays on a 200k window: 150k = 75%.
    CHECK(should_auto_compact(cfg, 150'000, "claude-haiku-4-5",
                              40, 1000));
}

TEST_CASE("should_auto_compact: char budget fallback when window unknown") {
    CompactionConfig cfg;
    cfg.keep_messages = 16;
    cfg.char_budget_fallback = 1000;

    CHECK(should_auto_compact(cfg, /*tokens=*/0, "ollama/llama3",
                              40, /*chars=*/1000));
    CHECK_FALSE(should_auto_compact(cfg, 0, "ollama/llama3",
                                     40, /*chars=*/999));
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
    st.boundary_role = "user";
    st.boundary_content = "boundary-msg";
    auto j = compaction_to_json(st);
    auto back = compaction_from_json(j.get());
    CHECK(back.summary == st.summary);
    CHECK(back.covered_until == st.covered_until);
    CHECK(back.generation == st.generation);
    CHECK(back.boundary_role == st.boundary_role);
    CHECK(back.boundary_content == st.boundary_content);
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

TEST_CASE("sanitize_compaction_state clears covered_until without summary") {
    CompactionState st;
    st.covered_until = 4;
    st.generation = 1;
    sanitize_compaction_state(st, 10);
    CHECK(st.summary.empty());
    CHECK(st.covered_until == 0);
    CHECK(st.generation == 0);
}

TEST_CASE("is_tool_results_message detects prefix") {
    CHECK(is_tool_results_message(Message{"user", "[TOOL RESULTS]\nok"}));
    CHECK_FALSE(is_tool_results_message(Message{"user", "hello"}));
    CHECK_FALSE(is_tool_results_message(Message{"assistant", "[TOOL RESULTS]"}));
}

TEST_CASE("context_window helpers match prior sidebar behavior") {
    // Bare hyphenated aliases share the OpenRouter dotted-slug window.
    CHECK(context_window_for_model("claude-sonnet-4-6") == 1'000'000);
    CHECK(context_window_for_model("anthropic/claude-sonnet-4.6") == 1'000'000);
    CHECK(context_window_for_model("claude-haiku-4-5") == 200'000);
    CHECK(context_pct_value(40'000, "claude-sonnet-4-6") == 4);
    CHECK(context_pct_value(40'000, "claude-haiku-4-5") == 20);
    CHECK(context_pct_value(10'000, "ollama/llama3") == -1);
}
