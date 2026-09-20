#pragma once
// arbiter/include/loop_wait.h
//
// Interruptible pause between LoopManager iterations.  kill()/inject()/
// suspend() notify the entry cv; sleeping with sleep_for would ignore
// those wakes and freeze /kill join for the remainder of the delay.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace arbiter {

inline bool loop_wait_interruptible(std::mutex& mu,
                                    std::condition_variable& cv,
                                    bool& stop_req,
                                    bool& suspend_req,
                                    const std::queue<std::string>& injected,
                                    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    if (stop_req) return true;
    cv.wait_for(lk, timeout, [&] {
        return stop_req || suspend_req || !injected.empty();
    });
    return stop_req;
}

}  // namespace arbiter
