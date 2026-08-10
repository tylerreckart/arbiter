#include "repl/conversation_store.h"

#include "agent_conversation.h"
#include "atomic_file.h"
#include "json.h"
#include "orchestrator.h"
#include "workspace_root.h"
#include "repl/conversation_titling.h"
#include "repl/layout_snapshot.h"
#include "tenant_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

namespace arbiter {

namespace {

std::int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string empty_session_json() {
    return R"({"version":2,"index":[],"agents":{},"compaction":{}})";
}

void sort_entries(std::vector<ConversationEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const ConversationEntry& a, const ConversationEntry& b) {
                  if (a.updated_at != b.updated_at) {
                      return a.updated_at > b.updated_at;
                  }
                  // Decimal string ids — compare numerically so "10" > "9".
                  char* end_a = nullptr;
                  char* end_b = nullptr;
                  const long long ia = std::strtoll(a.id.c_str(), &end_a, 10);
                  const long long ib = std::strtoll(b.id.c_str(), &end_b, 10);
                  const bool a_ok = end_a && end_a != a.id.c_str() && *end_a == '\0';
                  const bool b_ok = end_b && end_b != b.id.c_str() && *end_b == '\0';
                  if (a_ok && b_ok) return ia > ib;
                  return a.id > b.id;
              });
}

std::string lowercase(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int count_matches(const std::string& haystack, const std::string& needle_lc,
                  std::string* first_snippet) {
    const std::string hay_lc = lowercase(haystack);
    int count = 0;
    size_t pos = hay_lc.find(needle_lc);
    while (pos != std::string::npos) {
        if (count == 0 && first_snippet && first_snippet->empty()) {
            constexpr size_t kBefore = 30;
            constexpr size_t kAfter = 50;
            const size_t begin = pos > kBefore ? pos - kBefore : 0;
            const size_t end =
                std::min(haystack.size(), pos + needle_lc.size() + kAfter);
            std::string snip = haystack.substr(begin, end - begin);
            for (char& c : snip) {
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            }
            *first_snippet = (begin > 0 ? "…" : "") + snip
                           + (end < haystack.size() ? "…" : "");
        }
        ++count;
        pos = hay_lc.find(needle_lc, pos + needle_lc.size());
    }
    return count;
}

int count_session_matches(const JsonValue& root, const std::string& needle_lc,
                          std::string* first_snippet) {
    int total = 0;
    auto scan_messages = [&](const JsonValue* arr) {
        if (!arr || !arr->is_array()) return;
        for (const auto& m : arr->as_array()) {
            if (!m || !m->is_object()) continue;
            total +=
                count_matches(m->get_string("content"), needle_lc, first_snippet);
        }
    };
    scan_messages(root.get("index").get());
    if (auto agents = root.get("agents"); agents && agents->is_object()) {
        for (const auto& [id, msgs] : agents->as_object()) {
            (void)id;
            scan_messages(msgs.get());
        }
    }
    return total;
}

bool session_json_is_empty(const std::string& raw) {
    if (raw.empty()) return true;
    try {
        auto root = json_parse(raw);
        if (!root || !root->is_object()) return true;
        auto idx = root->get("index");
        if (idx && idx->is_array() && !idx->as_array().empty()) return false;
        auto agents = root->get("agents");
        if (agents && agents->is_object() && !agents->as_object().empty())
            return false;
        return true;
    } catch (...) {
        return true;
    }
}

} // namespace

std::chrono::seconds ConversationStore::autosave_interval_from_env() {
    const char* env = std::getenv("ARBITER_AUTOSAVE_INTERVAL_SEC");
    if (!env || !*env) return std::chrono::seconds(30);
    char* end = nullptr;
    long v = std::strtol(env, &end, 10);
    if (end == env || v < 0) return std::chrono::seconds(30);
    return std::chrono::seconds(v);
}

std::string ConversationStore::format_id(int64_t id) {
    return std::to_string(id);
}

int64_t ConversationStore::parse_id(const std::string& id) const {
    if (id.empty()) return 0;
    char* end = nullptr;
    const long long v = std::strtoll(id.c_str(), &end, 10);
    if (end == id.c_str() || *end != '\0' || v <= 0) return 0;
    return static_cast<int64_t>(v);
}

ConversationEntry ConversationStore::entry_from_row(const Conversation& c) const {
    ConversationEntry e;
    e.id = format_id(c.id);
    e.title = c.title.empty() ? "Untitled" : c.title;
    e.cwd = c.cwd;
    e.created_at = c.created_at;
    e.updated_at = c.updated_at;
    e.deleted_at = c.deleted_at;
    e.total_tokens = c.total_tokens;
    e.titled = c.titled;
    if (c.folder_id > 0) e.folder_id = format_id(c.folder_id);
    return e;
}

