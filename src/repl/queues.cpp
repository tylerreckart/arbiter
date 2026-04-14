// index_ai/src/repl/queues.cpp — see repl/queues.h

#include "repl/queues.h"

namespace index_ai {

// ─── CommandQueue ────────────────────────────────────────────────────────────

void CommandQueue::push(std::string cmd) {
    std::lock_guard<std::mutex> lk(mu_);
    items_.push(std::move(cmd));
    cv_.notify_one();
}

bool CommandQueue::pop(std::string& out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this]{ return !items_.empty() || stopped_; });
    if (items_.empty()) return false;
    out = std::move(items_.front());
    items_.pop();
    return true;
}

void CommandQueue::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    stopped_ = true;
    cv_.notify_all();
}

int CommandQueue::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(items_.size());
}

void CommandQueue::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    while (!items_.empty()) items_.pop();
}

// ─── OutputQueue ─────────────────────────────────────────────────────────────

void OutputQueue::push(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    buf_ += s;
}

std::string OutputQueue::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    return std::move(buf_);
}

} // namespace index_ai
