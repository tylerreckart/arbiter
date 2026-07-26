#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "agent.h"
#include "agent_conversation.h"
#include "api_client.h"
#include "constitution.h"

using namespace arbiter;

TEST_CASE("ConversationScope nests and restores the thread-local key") {
    CHECK(agent_conversation_key().empty());
    {
        ConversationScope a("aaa");
        CHECK(agent_conversation_key() == "aaa");
        {
            ConversationScope b("bbb");
            CHECK(agent_conversation_key() == "bbb");
        }
        CHECK(agent_conversation_key() == "aaa");
    }
    CHECK(agent_conversation_key().empty());
}

TEST_CASE("Agent histories are isolated per ConversationScope") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    {
        ConversationScope scope("conv-a");
        agent.set_history({Message{"user", "alpha"}});
        CHECK(agent.history().size() == 1);
        CHECK(agent.history()[0].content == "alpha");
    }
    {
        ConversationScope scope("conv-b");
        CHECK(agent.history().empty());
        agent.set_history({Message{"user", "beta"}, Message{"assistant", "ok"}});
        CHECK(agent.history().size() == 2);
    }
    {
        ConversationScope scope("conv-a");
        CHECK(agent.history().size() == 1);
        CHECK(agent.history()[0].content == "alpha");
        agent.reset_history();
        CHECK(agent.history().empty());
    }
    {
        ConversationScope scope("conv-b");
        CHECK(agent.history().size() == 2);
    }

    CHECK(agent.has_conversation("conv-b"));
    CHECK_FALSE(agent.has_conversation("conv-a"));
    agent.erase_conversation("conv-b");
    CHECK_FALSE(agent.has_conversation("conv-b"));
}

TEST_CASE("Agent to_json persists thinking and multi-turn tool_trace") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    ConversationScope scope("conv-persist");
    Message turn1{"assistant", "first reply"};
    turn1.thinking = "step one";
    ToolTraceEntry t1;
    t1.id = "t1";
    t1.label = "help";
    t1.kind = "help";
    t1.detail = "mem";
    t1.ok = true;
    t1.result_preview = "ok";
    turn1.tool_trace.push_back(t1);

    Message turn2{"assistant", "second reply"};
    turn2.thinking = "step two";
    ToolTraceEntry t2;
    t2.id = "t2";
    t2.label = "fetch:https://example.com";
    t2.kind = "fetch";
    t2.ok = false;
    t2.result_preview = "ERR: timeout";
    turn2.tool_trace.push_back(t2);

    agent.set_history({
        Message{"user", "go"},
        std::move(turn1),
        Message{"user", "again"},
        std::move(turn2),
    });

    const std::string json = agent.to_json();
    CHECK(json.find("\"thinking\":\"step one\"") != std::string::npos);
    CHECK(json.find("\"thinking\":\"step two\"") != std::string::npos);
    CHECK(json.find("\"tool_trace\"") != std::string::npos);
    CHECK(json.find("\"id\":\"t1\"") != std::string::npos);
    CHECK(json.find("\"id\":\"t2\"") != std::string::npos);
    CHECK(json.find("\"ok\":false") != std::string::npos);

    // Live append attaches to the latest assistant message.
    ToolTraceEntry t3;
    t3.id = "t3";
    t3.label = "help";
    t3.kind = "help";
    t3.ok = true;
    agent.append_tool_trace(std::move(t3));
    agent.append_thinking(" more thought");
    const auto hist = agent.history();
    REQUIRE(hist.size() == 4);
    REQUIRE(hist[3].tool_trace.size() == 2);
    CHECK(hist[3].tool_trace.back().id == "t3");
    CHECK(hist[1].tool_trace.size() == 1);
    CHECK(hist[3].thinking == "step two more thought");
    CHECK(hist[1].thinking == "step one");
}

TEST_CASE("append_thinking no-ops when latest message is not assistant") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    ConversationScope scope("conv-think");
    agent.set_history({Message{"user", "hi"}});
    agent.append_thinking("should not stick");
    REQUIRE(agent.history().size() == 1);
    CHECK(agent.history()[0].thinking.empty());
}

