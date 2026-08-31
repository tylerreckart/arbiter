// arbiter/src/repl/queues.cpp — see repl/queues.h

#include "repl/queues.h"
#include "render_policy.h"

namespace arbiter {

namespace {

void trim_trailing_newlines(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

} // namespace

// ─── CommandQueue ────────────────────────────────────────────────────────────

bool CommandQueue::push(std::string cmd) {
    QueuedCommand q;
    q.text = std::move(cmd);
    return push(std::move(q));
}

bool CommandQueue::push(QueuedCommand cmd) {
    std::lock_guard<std::mutex> lk(mu_);
    if (static_cast<int>(items_.size()) >= kMaxDepth) return false;
    items_.push(std::move(cmd));
    cv_.notify_one();
    return true;
}

void CommandQueue::push_unbounded(std::string cmd) {
    QueuedCommand q;
    q.text = std::move(cmd);
    push_unbounded(std::move(q));
}

void CommandQueue::push_unbounded(QueuedCommand cmd) {
    std::lock_guard<std::mutex> lk(mu_);
    items_.push(std::move(cmd));
    cv_.notify_one();
}

bool CommandQueue::pop(QueuedCommand& out) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this]{ return !items_.empty() || stopped_; });
    if (items_.empty()) return false;
    out = std::move(items_.front());
    items_.pop();
    return true;
}

bool CommandQueue::pop(std::string& out) {
    QueuedCommand q;
    if (!pop(q)) return false;
    out = std::move(q.text);
    return true;
}

bool CommandQueue::try_pop(QueuedCommand& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (items_.empty()) return false;
    out = std::move(items_.front());
    items_.pop();
    return true;
}

bool CommandQueue::try_pop(std::string& out) {
    QueuedCommand q;
    if (!try_pop(q)) return false;
    out = std::move(q.text);
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
            items_.push_back(
                {OutputItem::Kind::Text, s, {}, OutputItem::CodeOp::Open, 0, {}, new_block});
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

void OutputQueue::set_diff_review_hooks(DiffRegisterFn reg, DiffReviewFn review) {
    std::lock_guard<std::mutex> lk(mu_);
    diff_register_ = std::move(reg);
    diff_review_ = std::move(review);
}

void OutputQueue::push_diff(const std::string& patch) {
    if (patch.empty()) return;

    DiffRegisterFn reg;
    DiffReviewFn review;
    int proposal_id = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        reg = diff_register_;
        review = diff_review_;
    }
    // Register on the producer thread so the review gate can block the
    // model stream with a stable patch id (pump only renders).
    if (reg) proposal_id = reg(patch);

    const bool gated = review && proposal_id > 0;
    if (gated) {
        // Pause the producer until the user answers — same contract as confirms.
        // Enqueue and wake the pump only after review so Patch #N prose is not
        // drained on top of the interactive card while arrow keys repaint via
        // replace_last_prose.
        review(proposal_id, patch);
    }

    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!items_.empty() && items_.back().kind == OutputItem::Kind::Text) {
            trim_trailing_newlines(items_.back().data);
        }
        OutputItem item;
        item.kind = OutputItem::Kind::Diff;
        item.data = patch;
        item.diff_proposal_id = proposal_id;
        items_.push_back(std::move(item));
        split_after_diff_ = true;
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::push_prose(const std::vector<StyledLine>& lines) {
    if (lines.empty()) return;
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Committed lines supersede a queued live tail in the same batch.
        while (!items_.empty() && items_.back().kind == OutputItem::Kind::LiveProse) {
            items_.pop_back();
        }
        bool new_block = false;
        if (need_sep_) {
            new_block = true;
            need_sep_ = false;
        }
        if (split_after_diff_) {
            new_block = true;
            split_after_diff_ = false;
        }

        if (!items_.empty() && items_.back().kind == OutputItem::Kind::Prose && !new_block) {
            auto& back = items_.back().styled_lines;
            back.insert(back.end(), lines.begin(), lines.end());
        } else {
            items_.push_back(
                {OutputItem::Kind::Prose, {}, lines, OutputItem::CodeOp::Open, 0, {}, new_block});
        }
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::set_live_prose(const StyledLine& line) {
    if (line.text.empty()) {
        clear_live_prose();
        return;
    }
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!items_.empty() && items_.back().kind == OutputItem::Kind::LiveProse) {
            items_.back().styled_lines = {line};
        } else {
            OutputItem item;
            item.kind = OutputItem::Kind::LiveProse;
            item.styled_lines = {line};
            items_.push_back(std::move(item));
        }
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::clear_live_prose() {
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!items_.empty() && items_.back().kind == OutputItem::Kind::LiveProse) {
            items_.back().styled_lines.clear();
            fn = notify_fn_;
        }
    }
    if (fn) fn();
}

