#pragma once
// FIFO interactive prompt queue for TUI confirm + diff-review cards.
//
// Replaces the old single-slot ConfirmState / DiffReviewState bridges:
// concurrent producers enqueue without failing prior waiters (the race that
// made approved commands report ERR: user declined).  Main thread takes the
// front entry, renders a card, and completes that entry's promise.
//
// DiffReview may also be auto-enqueued (no waiter) when ```diff patches
// register; the service path applies/rejects directly via on_complete.

#include "commands.h"
#include "tui/prompt_bridge.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

enum class InteractiveKind : std::uint8_t {
    Confirm,
    DiffReview,
};

enum class InteractiveDecision : std::uint8_t {
    Allow,
    Deny,
    AllowAll,  // apply/allow this + remaining; may set accept_edits
    Cancel,
};

inline bool decision_is_affirmative(InteractiveDecision d) {
    return d == InteractiveDecision::Allow || d == InteractiveDecision::AllowAll;
}

struct InteractiveRequest {
    InteractiveKind kind = InteractiveKind::Confirm;

    // Confirm fields (also reused for action chrome on diffs).
    std::string action;   // "write" | "exec" | "diff"
    std::string target;
    std::string summary;
    std::vector<std::string> preview_lines;

    // DiffReview fields.
    int         patch_id = 0;
    std::string path;
    void*       pane = nullptr;  // opaque Pane* for auto-apply routing
    bool        auto_review = false;  // no blocking waiter; service applies

    // Optional completion for auto-review (runs on main thread after decision).
    std::function<void(InteractiveDecision)> on_complete;
};

struct InteractiveEntry {
    InteractiveRequest request;
    std::shared_ptr<std::promise<InteractiveDecision>> promise;  // may be null
    // True when accept_edits was already on at enqueue time.  Used so
    // Confirm `A` (which turns accept_edits on) does not silently apply
    // DiffReview entries that were already waiting in the FIFO.
    bool enqueued_under_accept_edits = false;
};

class InteractivePromptQueue {
public:
    // Called after a new entry is enqueued so the main thread can leave
    // read_line and service the card.
    void set_notify(std::function<void()> fn) { notify_ = std::move(fn); }

    // Session accept-edits (Claude Code–style): auto-Allow new DiffReview
    // enqueues without a card.  Confirm (exec) always prompts.
    void set_accept_edits(bool on) {
        accept_edits_.store(on, std::memory_order_release);
    }
    [[nodiscard]] bool accept_edits() const {
        return accept_edits_.load(std::memory_order_acquire);
    }

    // Enqueue and block until the main thread completes this entry.
    // Never fails a prior waiter.  DiffReview under accept_edits still
    // goes through the service path (no apply on the caller thread).
    InteractiveDecision request(InteractiveRequest req) {
        auto done = std::make_shared<std::promise<InteractiveDecision>>();
        auto fut = done->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            InteractiveEntry entry;
            entry.enqueued_under_accept_edits =
                (req.kind == InteractiveKind::DiffReview) &&
                accept_edits_.load(std::memory_order_relaxed);
            entry.request = std::move(req);
            entry.promise = done;
            q_.push_back(std::move(entry));
        }
        if (notify_) notify_();
        return fut.get();
    }

    // Non-blocking enqueue for auto diff review (promise may be null).
    // Always enqueues so apply runs on the main service path (safe vs
    // pump/drain).  When accept_edits is on at enqueue time, service skips
    // the card for that entry only.
    void enqueue_auto(InteractiveRequest req) {
        req.auto_review = true;
        InteractiveEntry entry;
        entry.enqueued_under_accept_edits =
            accept_edits_.load(std::memory_order_relaxed);
        entry.request = std::move(req);
        entry.promise = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(entry));
        }
        if (notify_) notify_();
    }

    // ConfirmFn adapter: Allow/AllowAll → true.
    bool request_confirm(const ConfirmRequest& creq) {
        InteractiveRequest req;
        req.kind = InteractiveKind::Confirm;
        req.action = creq.action;
        req.target = creq.target;
        req.summary = creq.summary;
        req.preview_lines = creq.preview_lines;
        return decision_is_affirmative(request(std::move(req)));
    }

    // Blocking diff review (used by /diff review on the pane exec thread).
    InteractiveDecision request_diff_review(int patch_id,
                                            std::string path,
                                            std::string summary,
                                            std::vector<std::string> preview,
                                            void* pane = nullptr) {
        InteractiveRequest req;
        req.kind = InteractiveKind::DiffReview;
        req.action = "diff";
        req.target = path;
        req.patch_id = patch_id;
        req.path = std::move(path);
        req.summary = std::move(summary);
        req.preview_lines = std::move(preview);
        req.pane = pane;
        return request(std::move(req));
    }

    // Main thread: take the next pending entry (if any).
    std::optional<InteractiveEntry> take_front() {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) return std::nullopt;
        InteractiveEntry e = std::move(q_.front());
        q_.pop_front();
        return e;
    }

    [[nodiscard]] bool pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !q_.empty();
    }

    // Esc / teardown / pane close: cancel every waiter and drop auto entries
    // after running on_complete(Cancel) when present.
    void fail_all(InteractiveDecision d = InteractiveDecision::Cancel) {
        std::deque<InteractiveEntry> stolen;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stolen.swap(q_);
        }
        for (auto& e : stolen) {
            if (e.request.on_complete) e.request.on_complete(d);
            if (e.promise) complete_prompt_promise(e.promise, d);
        }
    }

    // After AllowAll on a DiffReview: complete remaining DiffReview entries
    // as Allow (and invoke on_complete).  Confirm entries stay queued.
    void allow_remaining_diff_reviews() {
        set_accept_edits(true);
        std::deque<InteractiveEntry> rest;
        std::deque<InteractiveEntry> keep;
        {
            std::lock_guard<std::mutex> lk(mu_);
            while (!q_.empty()) {
                auto e = std::move(q_.front());
                q_.pop_front();
                if (e.request.kind == InteractiveKind::DiffReview) {
                    rest.push_back(std::move(e));
                } else {
                    keep.push_back(std::move(e));
                }
            }
            q_ = std::move(keep);
        }
        for (auto& e : rest) {
            if (e.request.on_complete)
                e.request.on_complete(InteractiveDecision::Allow);
            if (e.promise)
                complete_prompt_promise(e.promise, InteractiveDecision::Allow);
        }
    }

private:
    mutable std::mutex mu_;
    std::deque<InteractiveEntry> q_;
    std::atomic<bool> accept_edits_{false};
    std::function<void()> notify_;
};

}  // namespace arbiter
