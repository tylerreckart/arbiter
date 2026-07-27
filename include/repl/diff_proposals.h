#pragma once
// arbiter/include/repl/diff_proposals.h
//
// Pane-local registry of ```diff patches streamed into scrollback.
// Each proposal gets a stable id for `/diff apply|reject|undo N`.

#include "diff/apply.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

enum class DiffProposalStatus : std::uint8_t {
    Pending,
    Applied,
    Rejected,
    Failed,
};

inline const char* diff_proposal_status_label(DiffProposalStatus s) {
    switch (s) {
    case DiffProposalStatus::Pending:  return "pending";
    case DiffProposalStatus::Applied:  return "applied";
    case DiffProposalStatus::Rejected: return "rejected";
    case DiffProposalStatus::Failed:   return "failed";
    }
    return "unknown";
}

struct DiffProposal {
    int                 id = 0;
    std::string         path;     // relative path from +++ header
    std::string         patch;
    DiffProposalStatus  status = DiffProposalStatus::Pending;
    std::string         error;
    DiffUndoSnapshot    undo;
};

class DiffProposalStore {
public:
    // Register a newly streamed patch.  Returns the proposal (with id)
    // or nullopt when the patch cannot be parsed far enough to show a
    // path — caller still renders the raw diff in that case.
    std::optional<DiffProposal> add_patch(std::string_view patch);

    std::optional<DiffProposal> get(int id) const;
    std::vector<DiffProposal> list() const;

    // Latest pending proposal, if any.
    std::optional<DiffProposal> latest_pending() const;
    // Latest applied proposal, if any.
    std::optional<DiffProposal> latest_applied() const;

    bool mark_rejected(int id);
    bool mark_applied(int id, DiffUndoSnapshot undo);
    bool mark_failed(int id, std::string error);
    bool clear_undo_after_revert(int id);  // applied → pending after undo

    void clear();

private:
    mutable std::mutex mu_;
    int next_id_ = 1;
    std::vector<DiffProposal> items_;
};

} // namespace arbiter