ConversationFolderEntry
ConversationStore::folder_from_row(const ConversationFolder& f) const {
    ConversationFolderEntry e;
    e.id = format_id(f.id);
    e.name = f.name;
    e.position = f.position;
    e.created_at = f.created_at;
    e.updated_at = f.updated_at;
    return e;
}

ConversationStore::ConversationStore(std::string config_dir)
    : config_dir_(std::move(config_dir)),
      legacy_store_dir_(config_dir_ + "/conversations"),
      autosave_interval_(autosave_interval_from_env()) {
    tenants_.open(config_dir_ + "/tenants.db");
    {
        auto all = tenants_.list_tenants();
        if (all.empty()) {
            tenant_id_ = tenants_.create_tenant("default").tenant.id;
        } else if (auto primary = resolve_primary_tenant(all)) {
            tenant_id_ = primary->id;
        } else {
            throw std::runtime_error(
                "all tenants are disabled — re-enable with: "
                "arbiter --enable-tenant <id|name>");
        }
    }
    ensure_initialized();
    save_thread_ = std::thread(&ConversationStore::save_worker_loop, this);
    if (autosave_interval_.count() > 0) {
        autosave_timer_thread_ =
            std::thread(&ConversationStore::autosave_timer_loop, this);
    }
    title_thread_ = std::thread(&ConversationStore::title_worker_loop, this);
}

ConversationStore::~ConversationStore() {
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        stop_ = true;
    }
    async_cv_.notify_all();
    timer_stop_.store(true, std::memory_order_release);
    if (save_thread_.joinable()) save_thread_.join();
    if (autosave_timer_thread_.joinable()) autosave_timer_thread_.join();

    {
        std::lock_guard<std::mutex> lk(title_mu_);
        title_stop_ = true;
    }
    title_cv_.notify_all();
    if (title_thread_.joinable()) title_thread_.join();
}

void ConversationStore::autosave_timer_loop() {
    const auto interval_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(autosave_interval_);
    while (!timer_stop_.load(std::memory_order_acquire)) {
        auto remaining = interval_ms;
        while (remaining.count() > 0 &&
               !timer_stop_.load(std::memory_order_acquire)) {
            const auto chunk =
                std::min(remaining, std::chrono::milliseconds(100));
            std::this_thread::sleep_for(chunk);
            remaining -= chunk;
        }
        if (timer_stop_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lk(async_mu_);
            if (stop_) return;
            periodic_due_ = true;
        }
        async_cv_.notify_all();
    }
}

void ConversationStore::save_worker_loop() {
    for (;;) {
        std::string id;
        Orchestrator* orch = nullptr;
        {
            std::unique_lock<std::mutex> lk(async_mu_);
            async_cv_.wait(lk, [&] {
                return !pending_saves_.empty() || stop_ || periodic_due_;
            });

            if (pending_saves_.empty() && periodic_due_) {
                periodic_due_ = false;
                if (last_orch_ && !dirty_ids_.empty()) {
                    for (const auto& did : dirty_ids_)
                        pending_saves_[did] = last_orch_;
                }
            }

            if (pending_saves_.empty() && stop_) {
                if (last_orch_ && !dirty_ids_.empty()) {
                    for (const auto& did : dirty_ids_)
                        pending_saves_[did] = last_orch_;
                }
                if (pending_saves_.empty()) return;
            }

            if (pending_saves_.empty()) continue;

            auto it = pending_saves_.begin();
            id = it->first;
            orch = it->second;
            pending_saves_.erase(it);
            busy_ = true;
        }

        if (orch) save(id, *orch);

        {
            std::lock_guard<std::mutex> lk(async_mu_);
            if (pending_saves_.find(id) == pending_saves_.end())
                dirty_ids_.erase(id);
            busy_ = false;
        }
        async_cv_.notify_all();
    }
}

void ConversationStore::save_async(const std::string& id, Orchestrator& orch) {
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        last_orch_ = &orch;
        dirty_ids_.insert(id);
        pending_saves_[id] = &orch;
    }
    async_cv_.notify_all();
}

void ConversationStore::mark_dirty(const std::string& id, Orchestrator& orch) {
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        last_orch_ = &orch;
        dirty_ids_.insert(id);
    }
    async_cv_.notify_all();
}

void ConversationStore::flush() {
    std::unique_lock<std::mutex> lk(async_mu_);
    if (last_orch_) {
        for (const auto& did : dirty_ids_)
            pending_saves_[did] = last_orch_;
    }
    if (!pending_saves_.empty()) {
        lk.unlock();
        async_cv_.notify_all();
        lk.lock();
    }
    async_cv_.wait(lk, [&] {
        return pending_saves_.empty() && !busy_;
    });
}

