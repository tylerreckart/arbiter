#pragma once
// arbiter/include/sse_mailbox.h
//
// Bounded buffering between SSE publishers (event buses) and slow HTTP
// clients.  Without a cap, live-tail mailboxes grow without bound when
// write_all cannot keep pace with producers.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace arbiter {

inline constexpr size_t kDefaultSseMailboxMaxDepth = 2048;

// Push one item into a per-connection mailbox.  Returns false when the
// item was rejected because the client is too slow (overflowed is set).
// When `force_deliver` is true (terminal SSE envelopes), drop the oldest
// buffered items until there is room so the stream can close cleanly.
template<typename T>
bool sse_mailbox_push(std::deque<T>& mailbox,
                      std::mutex& mu,
                      std::condition_variable& cv,
                      std::atomic<bool>& overflowed,
                      T item,
                      bool force_deliver = false) {
    std::lock_guard<std::mutex> lk(mu);
    if (overflowed.load(std::memory_order_relaxed)) return false;
    const size_t cap = kDefaultSseMailboxMaxDepth;
    if (mailbox.size() >= cap) {
        if (force_deliver) {
            while (mailbox.size() >= cap && !mailbox.empty()) {
                mailbox.pop_front();
            }
        } else {
            overflowed.store(true, std::memory_order_relaxed);
            cv.notify_one();
            return false;
        }
    }
    mailbox.push_back(std::move(item));
    cv.notify_one();
    return true;
}

} // namespace arbiter
