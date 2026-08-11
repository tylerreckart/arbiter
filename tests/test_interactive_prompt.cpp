#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/interactive_prompt.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("FIFO enqueue does not fail prior waiter") {
    arbiter::InteractivePromptQueue q;
    std::atomic<bool> first_started{false};
    std::atomic<arbiter::InteractiveDecision> first_result{
        arbiter::InteractiveDecision::Cancel};
    std::atomic<arbiter::InteractiveDecision> second_result{
        arbiter::InteractiveDecision::Cancel};

    std::thread t1([&] {
        arbiter::InteractiveRequest req;
        req.kind = arbiter::InteractiveKind::Confirm;
        req.action = "exec";
        req.target = "first";
        first_started.store(true);
        first_result.store(q.request(std::move(req)));
    });
    while (!first_started.load()) {
        std::this_thread::yield();
    }
    // Give t1 time to block inside request().
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::thread t2([&] {
        arbiter::InteractiveRequest req;
        req.kind = arbiter::InteractiveKind::Confirm;
        req.action = "exec";
        req.target = "second";
        second_result.store(q.request(std::move(req)));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto a = q.take_front();
    REQUIRE(a.has_value());
    CHECK(a->request.target == "first");
    arbiter::complete_prompt_promise(a->promise,
                                     arbiter::InteractiveDecision::Allow);

    auto b = q.take_front();
    REQUIRE(b.has_value());
    CHECK(b->request.target == "second");
    arbiter::complete_prompt_promise(b->promise,
                                     arbiter::InteractiveDecision::Deny);

    t1.join();
    t2.join();
    CHECK(first_result.load() == arbiter::InteractiveDecision::Allow);
    CHECK(second_result.load() == arbiter::InteractiveDecision::Deny);
}

TEST_CASE("accept_edits still enqueues DiffReview for service path") {
    arbiter::InteractivePromptQueue q;
    q.set_accept_edits(true);
    int calls = 0;
    std::atomic<bool> started{false};
    std::thread t([&] {
        arbiter::InteractiveRequest req;
        req.kind = arbiter::InteractiveKind::DiffReview;
        req.patch_id = 3;
        req.on_complete = [&](arbiter::InteractiveDecision d) {
            CHECK(d == arbiter::InteractiveDecision::Allow);
            ++calls;
        };
        started.store(true);
        (void)q.request(std::move(req));
    });
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(q.pending());
    auto e = q.take_front();
    REQUIRE(e.has_value());
    // Service path applies (on_complete) then completes the waiter.
    if (e->request.on_complete)
        e->request.on_complete(arbiter::InteractiveDecision::Allow);
    arbiter::complete_prompt_promise(e->promise,
                                     arbiter::InteractiveDecision::Allow);
    t.join();
    CHECK(calls == 1);
}

TEST_CASE("accept_edits only skips cards enqueued while flag was on") {
    arbiter::InteractivePromptQueue q;
    arbiter::InteractiveRequest before;
    before.kind = arbiter::InteractiveKind::DiffReview;
    before.patch_id = 1;
    q.enqueue_auto(std::move(before));

    q.set_accept_edits(true);

    arbiter::InteractiveRequest after;
    after.kind = arbiter::InteractiveKind::DiffReview;
    after.patch_id = 2;
    q.enqueue_auto(std::move(after));

    auto first = q.take_front();
    REQUIRE(first.has_value());
    CHECK(first->request.patch_id == 1);
    CHECK_FALSE(first->enqueued_under_accept_edits);

    auto second = q.take_front();
    REQUIRE(second.has_value());
    CHECK(second->request.patch_id == 2);
    CHECK(second->enqueued_under_accept_edits);
}

TEST_CASE("accept_edits does not skip Confirm") {
    arbiter::InteractivePromptQueue q;
    q.set_accept_edits(true);
    std::thread t([&] {
        arbiter::InteractiveRequest req;
        req.kind = arbiter::InteractiveKind::Confirm;
        req.action = "exec";
        (void)q.request(std::move(req));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(q.pending());
    auto e = q.take_front();
    REQUIRE(e.has_value());
    CHECK_FALSE(e->enqueued_under_accept_edits);
    arbiter::complete_prompt_promise(e->promise,
                                     arbiter::InteractiveDecision::Allow);
    t.join();
}

TEST_CASE("enqueue_auto always queues (apply on service path)") {
    arbiter::InteractivePromptQueue q;
    q.set_accept_edits(true);
    int calls = 0;
    arbiter::InteractiveRequest req;
    req.kind = arbiter::InteractiveKind::DiffReview;
    req.patch_id = 1;
    req.on_complete = [&](arbiter::InteractiveDecision) { ++calls; };
    q.enqueue_auto(std::move(req));
    CHECK(calls == 0);
    CHECK(q.pending());
    auto e = q.take_front();
    REQUIRE(e.has_value());
    if (e->request.on_complete)
        e->request.on_complete(arbiter::InteractiveDecision::Allow);
    CHECK(calls == 1);
}

TEST_CASE("allow_remaining_diff_reviews keeps Confirm queued") {
    arbiter::InteractivePromptQueue q;
    arbiter::InteractiveRequest d1;
    d1.kind = arbiter::InteractiveKind::DiffReview;
    d1.patch_id = 1;
    int applied = 0;
    d1.on_complete = [&](arbiter::InteractiveDecision) { ++applied; };
    q.enqueue_auto(std::move(d1));

    arbiter::InteractiveRequest c;
    c.kind = arbiter::InteractiveKind::Confirm;
    c.action = "exec";
    // Don't block — push via enqueue pattern: use a thread for confirm.
    std::thread t([&] { (void)q.request_confirm({"exec", "rm -rf /", "", {}}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    arbiter::InteractiveRequest d2;
    d2.kind = arbiter::InteractiveKind::DiffReview;
    d2.patch_id = 2;
    d2.on_complete = [&](arbiter::InteractiveDecision) { ++applied; };
    q.enqueue_auto(std::move(d2));

    // Service first diff manually, then AllowAll path.
    auto front = q.take_front();
    REQUIRE(front.has_value());
    CHECK(front->request.kind == arbiter::InteractiveKind::DiffReview);
    if (front->request.on_complete)
        front->request.on_complete(arbiter::InteractiveDecision::AllowAll);
    ++applied; // simulate service applying the current one
    q.allow_remaining_diff_reviews();

    CHECK(q.accept_edits());
    CHECK(applied >= 2);

    auto left = q.take_front();
    REQUIRE(left.has_value());
    CHECK(left->request.kind == arbiter::InteractiveKind::Confirm);
    arbiter::complete_prompt_promise(left->promise,
                                     arbiter::InteractiveDecision::Allow);
    t.join();
}

TEST_CASE("fail_all cancels waiters") {
    arbiter::InteractivePromptQueue q;
    std::atomic<arbiter::InteractiveDecision> result{
        arbiter::InteractiveDecision::Allow};
    std::thread t([&] {
        result.store(q.request_diff_review(1, "a.cpp", "sum", {}));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.fail_all();
    t.join();
    CHECK(result.load() == arbiter::InteractiveDecision::Cancel);
}

TEST_CASE("size peek and snapshot track FIFO backlog") {
    arbiter::InteractivePromptQueue q;
    CHECK(q.size() == 0);
    CHECK_FALSE(q.peek_front().has_value());
    CHECK(q.snapshot().empty());

    arbiter::InteractiveRequest d;
    d.kind = arbiter::InteractiveKind::DiffReview;
    d.patch_id = 7;
    d.path = "x.cpp";
    q.enqueue_auto(std::move(d));

    arbiter::InteractiveRequest c;
    c.kind = arbiter::InteractiveKind::Confirm;
    c.action = "exec";
    c.target = "ls";
    std::thread t([&] { (void)q.request(std::move(c)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(q.size() == 2);
    auto peek = q.peek_front();
    REQUIRE(peek.has_value());
    CHECK(peek->kind == arbiter::InteractiveKind::DiffReview);
    CHECK(peek->patch_id == 7);
    CHECK(q.size() == 2);  // peek is non-destructive

    auto snap = q.snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0].patch_id == 7);
    CHECK(snap[1].action == "exec");
    CHECK(arbiter::interactive_request_label(snap[0]).find("diff #7")
          != std::string::npos);
    CHECK(arbiter::interactive_request_label(snap[1]).find("exec")
          != std::string::npos);

    auto front = q.take_front();
    REQUIRE(front.has_value());
    CHECK(q.size() == 1);
    auto left = q.take_front();
    REQUIRE(left.has_value());
    arbiter::complete_prompt_promise(left->promise,
                                     arbiter::InteractiveDecision::Allow);
    t.join();
}

TEST_CASE("request_confirm maps AllowAll to true") {
    CHECK(arbiter::decision_is_affirmative(arbiter::InteractiveDecision::Allow));
    CHECK(arbiter::decision_is_affirmative(arbiter::InteractiveDecision::AllowAll));
    CHECK_FALSE(arbiter::decision_is_affirmative(arbiter::InteractiveDecision::Deny));
    CHECK_FALSE(arbiter::decision_is_affirmative(arbiter::InteractiveDecision::Cancel));
}
