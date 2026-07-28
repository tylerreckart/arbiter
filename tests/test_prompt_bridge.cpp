#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/prompt_bridge.h"

#include <future>
#include <mutex>

TEST_CASE("complete_prompt_promise is idempotent") {
    auto p = std::make_shared<std::promise<bool>>();
    auto fut = p->get_future();
    arbiter::complete_prompt_promise(p, true);
    CHECK(fut.get() == true);
    CHECK(p == nullptr);
    // Second complete after reset is a no-op.
    arbiter::complete_prompt_promise(p, false);
}

TEST_CASE("arm_prompt_promise fails the previous waiter") {
    std::mutex mu;
    std::shared_ptr<std::promise<int>> pending;
    auto first = arbiter::arm_prompt_promise(mu, pending, -1);
    auto first_fut = first->get_future();
    auto second = arbiter::arm_prompt_promise(mu, pending, -1);
    // First waiter is completed with the fail value.
    CHECK(first_fut.get() == -1);
    auto second_fut = second->get_future();
    arbiter::complete_prompt_promise(pending, 42);
    CHECK(second_fut.get() == 42);
}

TEST_CASE("take_prompt_promise hands ownership to the service path") {
    std::mutex mu;
    std::shared_ptr<std::promise<char>> pending;
    auto armed = arbiter::arm_prompt_promise(mu, pending, char{0});
    auto fut = armed->get_future();
    auto taken = arbiter::take_prompt_promise(mu, pending);
    CHECK(pending == nullptr);
    CHECK(taken != nullptr);
    arbiter::complete_prompt_promise(taken, 'a');
    CHECK(fut.get() == 'a');
}

TEST_CASE("complete after take does not double-set") {
    std::mutex mu;
    std::shared_ptr<std::promise<bool>> pending;
    auto armed = arbiter::arm_prompt_promise(mu, pending, false);
    auto fut = armed->get_future();
    // Esc/cancel path takes and denies.
    auto cancelled = arbiter::take_prompt_promise(mu, pending);
    arbiter::complete_prompt_promise(cancelled, false);
    // Service path finds nothing.
    auto again = arbiter::take_prompt_promise(mu, pending);
    CHECK(again == nullptr);
    CHECK(fut.get() == false);
}