void ConversationStore::reload_entries_unlocked() {
    entries_.clear();
    // Include soft-deleted so purge can resolve ids after restart; list()
    // still filters them out of the sidebar.
    for (const auto& c :
         tenants_.list_tui_conversations(tenant_id_, /*include_deleted=*/true)) {
        entries_.push_back(entry_from_row(c));
    }
    sort_entries(entries_);
}

void ConversationStore::migrate_json_store_if_needed() {
    if (tenants_.tui_conversations_migrated(tenant_id_)) return;

    std::unordered_map<std::string, std::string> id_map; // old → new
    std::vector<ConversationEntry> imported;

    auto session_token_total = [](const std::string& body) -> int {
        if (body.empty()) return 0;
        try {
            auto root = json_parse(body);
            if (!root || !root->is_object()) return 0;
            auto usage = root->get("usage");
            if (!usage || !usage->is_object()) return 0;
            return static_cast<int>(usage->get_number("total_tokens"));
        } catch (...) {
            return 0;
        }
    };

    auto import_one = [&](const ConversationEntry& meta,
                          const std::string& session_body) {
        if (meta.id.empty()) return;

        const std::string body =
            session_body.empty() ? empty_session_json() : session_body;
        const int tokens = std::max(meta.total_tokens, session_token_total(body));

        auto apply_meta = [&](int64_t db_id) {
            tenants_.update_tui_conversation(
                tenant_id_, db_id,
                meta.title.empty() ? std::string("Untitled") : meta.title,
                meta.cwd,
                meta.titled ? 1 : 0,
                meta.deleted_at,
                std::max(0, tokens));
            tenants_.set_conversation_session_json(
                tenant_id_, db_id, body, /*bump_updated_at=*/false);
            tenants_.set_conversation_timestamps(
                tenant_id_, db_id,
                meta.created_at > 0 ? meta.created_at : -1,
                meta.updated_at > 0 ? meta.updated_at : -1);
        };

        // Crash resume: row may already exist from a partial prior import.
        // Still re-apply metadata/session so a death between INSERT and
        // PATCH cannot leave stale timestamps/tokens forever.
        if (auto existing =
                tenants_.find_tui_by_legacy_id(tenant_id_, meta.id)) {
            apply_meta(existing->id);
            id_map[meta.id] = format_id(existing->id);
            ConversationEntry e = meta;
            e.id = format_id(existing->id);
            e.total_tokens = tokens;
            imported.push_back(std::move(e));
            return;
        }

        Conversation created;
        try {
            created = tenants_.create_tui_conversation(
                tenant_id_, meta.title, meta.cwd, body, meta.id);
        } catch (const std::exception&) {
            if (auto existing =
                    tenants_.find_tui_by_legacy_id(tenant_id_, meta.id)) {
                apply_meta(existing->id);
                id_map[meta.id] = format_id(existing->id);
                return;
            }
            throw;
        }

        apply_meta(created.id);
        const std::string new_id = format_id(created.id);
        id_map[meta.id] = new_id;
        ConversationEntry e = meta;
        e.id = new_id;
        e.total_tokens = tokens;
        imported.push_back(std::move(e));
    };

    auto import_sessions_dir = [&]() {
        const std::string sessions_dir = config_dir_ + "/sessions";
        if (!fs::exists(sessions_dir)) return;

        const std::string current_cwd = fs::current_path().string();
        // Match prior ConversationStore::cwd_session_hash (FNV-1a 32-bit).
        auto cwd_hash = [](const std::string& cwd) {
            std::uint32_t h = 2166136261u;
            for (unsigned char c : cwd) {
                h ^= c;
                h *= 16777619u;
            }
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x", h);
            return std::string(buf);
        };
        const std::string current_hash = cwd_hash(current_cwd);
        std::string preferred_legacy;

        for (const auto& ent : fs::directory_iterator(sessions_dir)) {
            if (!ent.is_regular_file()) continue;
            const std::string name = ent.path().filename().string();
            if (name.size() < 6 || name.substr(name.size() - 5) != ".json")
                continue;

            const std::string hash = name.substr(0, name.size() - 5);
            ConversationEntry e;
            e.id = "session:" + hash; // stable legacy key
            e.title = "Untitled";
            e.cwd = (hash == current_hash) ? current_cwd : ("session:" + hash);
            e.created_at = now_epoch();
            e.updated_at = e.created_at;
            const std::string body = read_file(ent.path().string());
            import_one(e, body);
            if (hash == current_hash) preferred_legacy = e.id;
        }

        if (!preferred_legacy.empty()) {
            auto it = id_map.find(preferred_legacy);
            if (it != id_map.end()) {
                tenants_.set_tui_active_conversation(
                    tenant_id_, parse_id(it->second));
            }
        }
    };

    const std::string manifest_path = legacy_store_dir_ + "/manifest.json";
    const bool legacy_dir_exists = fs::exists(legacy_store_dir_);

    auto conversations_has_importable = [&]() -> bool {
        if (!legacy_dir_exists) return false;
        if (fs::exists(manifest_path)) return true;
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator(legacy_store_dir_, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            const std::string name = ent.path().filename().string();
            if (name.size() >= 6 && name.substr(name.size() - 5) == ".json"
                && name != "manifest.json" && name != "layout.json") {
                return true;
            }
        }
        return false;
    };

    if (!conversations_has_importable()) {
        // No JSON global-store threads to lift — still pull legacy
        // per-cwd sessions/*.json when present (even if conversations/
        // exists with only layout.json / chrome files).
        import_sessions_dir();
        if (legacy_dir_exists) {
            atomic_write_file(legacy_store_dir_ + "/.migrated_to_sqlite", "1\n");
        }
        tenants_.mark_tui_conversations_migrated(tenant_id_);
        return;
    }

    // Prefer manifest; fall back to scanning session files.
    bool used_manifest = false;
    if (fs::exists(manifest_path)) {
        const std::string raw = read_file(manifest_path);
        try {
            auto root = json_parse(raw);
            if (root && root->is_object()) {
                auto arr = root->get("conversations");
                if (arr && arr->is_array()) {
                    used_manifest = true;
                    for (const auto& v : arr->as_array()) {
                        if (!v || !v->is_object()) continue;
                        ConversationEntry e;
                        e.id = v->get_string("id");
                        e.title = v->get_string("title", "Untitled");
                        e.cwd = v->get_string("cwd");
                        e.created_at =
                            static_cast<std::int64_t>(v->get_number("created_at"));
                        e.updated_at =
                            static_cast<std::int64_t>(v->get_number("updated_at"));
                        e.deleted_at =
                            static_cast<std::int64_t>(v->get_number("deleted_at"));
                        e.total_tokens =
                            static_cast<int>(v->get_number("total_tokens"));
                        e.titled = v->get_bool("titled", false);
                        if (e.id.empty()) continue;
                        const std::string body =
                            read_file(legacy_store_dir_ + "/" + e.id + ".json");
                        import_one(e, body);
                    }
                }
            }
        } catch (...) {
            used_manifest = false;
        }
    }

    if (!used_manifest) {
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator(legacy_store_dir_, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            const std::string name = ent.path().filename().string();
            if (name == "manifest.json" || name == "layout.json" || name == "active"
                || name == ".migrated_to_sqlite"
                || name.size() < 6
                || name.substr(name.size() - 5) != ".json") {
                continue;
            }
            ConversationEntry e;
            e.id = name.substr(0, name.size() - 5);
            e.title = "Untitled (recovered)";
            e.created_at = now_epoch();
            e.updated_at = e.created_at;
            import_one(e, read_file(ent.path().string()));
        }
    }

    // Rewrite layout conversation ids.
    const std::string layout_path = layout_snapshot_path(config_dir_);
    if (auto snap = load_layout_snapshot(layout_path)) {
        for_each_layout_leaf(snap->root, [&](LayoutSnapshot::Node& leaf) {
            auto it = id_map.find(leaf.conversation_id);
            if (it != id_map.end()) leaf.conversation_id = it->second;
        });
        const std::string layout_json = layout_snapshot_to_json(*snap);
        tenants_.set_tui_layout_json(tenant_id_, layout_json);
        save_layout_snapshot(layout_path, *snap);
    } else {
        const std::string existing = tenants_.get_tui_layout_json(tenant_id_);
        if (!existing.empty()) {
            if (auto snap2 = layout_snapshot_from_json(existing)) {
                for_each_layout_leaf(snap2->root, [&](LayoutSnapshot::Node& leaf) {
                    auto it = id_map.find(leaf.conversation_id);
                    if (it != id_map.end()) leaf.conversation_id = it->second;
                });
                tenants_.set_tui_layout_json(tenant_id_,
                                             layout_snapshot_to_json(*snap2));
            }
        }
    }

    // Active pointer.
    std::string active = read_file(legacy_store_dir_ + "/active");
    while (!active.empty()
           && (active.back() == '\n' || active.back() == '\r')) {
        active.pop_back();
    }
    auto ait = id_map.find(active);
    if (ait != id_map.end()) {
        tenants_.set_tui_active_conversation(tenant_id_, parse_id(ait->second));
    } else if (!imported.empty()) {
        for (const auto& e : imported) {
            if (e.deleted_at == 0) {
                tenants_.set_tui_active_conversation(tenant_id_, parse_id(e.id));
                break;
            }
        }
    }

    atomic_write_file(legacy_store_dir_ + "/.migrated_to_sqlite", "1\n");
    tenants_.mark_tui_conversations_migrated(tenant_id_);
}

