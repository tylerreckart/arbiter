// arbiter/src/repl/queues.cpp — see repl/queues.h

#include "repl/queues.h"

namespace arbiter {

namespace {

void trim_trailing_newlines(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

} // namespace

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

bool CommandQueue::try_pop(std::string& out) {
    std::lock_guard<std::mutex> lk(mu_);
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

void OutputQueue::set_notify_fn(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    notify_fn_ = std::move(fn);
}

void OutputQueue::push(const std::string& s) {
    if (s.empty()) return;
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bool new_block = false;
        if (need_sep_) {
            new_block = true;
            need_sep_ = false;
        }
        if (split_after_diff_) {
            new_block = true;
            split_after_diff_ = false;
        }

        if (!items_.empty() && items_.back().kind == OutputItem::Kind::Text && !new_block) {
            items_.back().data += s;
        } else {
            if (new_block && !items_.empty() && items_.back().kind == OutputItem::Kind::Text) {
                trim_trailing_newlines(items_.back().data);
            }
            items_.push_back(OutputItem{OutputItem::Kind::Text, s, new_block});
        }
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::end_message() {
    std::lock_guard<std::mutex> lk(mu_);
    need_sep_ = true;
}

void OutputQueue::push_msg(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == '\n' || s[start] == '\r')) ++start;
    if (start == 0) {
        push(s);
    } else if (start < s.size()) {
        push(s.substr(start));
    }
    end_message();
}

void OutputQueue::push_diff(const std::string& patch) {
    if (patch.empty()) return;
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!items_.empty() && items_.back().kind == OutputItem::Kind::Text) {
            trim_trailing_newlines(items_.back().data);
        }
        items_.push_back(OutputItem{OutputItem::Kind::Diff, patch, false});
        split_after_diff_ = true;
        fn = notify_fn_;
    }
    if (fn) fn();
}

std::vector<OutputItem> OutputQueue::drain_items() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<OutputItem> out;
    out.swap(items_);
    return out;
}

} // namespace arbiter
