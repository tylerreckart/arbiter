#pragma once
// arbiter/include/loop_wait.h
//
// Interruptible pause between LoopManager iterations.  kill()/inject()/
// suspend() notify the entry cv; sleeping with sleep_for would ignore
// those wakes and freeze /kill join for the remainder of the delay.
//
// stop/suspend are atomics so a waker can store+notify without taking
// `mu` (the injected queue still needs the lock).  gcc 11 TSan does
// not model condition_variable::wait_for as releasing the mutex, so a
// second lock_guard on the waiter’s mutex looks like a double-lock.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace arbiter {

inline bool loop_wait_interruptible(std::mutex& mu,
                                    std::condition_variable& cv,
                                    std::atomic<bool>& stop_req,
                                    std::atomic<bool>& suspend_req,
                                    const std::queue<std::string>& injected,
                                    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    if (stop_req.load(std::memory_order_acquire)) return true;
    cv.wait_for(lk, timeout, [&] {
        return stop_req.load(std::memory_order_acquire) ||
               suspend_req.load(std::memory_order_acquire) ||
               !injected.empty();
    });
    return stop_req.load(std::memory_order_acquire);
}

}  // namespace arbiter