void ConversationStore::migrate_legacy_conv_tool_scope() {
    const std::string sessions_dir = config_dir_ + "/sessions";
    if (!fs::exists(sessions_dir)) return;

    auto cwd_hash = [](const std::string& cwd) {
        std::uint32_t h = 2166136261u;
        for (unsigned char c : cwd) {
            h ^= c;
            h *= 16777619u;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", h);
        return std::string(buf);
    };
    const std::string current_hash = cwd_hash(fs::current_path().string());
    const int64_t active_db = parse_id(active_id_);

    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(sessions_dir, ec)) {
        if (ec) break;
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() < 6 || name.substr(name.size() - 5) != ".conv")
            continue;

        const std::string hash = name.substr(0, name.size() - 5);
        const std::string path = ent.path().string();

        int64_t from_id = 0;
        {
            std::ifstream mf(path);
            if (mf) mf >> from_id;
        }
        if (from_id <= 0) {
            std::error_code ren_ec;
            fs::rename(path, path + ".migrated", ren_ec);
            continue;
        }

        auto from = tenants_.get_conversation(tenant_id_, from_id);
        if (!from) {
            std::error_code ren_ec;
            fs::rename(path, path + ".migrated", ren_ec);
            continue;
        }

        int64_t to_id = 0;
        if (hash == current_hash && active_db > 0) {
            to_id = active_db;
        } else if (auto leg =
                       tenants_.find_tui_by_legacy_id(tenant_id_,
                                                      "session:" + hash)) {
            to_id = leg->id;
        } else if (auto leg = tenants_.find_tui_by_legacy_id(
                       tenant_id_, "session:" + hash + ":tool-scope")) {
            to_id = leg->id;
        } else {
            // Preserve scoped tool data under a dedicated TUI thread so it
            // remains reachable from the sidebar after the dual-id removal.
            const std::string cwd =
                (hash == current_hash) ? fs::current_path().string()
                                       : ("session:" + hash);
            auto created = tenants_.create_tui_conversation(
                tenant_id_, "Untitled", cwd, empty_session_json(),
                "session:" + hash + ":tool-scope");
            to_id = created.id;
        }

        if (to_id <= 0) continue;

        if (to_id != from_id) {
            if (!tenants_.reassign_conversation_scoped_data(
                    tenant_id_, from_id, to_id)) {
                // Leave the .conv marker so a later launch can retry.
                continue;
            }
            // Drop the legacy API "TUI session" holder — scoped rows now
            // live on the unified TUI thread and HTTP should not keep a
            // duplicate conversation resource around.
            if (from->origin != "tui") {
                tenants_.delete_conversation_force(tenant_id_, from_id);
            }
        }

        std::error_code ren_ec;
        fs::rename(path, path + ".migrated", ren_ec);
    }
}