TEST_CASE("reset_history clears compaction state for the scope") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    ConversationScope scope("conv-compact");
    agent.set_history({
        Message{"user", "a"},
        Message{"assistant", "b"},
        Message{"user", "c"},
        Message{"assistant", "d"},
    });
    CompactionState st;
    st.summary = "prior work";
    st.covered_until = 2;
    st.generation = 1;
    agent.set_compaction_state(st);
    REQUIRE(agent.compaction_state().generation == 1);

    agent.reset_history();
    CHECK(agent.history().empty());
    CHECK(agent.compaction_state().summary.empty());
    CHECK(agent.compaction_state().covered_until == 0);
    CHECK(agent.compaction_state().generation == 0);
}

TEST_CASE("pinned facts are scoped per conversation") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    {
        ConversationScope a("conv-a");
        agent.set_compaction_pinned_facts("todos from A");
    }
    {
        ConversationScope b("conv-b");
        agent.set_compaction_pinned_facts("todos from B");
    }
    {
        ConversationScope a("conv-a");
        CHECK(agent.compaction_pinned_facts() == "todos from A");
    }
    {
        ConversationScope b("conv-b");
        CHECK(agent.compaction_pinned_facts() == "todos from B");
        agent.set_compaction_pinned_facts({});
        CHECK(agent.compaction_pinned_facts().empty());
    }
    {
        ConversationScope a("conv-a");
        CHECK(agent.compaction_pinned_facts() == "todos from A");
    }
}

TEST_CASE("set_history clears pinned facts and notices for the scope") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    ConversationScope scope("conv-reload");
    agent.set_compaction_pinned_facts("stale todos");
    CHECK(agent.compaction_pinned_facts() == "stale todos");
    agent.set_history({Message{"user", "hi"}, Message{"assistant", "yo"}});
    CHECK(agent.compaction_pinned_facts().empty());
    CHECK(agent.take_compaction_notice().empty());
    CHECK(agent.compaction_state().summary.empty());
}

TEST_CASE("commit_user_message persists tool envelopes without a model call") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    ConversationScope scope("conv-tools");
    agent.set_history({
        Message{"user", "fetch it"},
        Message{"assistant", "/fetch https://example.com"},
    });

    const std::string envelope =
        "[TOOL RESULTS]\n[/fetch https://example.com]\nok\n[END FETCH]\n"
        "[END TOOL RESULTS]";
    agent.commit_user_message(envelope);

    const auto hist = agent.history();
    REQUIRE(hist.size() == 3);
    CHECK(hist[2].role == "user");
    CHECK(hist[2].content == envelope);

    // Mid-turn quit simulation: history already contains the envelope the
    // model needs on the next turn, even though stream_continue never ran.
    const std::string json = agent.to_json();
    CHECK(json.find("[END TOOL RESULTS]") != std::string::npos);
    CHECK(json.find("/fetch https://example.com") != std::string::npos);
}

TEST_CASE("commit_user_message multipart keeps image parts") {
    ApiClient client({});
    Constitution cfg;
    cfg.name = "tester";
    cfg.model = "test-model";
    Agent agent("tester", cfg, client);

    ConversationScope scope("conv-img");
    std::vector<ContentPart> parts;
    ContentPart text;
    text.kind = ContentPart::TEXT;
    text.text = "[TOOL RESULTS]\n[/fetch img]\n[END FETCH]\n[END TOOL RESULTS]";
    parts.push_back(std::move(text));
    ContentPart img;
    img.kind = ContentPart::IMAGE;
    img.media_type = "image/png";
    img.image_data = "abc";
    parts.push_back(std::move(img));

    agent.commit_user_message(std::move(parts));
    const auto hist = agent.history();
    REQUIRE(hist.size() == 1);
    CHECK(hist[0].role == "user");
    REQUIRE(hist[0].parts.size() == 2);
    CHECK(hist[0].parts[0].kind == ContentPart::TEXT);
    CHECK(hist[0].parts[1].kind == ContentPart::IMAGE);
}

