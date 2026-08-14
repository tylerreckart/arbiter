#pragma once
// Turn-scoped intent-reroute history mirror.
//
// When ingress classifies index → specialist, the specialist must see the
// addressed conversation's history for the turn, checkpoints must persist
// onto the requested agent, and the specialist's own history must be
// restored afterward.  Backup lives on this RAII object (not on a single
// Orchestrator slot) so concurrent pane exec threads cannot overwrite
// each other's restore state.  Nested dispatch on the same thread leaves
// the depth-0 mirror registered via a TLS stack.

#include "agent.h"

#include <utility>
#include <vector>

namespace arbiter {

class IntentHistoryMirror;

namespace detail {
inline thread_local IntentHistoryMirror* g_intent_mirror = nullptr;
} // namespace detail

class IntentHistoryMirror {
public:
    IntentHistoryMirror() = default;

    IntentHistoryMirror(Agent& requested, Agent& dispatched)
        : requested_(&requested), dispatched_(&dispatched) {
        if (requested_ == dispatched_) {
            requested_ = nullptr;
            dispatched_ = nullptr;
            return;
        }
        dispatched_history_backup_ = dispatched_->history();
        dispatched_compaction_backup_ = dispatched_->compaction_state();
        dispatched_pinned_backup_ = dispatched_->compaction_pinned_facts();
        copy_scope(*requested_, *dispatched_);
        active_ = true;
        register_tls();
    }

    ~IntentHistoryMirror() { restore(); }

    IntentHistoryMirror(const IntentHistoryMirror&) = delete;
    IntentHistoryMirror& operator=(const IntentHistoryMirror&) = delete;

    IntentHistoryMirror(IntentHistoryMirror&& o) noexcept { *this = std::move(o); }

    IntentHistoryMirror& operator=(IntentHistoryMirror&& o) noexcept {
        if (this == &o) return *this;
        restore();
        requested_ = o.requested_;
        dispatched_ = o.dispatched_;
        dispatched_history_backup_ = std::move(o.dispatched_history_backup_);
        dispatched_compaction_backup_ = std::move(o.dispatched_compaction_backup_);
        dispatched_pinned_backup_ = std::move(o.dispatched_pinned_backup_);
        active_ = o.active_;
        prev_ = o.prev_;
        if (o.active_ && detail::g_intent_mirror == &o) detail::g_intent_mirror = this;
        o.active_ = false;
        o.requested_ = nullptr;
        o.dispatched_ = nullptr;
        o.prev_ = nullptr;
        return *this;
    }

    bool active() const { return active_; }

    // Checkpoint: copy specialist mutations onto the requested agent so
    // persist/hydrate still sees the thread the caller addressed.
    void sync_to_requested() {
        if (!active_ || !requested_ || !dispatched_) return;
        copy_scope(*dispatched_, *requested_);
    }

    static IntentHistoryMirror* current() { return detail::g_intent_mirror; }

private:
    static void copy_scope(Agent& src, Agent& dst) {
        dst.set_history(src.history());
        dst.set_compaction_state(src.compaction_state());
        dst.set_compaction_pinned_facts(src.compaction_pinned_facts());
    }

    void register_tls() {
        prev_ = detail::g_intent_mirror;
        detail::g_intent_mirror = this;
    }

    void unregister_tls() {
        if (detail::g_intent_mirror == this) detail::g_intent_mirror = prev_;
        prev_ = nullptr;
    }

    void restore() {
        if (!active_) return;
        active_ = false;
        try {
            if (requested_ && dispatched_) {
                copy_scope(*dispatched_, *requested_);
                dispatched_->set_history(std::move(dispatched_history_backup_));
                dispatched_->set_compaction_state(dispatched_compaction_backup_);
                dispatched_->set_compaction_pinned_facts(dispatched_pinned_backup_);
            }
        } catch (...) {}
        unregister_tls();
        requested_ = nullptr;
        dispatched_ = nullptr;
    }

    Agent* requested_ = nullptr;
    Agent* dispatched_ = nullptr;
    std::vector<Message> dispatched_history_backup_;
    CompactionState dispatched_compaction_backup_;
    std::string dispatched_pinned_backup_;
    bool active_ = false;
    IntentHistoryMirror* prev_ = nullptr;
};

} // namespace arbiter