void ConversationStore::ensure_initialized() {
    migrate_json_store_if_needed();
    reload_entries_unlocked();

    const int64_t active_db = tenants_.get_tui_active_conversation(tenant_id_);
    if (active_db > 0) active_id_ = format_id(active_db);

    gc_stale_empty_unlocked();

    auto has_id = [&](const std::string& id) {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&](const ConversationEntry& e) {
                               return e.id == id && e.deleted_at == 0;
                           });
    };

    if (active_id_.empty() || !has_id(active_id_)) {
        if (!entries_.empty()) {
            // First non-deleted.
            active_id_.clear();
            for (const auto& e : entries_) {
                if (e.deleted_at == 0) {
                    active_id_ = e.id;
                    break;
                }
            }
            if (active_id_.empty()) {
                active_id_ = create(fs::current_path().string());
            } else {
                set_active_unlocked(active_id_);
            }
        } else {
            active_id_ = create(fs::current_path().string());
        }
    }

    // After an active TUI thread exists, fold legacy per-cwd tool scope.
    migrate_legacy_conv_tool_scope();
    reload_entries_unlocked();
    if (active_id_.empty() || !has_id(active_id_)) {
        for (const auto& e : entries_) {
            if (e.deleted_at == 0) {
                active_id_ = e.id;
                set_active_unlocked(active_id_);
                break;
            }
        }
    }
}

void ConversationStore::gc_stale_empty_unlocked() {
    const std::int64_t cutoff = now_epoch() - 24 * 3600;
    std::vector<ConversationEntry> keep;
    keep.reserve(entries_.size());
    bool changed = false;
    for (auto& e : entries_) {
        const bool stale = e.deleted_at == 0
            && e.id != active_id_
            && e.title == "Untitled"
            && e.created_at < cutoff
            && session_is_empty_unlocked(e.id);
        if (stale) {
            const int64_t cid = parse_id(e.id);
            if (cid > 0) tenants_.delete_conversation_force(tenant_id_, cid);
            changed = true;
            continue;
        }
        keep.push_back(std::move(e));
    }
    entries_ = std::move(keep);
    (void)changed;
}

