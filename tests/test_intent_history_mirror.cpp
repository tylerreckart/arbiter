// tests/test_intent_history_mirror.cpp — Turn-scoped intent reroute
// history backup.  Pins the contract that concurrent ConversationScopes
// cannot overwrite each other's specialist restore state (the bug in
// the old Orchestrator::intent_mirror_ slot).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "agent.h"
#include "agent_conversation.h"
#include "api_client.h"
#include "constitution.h"
#include "intent_history_mirror.h"

#include <atomic>
#include <thread>

using namespace arbiter;

TEST_CASE("mirror copies requested onto dispatched and restores after") {
    ApiClient client({});
    Constitution cfg;
    cfg.model = "test-model";
    cfg.name = "index";
    Agent index("index", cfg, client);
    cfg.name = "research";
    Agent research("research", cfg, client);

    ConversationScope scope("conv-a");
    index.set_history({Message{"user", "from-index"}});
    research.set_history({Message{"user", "prior-research"}});

    {
        IntentHistoryMirror mirror(index, research);
        CHECK(mirror.active());
        CHECK(IntentHistoryMirror::current() == &mirror);
        CHECK(research.history().size() == 1);
        CHECK(research.history()[0].content == "from-index");

        research.set_history({Message{"user", "from-index"},
                              Message{"assistant", "specialist-turn"}});
        mirror.sync_to_requested();
        CHECK(index.history().size() == 2);
        CHECK(index.history()[1].content == "specialist-turn");
    }

    CHECK(IntentHistoryMirror::current() == nullptr);
    CHECK(research.history().size() == 1);
    CHECK(research.history()[0].content == "prior-research");
    CHECK(index.history().size() == 2);
    CHECK(index.history()[1].content == "specialist-turn");
}

TEST_CASE("inactive default mirror does not hide a nested active one") {
    ApiClient client({});
    Constitution cfg;
    cfg.model = "test-model";
    cfg.name = "index";
    Agent index("index", cfg, client);
    cfg.name = "research";
    Agent research("research", cfg, client);
    ConversationScope scope("conv-a");
    index.set_history({Message{"user", "idx"}});

    IntentHistoryMirror outer(index, research);
    CHECK(IntentHistoryMirror::current() == &outer);
    {
        IntentHistoryMirror inner;  // depth>0 send_internal: inactive
        CHECK_FALSE(inner.active());
        CHECK(IntentHistoryMirror::current() == &outer);
        outer.sync_to_requested();
    }
    CHECK(IntentHistoryMirror::current() == &outer);
}

TEST_CASE("concurrent mirrors isolate backups per ConversationScope") {
    ApiClient client({});
    Constitution cfg;
    cfg.model = "test-model";
    cfg.name = "index";
    Agent index("index", cfg, client);
    cfg.name = "research";
    Agent research("research", cfg, client);
    cfg.name = "reviewer";
    Agent reviewer("reviewer", cfg, client);

    {
        ConversationScope a("conv-a");
        research.set_history({Message{"user", "research-a"}});
        ConversationScope b("conv-b");
        reviewer.set_history({Message{"user", "reviewer-b"}});
    }

    std::atomic<int> ready{0};

    std::thread t_a([&] {
        ConversationScope scope("conv-a");
        index.set_history({Message{"user", "index-a"}});
        IntentHistoryMirror mirror(index, research);
        ready.fetch_add(1);
        while (ready.load() < 2) std::this_thread::yield();
        research.set_history({Message{"user", "index-a"},
                              Message{"assistant", "research-turn-a"}});
        mirror.sync_to_requested();
        // Keep the mirror alive until the sibling has also mutated.
        while (ready.load() < 3) std::this_thread::yield();
    });

    std::thread t_b([&] {
        ConversationScope scope("conv-b");
        index.set_history({Message{"user", "index-b"}});
        IntentHistoryMirror mirror(index, reviewer);
        ready.fetch_add(1);
        while (ready.load() < 2) std::this_thread::yield();
        reviewer.set_history({Message{"user", "index-b"},
                              Message{"assistant", "reviewer-turn-b"}});
        mirror.sync_to_requested();
        ready.fetch_add(1);
    });

    t_a.join();
    t_b.join();

    {
        ConversationScope a("conv-a");
        CHECK(research.history().size() == 1);
        CHECK(research.history()[0].content == "research-a");
        CHECK(index.history().size() == 2);
        CHECK(index.history()[1].content == "research-turn-a");
    }
    {
        ConversationScope b("conv-b");
        CHECK(reviewer.history().size() == 1);
        CHECK(reviewer.history()[0].content == "reviewer-b");
        CHECK(index.history().size() == 2);
        CHECK(index.history()[1].content == "reviewer-turn-b");
    }
}
