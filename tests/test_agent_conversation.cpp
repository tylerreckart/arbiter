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
    const auto hist = agent.history();
    REQUIRE(hist.size() == 4);
    REQUIRE(hist[3].tool_trace.size() == 2);
    CHECK(hist[3].tool_trace.back().id == "t3");
    CHECK(hist[1].tool_trace.size() == 1);
}