std::vector<ConversationEntry> ConversationStore::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConversationEntry> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.deleted_at == 0) out.push_back(e);
    }
    return out;
}

std::vector<ConversationSearchHit>
ConversationStore::search(const std::string& term, size_t max_hits) const {
    std::vector<ConversationSearchHit> out;
    if (term.empty()) return out;
    const std::string needle_lc = lowercase(term);

    std::vector<ConversationEntry> entries = list();
    for (const auto& e : entries) {
        if (out.size() >= max_hits) break;
        const std::string raw = session_json(e.id);
        if (raw.empty()) continue;
        std::shared_ptr<JsonValue> root;
        try {
            root = json_parse(raw);
        } catch (...) {
            continue;
        }
        if (!root || !root->is_object()) continue;

        ConversationSearchHit hit;
        hit.match_count = count_session_matches(*root, needle_lc, &hit.snippet);
        hit.match_count += count_matches(e.title, needle_lc, &hit.snippet);
        if (hit.match_count == 0) continue;
        hit.id = e.id;
        hit.title = e.title;
        out.push_back(std::move(hit));
    }
    return out;
}

std::optional<std::string> ConversationStore::cwd_of(const std::string& id) const {
    if (id.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : entries_) {
        if (e.deleted_at != 0) continue;
        if (e.id == id) return e.cwd;
    }
    return std::nullopt;
}

std::string ConversationStore::resolved_workspace_root(const std::string& id,
                                                       std::string* err) const {
    auto fail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        return std::string();
    };
    auto stored = cwd_of(id);
    if (!stored) {
        return fail("conversation not found");
    }
    if (stored->empty()) {
        return fail("conversation has no workspace directory");
    }
    // commands.h — shared with /write and /diff apply.
    return canonical_workspace_root(*stored, err);
}

std::string ConversationStore::active_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_id_;
}

std::string ConversationStore::session_path(const std::string&) const {
    return {};
}

std::string ConversationStore::session_json(const std::string& id) const {
    const int64_t cid = parse_id(id);
    if (cid <= 0) return {};
    return tenants_.get_conversation_session_json(tenant_id_, cid);
}

std::string ConversationStore::create_unlocked(const std::string& cwd,
                                               const std::string& folder_id) {
    int64_t fid = parse_id(folder_id);
    // Stale folder ids (e.g. after delete) must not abort the TUI — file
    // the new conversation as unfiled instead.
    if (fid > 0 && !tenants_.get_conversation_folder(tenant_id_, fid)) {
        fid = 0;
    }
    auto created = tenants_.create_tui_conversation(
        tenant_id_, "Untitled", cwd, empty_session_json(),
        /*legacy_id=*/"", fid);
    ConversationEntry e = entry_from_row(created);
    entries_.insert(entries_.begin(), e);
    set_active_unlocked(e.id);
    return e.id;
}

std::string ConversationStore::create(const std::string& cwd,
                                      const std::string& folder_id) {
    std::lock_guard<std::mutex> lk(mu_);
    return create_unlocked(cwd, folder_id);
}

bool ConversationStore::session_is_empty_unlocked(const std::string& id) const {
    if (id.empty()) return true;
    return session_json_is_empty(
        tenants_.get_conversation_session_json(tenant_id_, parse_id(id)));
}

std::string ConversationStore::create_or_reuse(const std::string& cwd,
                                               const std::string& folder_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto bind_cwd = [&](const std::string& id) {
        if (id.empty() || cwd.empty()) return;
        if (!tenants_.update_tui_conversation(
                tenant_id_, parse_id(id), "", cwd, -1, -1, -1))
            return;
        for (auto& e : entries_) {
            if (e.id == id) e.cwd = cwd;
        }
    };
    if (session_is_empty_unlocked(active_id_)) {
        // Match create(): empty folder_id means unfiled, even when reusing.
        const int64_t fid = folder_id.empty() ? 0 : parse_id(folder_id);
        if (folder_id.empty() || fid > 0) {
            if (tenants_.set_conversation_folder(
                    tenant_id_, parse_id(active_id_), fid)) {
                for (auto& e : entries_) {
                    if (e.id == active_id_) {
                        e.folder_id = folder_id.empty() ? std::string{}
                                                        : format_id(fid);
                    }
                }
            }
        }
        bind_cwd(active_id_);
        return active_id_;
    }
    return create_unlocked(cwd, folder_id);
}