void OutputQueue::push_code_open(const std::string& open_fence,
                                 const std::string& lang,
                                 size_t preview_rows) {
    if (open_fence.empty()) return;
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool new_block = need_sep_ || split_after_diff_;
        need_sep_ = false;
        split_after_diff_ = false;
        items_.push_back({OutputItem::Kind::Code,
                          open_fence,
                          {},
                          OutputItem::CodeOp::Open,
                          preview_rows,
                          lang,
                          new_block});
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::push_code_line(const std::string& line) {
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        items_.push_back({OutputItem::Kind::Code,
                          line,
                          {},
                          OutputItem::CodeOp::Line,
                          0,
                          {},
                          false});
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::push_code_close(const std::string& close_fence) {
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        items_.push_back({OutputItem::Kind::Code,
                          close_fence,
                          {},
                          OutputItem::CodeOp::Close,
                          0,
                          {},
                          false});
        split_after_diff_ = true;
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::push_tool(const ToolActivityEvent& event) {
    if (event.id.empty() && event.label.empty()) return;
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
        OutputItem item;
        item.kind = OutputItem::Kind::Tool;
        item.new_block = new_block;
        item.tool = event;
        items_.push_back(std::move(item));
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::push_thinking(const std::string& delta, const std::string& agent_id) {
    if (delta.empty()) return;
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
        // Coalesce consecutive thinking deltas into one item when possible.
        if (!items_.empty() && items_.back().kind == OutputItem::Kind::Thinking
            && !new_block) {
            items_.back().data += delta;
            if (items_.back().agent_id.empty() && !agent_id.empty()) {
                items_.back().agent_id = agent_id;
            }
        } else {
            OutputItem item;
            item.kind = OutputItem::Kind::Thinking;
            item.data = delta;
            item.agent_id = agent_id;
            item.new_block = new_block;
            items_.push_back(std::move(item));
        }
        fn = notify_fn_;
    }
    if (fn) fn();
}

void OutputQueue::push_user_echo(std::string_view text, bool notify) {
    if (text.empty()) return;
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        OutputItem item;
        item.kind = OutputItem::Kind::UserEcho;
        item.data.assign(text.data(), text.size());
        item.new_block = true;  // each submit is its own box
        items_.push_back(std::move(item));
        // Consume pending separators so the following assistant block still
        // gets a clean gap via append_thinking / append_prose.
        need_sep_ = false;
        split_after_diff_ = false;
        if (notify) fn = notify_fn_;
    }
    if (fn) fn();
}

bool OutputQueue::try_drop_last_user_echo() {
    std::lock_guard<std::mutex> lk(mu_);
    if (items_.empty() || items_.back().kind != OutputItem::Kind::UserEcho)
        return false;
    items_.pop_back();
    need_sep_ = true;
    return true;
}

void OutputQueue::push_prose_msg(const std::string& text, StyleId id) {
    size_t start = 0;
    while (start < text.size() && (text[start] == '\n' || text[start] == '\r')) ++start;
    if (start >= text.size()) {
        end_message();
        return;
    }
    std::vector<StyledLine> lines;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(styled_plain_line(text.substr(start), id));
            break;
        }
        lines.push_back(styled_plain_line(text.substr(start, end - start), id));
        start = end + 1;
    }
    if (!lines.empty()) push_prose(lines);
    end_message();
}

std::vector<OutputItem> OutputQueue::drain_items() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<OutputItem> out;
    out.swap(items_);
    return out;
}

} // namespace arbiter
