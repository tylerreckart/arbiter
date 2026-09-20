// tests/test_loop_manager.cpp — Inter-iteration pause must wake on
// stop / inject / suspend.  run_loop used sleep_for(2s), so /kill join
// froze the TUI for the remainder of the delay even though kill()
// already notified the entry cv.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "loop_wait.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace arbiter;
using namespace std::chrono_literals;

namespace {

struct WaitState {
    std::mutex mu;
    std::condition_variable cv;
    bool stop_req = false;
    bool suspend_req = false;
    std::queue<std::string> injected;
};

void wait_until(std::atomic<bool>& flag) {
    for (int i = 0; i < 2000 && !flag.load(); ++i)
        std::this_thread::sleep_for(1ms);
}

bool wait_on(WaitState& s, std::chrono::milliseconds timeout) {
    return loop_wait_interruptible(s.mu, s.cv, s.stop_req, s.suspend_req,
                                   s.injected, timeout);
}

}  // namespace

TEST_CASE("already-stopped wait returns immediately") {
    WaitState s;
    s.stop_req = true;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(wait_on(s, 500ms));
    CHECK(std::chrono::steady_clock::now() - t0 < 80ms);
}

TEST_CASE("stop_req + notify wakes the inter-iteration wait") {
    WaitState s;
    std::atomic<bool> entered{false};
    std::atomic<bool> woke{false};
    bool stopped = false;
    std::thread t([&] {
        entered.store(true);
        stopped = wait_on(s, 2s);
        woke.store(true);
    });
    wait_until(entered);
    std::this_thread::sleep_for(20ms);
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.stop_req = true;
    }
    s.cv.notify_all();
    t.join();
    CHECK(stopped);
    CHECK(woke.load());
    CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
}

TEST_CASE("inject + notify wakes so the next iteration can take the prompt") {
    WaitState s;
    std::atomic<bool> entered{false};
    bool stopped = true;
    std::thread t([&] {
        entered.store(true);
        stopped = wait_on(s, 2s);
    });
    wait_until(entered);
    std::this_thread::sleep_for(20ms);
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.injected.push("continue from here");
    }
    s.cv.notify_all();
    t.join();
    CHECK_FALSE(stopped);
    CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
}

TEST_CASE("suspend_req + notify wakes so the loop can park") {
    WaitState s;
    std::atomic<bool> entered{false};
    bool stopped = true;
    std::thread t([&] {
        entered.store(true);
        stopped = wait_on(s, 2s);
    });
    wait_until(entered);
    std::this_thread::sleep_for(20ms);
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.suspend_req = true;
    }
    s.cv.notify_all();
    t.join();
    CHECK_FALSE(stopped);
    CHECK(std::chrono::steady_clock::now() - t0 < 200ms);
}

TEST_CASE("timeout without a signal waits the full delay") {
    WaitState s;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(wait_on(s, 80ms));
    CHECK(std::chrono::steady_clock::now() - t0 >= 70ms);
}
