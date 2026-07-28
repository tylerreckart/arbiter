#pragma once
// Shared helpers for the main-thread confirm / diff-review bridge.
// Exec threads post a heap-owned promise and wake the focused input; the
// main loop completes it.  Esc/cancel and overwritten posts must always
// complete any prior waiter so fut.get() cannot hang (join deadlock).

#include <future>
#include <memory>
#include <mutex>
#include <utility>

namespace arbiter {

// Complete `pending` with `value` (if still set).  Idempotent: a second
// call after the shared_ptr was reset is a no-op; a double set_value on the
// same promise is swallowed (future_error).
template <typename T>
inline void complete_prompt_promise(std::shared_ptr<std::promise<T>>& pending,
                                    T value) {
    if (!pending) return;
    auto p = std::move(pending);
    pending.reset();
    try {
        p->set_value(std::move(value));
    } catch (...) {
        // Already satisfied — another path won the race.
    }
}

// Atomically replace the pending promise under `mu`, failing any previous
// waiter with `fail_value`.  Returns the new promise's future-facing shared
// handle (caller keeps a local copy for get_future()).
template <typename T>
inline std::shared_ptr<std::promise<T>>
arm_prompt_promise(std::mutex& mu,
                   std::shared_ptr<std::promise<T>>& pending,
                   T fail_value) {
    auto next = std::make_shared<std::promise<T>>();
    std::shared_ptr<std::promise<T>> prev;
    {
        std::lock_guard<std::mutex> lk(mu);
        prev = std::move(pending);
        pending = next;
    }
    complete_prompt_promise(prev, std::move(fail_value));
    return next;
}

// Take ownership of a pending promise (for the main-thread service_* path).
template <typename T>
inline std::shared_ptr<std::promise<T>>
take_prompt_promise(std::mutex& mu,
                    std::shared_ptr<std::promise<T>>& pending) {
    std::lock_guard<std::mutex> lk(mu);
    auto out = std::move(pending);
    pending.reset();
    return out;
}

}  // namespace arbiter
