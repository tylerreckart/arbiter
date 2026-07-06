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
};

// Global conversation registry under ~/.arbiter/conversations/.
class ConversationStore {
public:
    explicit ConversationStore(std::string config_dir);

    [[nodiscard]] std::string active_id() const;

    [[nodiscard]] std::vector<ConversationEntry> list() const;

    // Create a new empty conversation and make it active.
    std::string create(const std::string& cwd);

    bool load(const std::string& id, Orchestrator& orch);
    void save(const std::string& id, Orchestrator& orch);

    void set_active(const std::string& id);
    void set_title(const std::string& id, const std::string& title);

    [[nodiscard]] std::string session_path(const std::string& id) const;

    std::string session_path_unlocked(const std::string& id) const {
        return store_dir_ + "/" + id + ".json";
    }

private:
    void ensure_initialized();
    void migrate_legacy_sessions();
    void load_manifest();
    void save_manifest() const;
    std::string import_legacy_file(const std::string& src_path,
                                   const std::string& cwd_hint,
                                   bool set_active);

    mutable std::mutex mu_;
    std::string config_dir_;
    std::string store_dir_;
    std::string active_id_;
    std::vector<ConversationEntry> entries_;
};

} // namespace arbiter
