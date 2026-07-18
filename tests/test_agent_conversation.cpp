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
