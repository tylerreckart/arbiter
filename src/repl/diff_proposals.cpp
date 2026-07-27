// arbiter/src/repl/diff_proposals.cpp

#include "repl/diff_proposals.h"

namespace arbiter {

std::optional<DiffProposal>
DiffProposalStore::add_patch(std::string_view patch) {
    if (patch.empty()) return std::nullopt;
    auto parsed = parse_unified_diff(patch);
    // Still register when parse fails — user can see the panel and we
    // surface the error on apply.  Path may be empty.
    DiffProposal p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p.id = next_id_++;
        p.patch = std::string(patch);
        if (!parsed.new_path.empty()) p.path = parsed.new_path;
        else if (!parsed.old_path.empty()) p.path = parsed.old_path;
        else p.path = "(unknown)";
        if (!parsed.error.empty()) {
            p.status = DiffProposalStatus::Failed;
            p.error = parsed.error;
        }
        items_.push_back(p);
    }
    return p;
}

std::optional<DiffProposal> DiffProposalStore::get(int id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : items_) {
        if (p.id == id) return p;
    }
    return std::nullopt;
}

std::vector<DiffProposal> DiffProposalStore::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_;
}

std::optional<DiffProposal> DiffProposalStore::latest_pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (it->status == DiffProposalStatus::Pending) return *it;
    }
    return std::nullopt;
}

std::optional<DiffProposal> DiffProposalStore::latest_applied() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (it->status == DiffProposalStatus::Applied) return *it;
    }
    return std::nullopt;
}

bool DiffProposalStore::mark_rejected(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : items_) {
        if (p.id != id) continue;
        if (p.status != DiffProposalStatus::Pending &&
            p.status != DiffProposalStatus::Failed)
            return false;
        p.status = DiffProposalStatus::Rejected;
        p.error.clear();
        return true;
    }
    return false;
}

bool DiffProposalStore::mark_applied(int id, DiffUndoSnapshot undo) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : items_) {
        if (p.id != id) continue;
        p.status = DiffProposalStatus::Applied;
        p.error.clear();
        p.undo = std::move(undo);
        return true;
    }
    return false;
}

bool DiffProposalStore::mark_failed(int id, std::string error) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : items_) {
        if (p.id != id) continue;
        p.status = DiffProposalStatus::Failed;
        p.error = std::move(error);
        return true;
    }
    return false;
}

bool DiffProposalStore::clear_undo_after_revert(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : items_) {
        if (p.id != id) continue;
        if (p.status != DiffProposalStatus::Applied) return false;
        p.status = DiffProposalStatus::Pending;
        p.undo = DiffUndoSnapshot{};
        p.error.clear();
        return true;
    }
    return false;
}

void DiffProposalStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    items_.clear();
    next_id_ = 1;
}

} // namespace arbiter