std::string ConversationStore::create_or_reuse_for(
    const std::string& cwd, const std::string& prefer_id,
    const std::string& folder_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto bind_cwd = [&](const std::string& id) {
        if (id.empty() || cwd.empty()) return;
        if (!tenants_.update_tui_conversation(
                tenant_id_, parse_id(id), "", cwd, -1, -1, -1))
            return;
        for (auto& e : entries_) {
            if (e.id == id) e.cwd = cwd;
        }
    };
    auto apply_folder = [&](const std::string& id) {
        if (id.empty()) return;
        // Empty folder_id clears membership (unfiled); non-empty must parse.
        const int64_t fid = folder_id.empty() ? 0 : parse_id(folder_id);
        if (!folder_id.empty() && fid <= 0) return;
        if (!tenants_.set_conversation_folder(tenant_id_, parse_id(id), fid))
            return;
        for (auto& e : entries_) {
            if (e.id == id) {
                e.folder_id = folder_id.empty() ? std::string{}
                                                : format_id(fid);
            }
        }
    };

    if (!prefer_id.empty() && session_is_empty_unlocked(prefer_id)) {
        set_active_unlocked(prefer_id);
        apply_folder(prefer_id);
        bind_cwd(prefer_id);
        return prefer_id;
    }
    // Only when the caller has no prefer_id (no focused conversation) —
    // never when prefer_id already has turns, or a multi-pane "new chat"
    // can steal another pane's empty active conversation.
    if (prefer_id.empty()
        && !active_id_.empty()
        && session_is_empty_unlocked(active_id_)) {
        apply_folder(active_id_);
        bind_cwd(active_id_);
        return active_id_;
    }
    return create_unlocked(cwd, folder_id);
}

bool ConversationStore::load(const std::string& id, Orchestrator& orch) {
    const std::string raw = session_json(id);
    ConversationScope scope(id);
    if (raw.empty()) return false;
    return orch.load_session_json(raw);
}

void ConversationStore::save(const std::string& id, Orchestrator& orch) {
    ConversationScope scope(id);
    const std::string raw = orch.session_to_json();
    const int64_t cid = parse_id(id);
    if (cid <= 0) return;

    std::lock_guard<std::mutex> lk(mu_);
    tenants_.set_conversation_session_json(tenant_id_, cid, raw,
                                           /*bump_updated_at=*/true);
    const std::int64_t ts = now_epoch();
    for (auto& e : entries_) {
        if (e.id == id) {
            e.updated_at = ts;
            break;
        }
    }
    sort_entries(entries_);
}

void ConversationStore::set_active_unlocked(const std::string& id) {
    active_id_ = id;
    const int64_t cid = parse_id(id);
    if (cid > 0) tenants_.set_tui_active_conversation(tenant_id_, cid);
}

void ConversationStore::set_active(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    set_active_unlocked(id);
}

void ConversationStore::set_title(const std::string& id, const std::string& title) {
    if (title.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            if (e.titled) return;
            e.title = title;
            e.updated_at = now_epoch();
            tenants_.update_tui_conversation(
                tenant_id_, parse_id(id), title, "", -1, -1, -1);
            break;
        }
    }
    sort_entries(entries_);
}

void ConversationStore::set_title_locked(const std::string& id,
                                         const std::string& title) {
    if (title.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            e.title = title;
            e.titled = true;
            e.updated_at = now_epoch();
            tenants_.update_tui_conversation(
                tenant_id_, parse_id(id), title, "", 1, -1, -1);
            break;
        }
    }
    sort_entries(entries_);
}

void ConversationStore::add_tokens(const std::string& id, int delta) {
    if (id.empty() || delta == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            const long long next =
                static_cast<long long>(e.total_tokens) + delta;
            e.total_tokens = static_cast<int>(std::max(0LL, next));
            tenants_.add_conversation_tokens(tenant_id_, parse_id(id), delta);
            return;
        }
    }
}

void ConversationStore::lock_title(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            e.titled = true;
            tenants_.update_tui_conversation(
                tenant_id_, parse_id(id), "", "", 1, -1, -1);
            break;
        }
    }
}

bool ConversationStore::is_titled(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : entries_) {
        if (e.id == id) return e.titled;
    }
    return false;
}

void ConversationStore::remove_and_reassign_active_unlocked(const std::string& id,
                                                             bool hard_delete) {
    const int64_t cid = parse_id(id);
    if (hard_delete) {
        if (cid > 0) tenants_.delete_conversation_force(tenant_id_, cid);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const ConversationEntry& e) {
                                          return e.id == id;
                                      }),
                       entries_.end());
    } else if (cid > 0) {
        tenants_.soft_delete_conversation(tenant_id_, cid);
    }

    if (active_id_ != id) return;

    for (const auto& e : entries_) {
        if (e.id != id && e.deleted_at == 0) {
            set_active_unlocked(e.id);
            return;
        }
    }
    create_unlocked(fs::current_path().string());
}

