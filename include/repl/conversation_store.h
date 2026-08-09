#pragma once

#include "tenant_store.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // remain in the DB until purge().
    std::int64_t deleted_at = 0;
    // Cumulative input+output tokens billed to this conversation (persisted
    // on the conversation row; updated on each completed turn).
    int total_tokens = 0;
    // True once the title is locked against further auto-titling: either a
    // model-generated title landed (success or exhausted attempt) or the
    // user renamed it manually via /chat title.
    bool titled = false;
    // Empty = unfiled. Otherwise stringified conversation_folders.id.
    std::string folder_id;
};

struct ConversationFolderEntry {
    std::string id;
    std::string name;
    int position = 0;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

// One conversation matching a cross-conversation search.  `snippet` is a
// flattened excerpt around the first match; `match_count` totals matches
// across every message in that conversation.
struct ConversationSearchHit {
    std::string id;
    std::string title;
    int match_count = 0;
    std::string snippet;
};

// Global conversation registry backed by tenants.db (TUI-origin rows in
// the shared `conversations` table).  Previously a JSON manifest under
// ~/.arbiter/conversations/; that tree is imported once on first open.
class ConversationStore {
public:
    explicit ConversationStore(std::string config_dir);
    ~ConversationStore();

    [[nodiscard]] TenantStore& tenant_store() { return tenants_; }
    [[nodiscard]] const TenantStore& tenant_store() const { return tenants_; }
    [[nodiscard]] int64_t tenant_id() const { return tenant_id_; }

    [[nodiscard]] std::string active_id() const;

    // Stored cwd for a non-deleted conversation, or nullopt if unknown.
    [[nodiscard]] std::optional<std::string> cwd_of(const std::string& id) const;

    // Canonical absolute workspace root for host FS ops on `id`.
    // Empty/missing/legacy `session:` placeholders yield "" and set err —
    // callers must not fall back to process cwd.
    [[nodiscard]] std::string resolved_workspace_root(const std::string& id,
                                                      std::string* err = nullptr) const;

    // Non-deleted TUI conversations, most-recently-updated first.
    [[nodiscard]] std::vector<ConversationEntry> list() const;

    // Case-insensitive substring search across every non-deleted
    // conversation's saved session JSON (index master + agents).
    [[nodiscard]] std::vector<ConversationSearchHit>
    search(const std::string& term, size_t max_hits = 20) const;

    // Create a new empty conversation and make it active.
    // Optional `folder_id` (stringified) files it into that folder.
    std::string create(const std::string& cwd,
                       const std::string& folder_id = {});

    // Like create(), but if the active conversation has no turns yet, reuses
    // it instead of creating another empty entry.
    std::string create_or_reuse(const std::string& cwd,
                                const std::string& folder_id = {});

    // Prefer reusing `prefer_id` when that session is empty. When prefer_id
    // is absent, fall back to empty-active reuse (create_or_reuse). When
    // prefer_id has turns, always create — never steal another empty chat
    // (multi-pane safe).
    std::string create_or_reuse_for(const std::string& cwd,
                                    const std::string& prefer_id,
                                    const std::string& folder_id = {});

    bool load(const std::string& id, Orchestrator& orch);
    void save(const std::string& id, Orchestrator& orch);

    // Marshal a save onto the store's single background thread.
    void save_async(const std::string& id, Orchestrator& orch);

    // Mark `id` dirty without immediately queueing a save.
    void mark_dirty(const std::string& id, Orchestrator& orch);

    // Blocks until any pending/in-flight autosave has completed.
    void flush();

    [[nodiscard]] static std::chrono::seconds autosave_interval_from_env();
    [[nodiscard]] std::chrono::seconds autosave_interval() const {
        return autosave_interval_;
    }

    void set_active(const std::string& id);

    void set_title(const std::string& id, const std::string& title);
    void set_title_locked(const std::string& id, const std::string& title);
    void add_tokens(const std::string& id, int delta);
    void lock_title(const std::string& id);
    [[nodiscard]] bool is_titled(const std::string& id) const;

    void enqueue_title_job(const std::string& id,
                           const std::string& user_msg,
                           const std::string& assistant_msg,
                           const std::string& model,
                           Orchestrator& orch);

    void soft_delete(const std::string& id);
    void purge(const std::string& id);

    // ── Folders ────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<ConversationFolderEntry> list_folders() const;
    std::string create_folder(const std::string& name);
    bool rename_folder(const std::string& id, const std::string& name);
    // Unfiles children, then deletes the folder.
    bool delete_folder(const std::string& id);
    // Empty folder_id unfiles. Returns false if conversation or folder missing.
    bool move_to_folder(const std::string& conversation_id,
                        const std::string& folder_id);

    // Persisted collapsed folder ids (JSON array of numbers).
    [[nodiscard]] std::string folder_collapse_json() const;
    void set_folder_collapse_json(const std::string& json);

    // Session body for `id` (empty if unknown). Preferred over session_path
    // now that sessions live in SQLite.
    [[nodiscard]] std::string session_json(const std::string& id) const;

    // Deprecated path helper — returns empty; kept so older call sites /
    // tests compile while migrating. Prefer session_json().
    [[nodiscard]] std::string session_path(const std::string& id) const;

private:
    void ensure_initialized();
    void migrate_json_store_if_needed();
    // Lift conversation-scoped tool rows off legacy sessions/<hash>.conv
    // API "TUI session" conversations onto unified origin=tui threads.
    void migrate_legacy_conv_tool_scope();
    void reload_entries_unlocked();
    void gc_stale_empty_unlocked();

    std::string create_unlocked(const std::string& cwd,
                                const std::string& folder_id = {});
    void set_active_unlocked(const std::string& id);
    bool session_is_empty_unlocked(const std::string& id) const;
    void remove_and_reassign_active_unlocked(const std::string& id,
                                             bool hard_delete);
    [[nodiscard]] int64_t parse_id(const std::string& id) const;
    [[nodiscard]] static std::string format_id(int64_t id);
    [[nodiscard]] ConversationEntry entry_from_row(const Conversation& c) const;
    [[nodiscard]] ConversationFolderEntry
    folder_from_row(const ConversationFolder& f) const;

    void save_worker_loop();
    void autosave_timer_loop();

    struct TitleJob {
        std::string id;
        std::string user_msg;
        std::string assistant_msg;
        std::string model;
        Orchestrator* orch = nullptr;
    };
    void title_worker_loop();
    void run_title_job(const TitleJob& job);

    mutable std::mutex mu_;
    std::string config_dir_;
    std::string legacy_store_dir_;
    TenantStore tenants_;
    int64_t tenant_id_ = 0;
    std::string active_id_;
    // Includes soft-deleted rows until purge (mirrors prior manifest behaviour).
    std::vector<ConversationEntry> entries_;

    std::thread save_thread_;
    std::thread autosave_timer_thread_;
    std::mutex async_mu_;
    std::condition_variable async_cv_;
    bool busy_ = false;
    bool stop_ = false;
    bool periodic_due_ = false;
    std::atomic<bool> timer_stop_{false};
    std::unordered_map<std::string, Orchestrator*> pending_saves_;
    std::unordered_set<std::string> dirty_ids_;
    Orchestrator* last_orch_ = nullptr;
    std::chrono::seconds autosave_interval_{30};

    std::thread title_thread_;
    std::mutex title_mu_;
    std::condition_variable title_cv_;
    std::deque<TitleJob> title_queue_;
    bool title_stop_ = false;
};

} // namespace arbiter
