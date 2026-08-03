// tests/test_file_cap.cpp — atomic per-response file byte cap (#92).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "file_cap.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace arbiter;

TEST_CASE("try_reserve_file_bytes: single reservation within cap") {
    std::atomic<size_t> captured{0};
    CHECK(try_reserve_file_bytes(captured, 100, 1000));
    CHECK(captured.load() == 100);
}

TEST_CASE("try_reserve_file_bytes: rejects oversize reservation") {
    std::atomic<size_t> captured{900};
    CHECK_FALSE(try_reserve_file_bytes(captured, 200, 1000));
    CHECK(captured.load() == 900);
}

TEST_CASE("try_reserve_file_bytes: parallel writers never exceed cap") {
    constexpr size_t kCap = 1000;
    constexpr size_t kThreads = 16;
    constexpr size_t kChunk = 100;  // each thread tries to reserve 100 bytes

    std::atomic<size_t> captured{0};
    std::atomic<int> successes{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            if (try_reserve_file_bytes(captured, kChunk, kCap))
                successes.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads) t.join();

    CHECK(captured.load() <= kCap);
    CHECK(successes.load() == static_cast<int>(kCap / kChunk));
    CHECK(captured.load() == kCap);
}

TEST_CASE("try_reserve_file_bytes: individually fitting chunks jointly exceed cap") {
    constexpr size_t kCap = 500;
    constexpr size_t kThreads = 8;
    constexpr size_t kChunk = 80;  // 8 * 80 = 640 > 500

    std::atomic<size_t> captured{0};
    std::atomic<int> successes{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            if (try_reserve_file_bytes(captured, kChunk, kCap))
                successes.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads) t.join();

    CHECK(captured.load() <= kCap);
    CHECK(successes.load() == static_cast<int>(kCap / kChunk));
}

TEST_CASE("try_reserve_file_bytes: rejects overflow-prone addition") {
    std::atomic<size_t> captured{SIZE_MAX - 10};
    CHECK_FALSE(try_reserve_file_bytes(captured, 20, SIZE_MAX));
    CHECK(captured.load() == SIZE_MAX - 10);
}

TEST_CASE("release_file_bytes restores cap budget after failed write") {
    std::atomic<size_t> captured{0};
    CHECK(try_reserve_file_bytes(captured, 400, 1000));
    release_file_bytes(captured, 400);
    CHECK(captured.load() == 0);
    CHECK(try_reserve_file_bytes(captured, 1000, 1000));
    CHECK(captured.load() == 1000);
}

// Protocol for SSE /write interceptors: persist (sandbox) may fail and
// release BEFORE commit (SSE emit).  After commit, the reservation must
// stay charged — releasing would let later writes exceed file_max_bytes
// while earlier payloads remain in the response.
TEST_CASE("capped write: release only before commit, never after") {
    constexpr size_t kCap = 1000;
    constexpr size_t kChunk = 600;
    std::atomic<size_t> captured{0};
    size_t committed = 0;

    // Attempt 1: reserve → persist fails → release (no commit).
    REQUIRE(try_reserve_file_bytes(captured, kChunk, kCap));
    release_file_bytes(captured, kChunk);  // persist failed pre-commit
    CHECK(captured.load() == 0);
    CHECK(committed == 0);

    // Attempt 2: reserve → persist ok → commit.  Must not release.
    REQUIRE(try_reserve_file_bytes(captured, kChunk, kCap));
    committed += kChunk;  // SSE emit
    CHECK(captured.load() == kChunk);

    // Attempt 3: another kChunk would exceed cap — correctly rejected
    // because attempt 2's committed bytes still charge the budget.
    CHECK_FALSE(try_reserve_file_bytes(captured, kChunk, kCap));
    CHECK(captured.load() == kChunk);
    CHECK(committed == kChunk);
    CHECK(committed + kChunk > kCap);
}
