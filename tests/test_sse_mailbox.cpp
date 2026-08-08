// tests/test_sse_mailbox.cpp — bounded SSE live-tail mailbox helper.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "sse_mailbox.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

using namespace arbiter;

TEST_CASE("sse_mailbox_push accepts up to cap then overflows") {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<int> mailbox;
    std::atomic<bool> overflowed{false};

    for (int i = 0; i < static_cast<int>(kDefaultSseMailboxMaxDepth); ++i) {
        REQUIRE(sse_mailbox_push(mailbox, mu, cv, overflowed, i));
    }
    CHECK(mailbox.size() == kDefaultSseMailboxMaxDepth);
    CHECK_FALSE(overflowed.load());

    CHECK_FALSE(sse_mailbox_push(mailbox, mu, cv, overflowed, 999));
    CHECK(overflowed.load());
    CHECK(mailbox.size() == kDefaultSseMailboxMaxDepth);
}

TEST_CASE("sse_mailbox_push force_deliver makes room for terminal item") {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> mailbox;
    std::atomic<bool> overflowed{false};

    for (size_t i = 0; i < kDefaultSseMailboxMaxDepth; ++i) {
        REQUIRE(sse_mailbox_push(mailbox, mu, cv, overflowed,
                                 std::to_string(i)));
    }
    REQUIRE(sse_mailbox_push(mailbox, mu, cv, overflowed,
                               std::string("terminal"), /*force_deliver=*/true));
    CHECK_FALSE(overflowed.load());
    CHECK(mailbox.size() == kDefaultSseMailboxMaxDepth);
    CHECK(mailbox.back() == "terminal");
    CHECK(mailbox.front() == "1");
}