void ConversationStore::soft_delete(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            e.deleted_at = now_epoch();
            break;
        }
    }
    remove_and_reassign_active_unlocked(id, /*hard_delete=*/false);
}

void ConversationStore::purge(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    remove_and_reassign_active_unlocked(id, /*hard_delete=*/true);
}

std::vector<ConversationFolderEntry> ConversationStore::list_folders() const {
    std::vector<ConversationFolderEntry> out;
    for (const auto& f : tenants_.list_conversation_folders(tenant_id_)) {
        out.push_back(folder_from_row(f));
    }
    return out;
}

std::string ConversationStore::create_folder(const std::string& name) {
    auto f = tenants_.create_conversation_folder(tenant_id_, name);
    return format_id(f.id);
}

bool ConversationStore::rename_folder(const std::string& id,
                                      const std::string& name) {
    return tenants_.update_conversation_folder(
        tenant_id_, parse_id(id), name, /*new_position=*/-1);
}

bool ConversationStore::delete_folder(const std::string& id) {
    const int64_t fid = parse_id(id);
    if (fid <= 0) return false;
    if (!tenants_.delete_conversation_folder(tenant_id_, fid)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.folder_id == id) e.folder_id.clear();
    }
    return true;
}

bool ConversationStore::move_to_folder(const std::string& conversation_id,
                                       const std::string& folder_id) {
    const int64_t cid = parse_id(conversation_id);
    if (cid <= 0) return false;
    const int64_t fid = folder_id.empty() ? 0 : parse_id(folder_id);
    if (!folder_id.empty() && fid <= 0) return false;
    if (!tenants_.set_conversation_folder(tenant_id_, cid, fid)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == conversation_id) {
            e.folder_id = folder_id.empty() ? std::string{} : format_id(fid);
            break;
        }
    }
    return true;
}

std::string ConversationStore::folder_collapse_json() const {
    return tenants_.get_tui_folder_collapse_json(tenant_id_);
}

void ConversationStore::set_folder_collapse_json(const std::string& json) {
    tenants_.set_tui_folder_collapse_json(tenant_id_, json);
}

void ConversationStore::enqueue_title_job(const std::string& id,
                                          const std::string& user_msg,
                                          const std::string& assistant_msg,
                                          const std::string& model,
                                          Orchestrator& orch) {
    if (is_titled(id)) return;
    {
        std::lock_guard<std::mutex> lk(title_mu_);
        title_queue_.push_back(
            TitleJob{id, user_msg, assistant_msg, model, &orch});
    }
    title_cv_.notify_all();
}

void ConversationStore::title_worker_loop() {
    for (;;) {
        TitleJob job;
        {
            std::unique_lock<std::mutex> lk(title_mu_);
            title_cv_.wait(lk, [&] {
                return !title_queue_.empty() || title_stop_;
            });
            if (!title_queue_.empty()) {
                job = std::move(title_queue_.front());
                title_queue_.pop_front();
            } else {
                return;
            }
        }
        if (!is_titled(job.id)) run_title_job(job);
    }
}

namespace {

std::string build_title_prompt(const std::string& user_msg,
                               const std::string& assistant_msg) {
    constexpr size_t kMaxTotal = 2000;
    // Prefer user-only prompts (parallel with the first turn). Include the
    // assistant reply when callers still have it (legacy / post-turn path).
    std::string prompt = "User: " + user_msg;
    if (!assistant_msg.empty()) {
        prompt += "\n\nAssistant: " + assistant_msg;
    }
    if (prompt.size() > kMaxTotal) prompt.resize(kMaxTotal);
    return prompt;
}

} // namespace

void ConversationStore::run_title_job(const TitleJob& job) {
    const std::string prompt = build_title_prompt(job.user_msg, job.assistant_msg);
    const std::string model = job.model;

    std::shared_ptr<ApiClient> client = job.orch->make_side_client();

    auto prom = std::make_shared<std::promise<std::string>>();
    std::future<std::string> fut = prom->get_future();
    std::thread worker([client, model, prompt, prom]() {
        ApiRequest req;
        req.model = model;
        req.system_prompt =
            "Reply with only a 3-6 word title for this conversation. "
            "No quotes, no punctuation at the end.";
        req.max_tokens = 32;
        req.temperature = 0.3;
        req.messages = {Message{"user", prompt}};
        ApiResponse resp = client->complete(req);
        try {
            prom->set_value(resp.ok ? resp.content : std::string());
        } catch (...) {
        }
    });
    worker.detach();

    std::string raw;
    if (fut.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
        raw = fut.get();
    } else {
        client->cancel();
    }

    const std::string sanitized = sanitize_model_title(raw);
    if (!sanitized.empty()) {
        set_title_locked(job.id, sanitized);
    } else {
        lock_title(job.id);
    }
}

} // namespace arbiter
