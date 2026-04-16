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
    if (s.empty()) return;
    if (need_sep_) {
        // Materialise the pending separator as exactly `\n\n`.  Strip any
        // trailing newlines from the buffer first so multi-line content
        // (markdown-rendered lines that end with `\n`) gets one blank line
        // between messages, not two or more.  If the buffer was drained in
        // between, buf_ is empty and we emit a leading `\n\n` — the prior
        // drain's content already ends with the trailing-content newline,
        // so `A` + next drain `\n\nB` renders as `A` + blank + `B`.
        while (!buf_.empty() && buf_.back() == '\n') buf_.pop_back();
        buf_ += "\n\n";
        need_sep_ = false;
    }
    buf_ += s;
}

void OutputQueue::end_message() {
    std::lock_guard<std::mutex> lk(mu_);
    need_sep_ = true;
}

void OutputQueue::push_msg(const std::string& s) {
    // Strip leading newlines from message content so callers don't
    // accidentally inject extra blank lines before their message.
    // The separator between messages is owned by end_message / need_sep_.
    size_t start = 0;
    while (start < s.size() && s[start] == '\n') ++start;
    if (start == 0)
        push(s);
    else if (start < s.size())
        push(s.substr(start));
    end_message();
}

std::string OutputQueue::drain() {
    std::lock_guard<std::mutex> lk(mu_);
    // need_sep_ intentionally survives drain — if the drained content ended
    // a message and the next push comes after a render has flushed to
    // stdout, we still want one blank line before the new content.
    return std::move(buf_);
}

} // namespace index_ai
