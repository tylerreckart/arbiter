#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace arbiter {

class Orchestrator;

struct ConversationEntry {
    std::string id;
    std::string title;
    std::string cwd;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
    // 0 = not deleted. Soft-deleted entries are filtered out of list() but
    // their session file stays on disk until purge().
    std::int64_t deleted_at = 0;
};

// Global conversation registry under ~/.arbiter/conversations/.
class ConversationStore {
public:
    explicit ConversationStore(std::string config_dir);

    [[nodiscard]] std::string active_id() const;

    // Non-deleted conversations, most-recently-updated first.
    [[nodiscard]] std::vector<ConversationEntry> list() const;

    // Create a new empty conversation and make it active.
    std::string create(const std::string& cwd);

    // Like create(), but if the active conversation has no turns yet, reuses
    // it instead of creating another empty entry. Returns the resulting
    // active id (compare against active_id() beforehand to tell reuse from
    // creation).
    std::string create_or_reuse(const std::string& cwd);

    bool load(const std::string& id, Orchestrator& orch);
    void save(const std::string& id, Orchestrator& orch);

    void set_active(const std::string& id);
    void set_title(const std::string& id, const std::string& title);

    // Soft delete: marks the entry deleted (filtered from list()), leaves
    // the session file on disk. If `id` was active, switches to the next
    // remaining conversation, or creates a fresh one if none remain.
    void soft_delete(const std::string& id);

    // Hard delete: removes the manifest row and the session file. If `id`
    // was active, behaves like soft_delete's active-reassignment.
    void purge(const std::string& id);

    [[nodiscard]] std::string session_path(const std::string& id) const;

    std::string session_path_unlocked(const std::string& id) const {
        return store_dir_ + "/" + id + ".json";
    }

private:
    void ensure_initialized();
    void migrate_legacy_sessions();
    void load_manifest_unlocked();
    void save_manifest_unlocked() const;
    void gc_stale_empty_unlocked();
    std::string import_legacy_file(const std::string& src_path,
                                   const std::string& cwd_hint,
                                   bool set_active);

    // Assumes mu_ is already held by the caller.
    std::string create_unlocked(const std::string& cwd);
    void set_active_unlocked(const std::string& id);
    bool session_is_empty_unlocked(const std::string& id) const;
    void remove_and_reassign_active_unlocked(const std::string& id,
                                             bool delete_file);

    mutable std::mutex mu_;
    std::string config_dir_;
    std::string store_dir_;
    std::string active_id_;
    std::vector<ConversationEntry> entries_;
};

} // namespace arbiter
