#pragma once

#include <cstdint>
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

    [[nodiscard]] const std::string& active_id() const { return active_id_; }

    [[nodiscard]] std::vector<ConversationEntry> list() const;

    // Create a new empty conversation and make it active.
    std::string create(const std::string& cwd);

    bool load(const std::string& id, Orchestrator& orch);
    void save(const std::string& id, Orchestrator& orch);

    void set_active(const std::string& id);
    void set_title(const std::string& id, const std::string& title);

    [[nodiscard]] std::string session_path(const std::string& id) const;

private:
    void ensure_initialized();
    void migrate_legacy_sessions();
    void load_manifest();
    void save_manifest() const;
    std::string import_legacy_file(const std::string& src_path,
                                   const std::string& cwd_hint,
                                   bool set_active);

    std::string config_dir_;
    std::string store_dir_;
    std::string active_id_;
    std::vector<ConversationEntry> entries_;
};

} // namespace arbiter
