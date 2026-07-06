#include "repl/conversation_store.h"

#include "json.h"
#include "orchestrator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace arbiter {

namespace {

std::int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string new_conversation_id() {
    static std::atomic<std::uint64_t> seq{0};
    const std::uint64_t t = static_cast<std::uint64_t>(now_epoch());
    const std::uint64_t n = ++seq;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx%08llx",
                  static_cast<unsigned long long>(t),
                  static_cast<unsigned long long>(n & 0xffffffffu));
    return buf;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path);
    if (f) f << data;
}

std::string cwd_session_hash(const std::string& cwd) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : cwd) {
        h ^= c;
        h *= 16777619u;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", h);
    return buf;
}

} // namespace

ConversationStore::ConversationStore(std::string config_dir)
    : config_dir_(std::move(config_dir)),
      store_dir_(config_dir_ + "/conversations") {
    ensure_initialized();
}

void ConversationStore::ensure_initialized() {
    fs::create_directories(store_dir_);

    const std::string manifest_path = store_dir_ + "/manifest.json";
    if (!fs::exists(manifest_path)) {
        migrate_legacy_sessions();
    }

    load_manifest();

    active_id_ = read_file(store_dir_ + "/active");
    while (!active_id_.empty()
           && (active_id_.back() == '\n' || active_id_.back() == '\r')) {
        active_id_.pop_back();
    }

    auto has_id = [&](const std::string& id) {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&](const ConversationEntry& e) { return e.id == id; });
    };

    if (active_id_.empty() || !has_id(active_id_)) {
        if (!entries_.empty()) {
            active_id_ = entries_.front().id;
        } else {
            active_id_ = create(fs::current_path().string());
            return;
        }
        set_active(active_id_);
    }
}

void ConversationStore::migrate_legacy_sessions() {
    const std::string sessions_dir = config_dir_ + "/sessions";
    if (!fs::exists(sessions_dir)) return;

    const std::string current_cwd = fs::current_path().string();
    const std::string current_hash = cwd_session_hash(current_cwd);
    std::string preferred_id;

    for (const auto& ent : fs::directory_iterator(sessions_dir)) {
        if (!ent.is_regular_file()) continue;
        const std::string name = ent.path().filename().string();
        if (name.size() < 6 || name.substr(name.size() - 5) != ".json") continue;

        const std::string hash = name.substr(0, name.size() - 5);
        const std::string cwd_hint =
            (hash == current_hash) ? current_cwd : ("session:" + hash);
        const bool prefer = (hash == current_hash);
        const std::string id =
            import_legacy_file(ent.path().string(), cwd_hint, prefer && preferred_id.empty());
        if (prefer && !id.empty()) preferred_id = id;
    }

    if (!preferred_id.empty()) set_active(preferred_id);
}

std::string ConversationStore::import_legacy_file(const std::string& src_path,
                                                  const std::string& cwd_hint,
                                                  bool set_active_flag) {
    const std::string raw = read_file(src_path);
    if (raw.empty()) return {};

    const std::string id = new_conversation_id();
    const std::string dst = session_path(id);
    write_file(dst, raw);

    ConversationEntry e;
    e.id = id;
    e.title = "Untitled";
    e.cwd = cwd_hint;
    e.created_at = now_epoch();
    e.updated_at = e.created_at;
    entries_.push_back(e);

    if (set_active_flag) active_id_ = id;

    save_manifest();
    if (set_active_flag) write_file(store_dir_ + "/active", active_id_ + "\n");
    return id;
}

void ConversationStore::load_manifest() {
    entries_.clear();
    const std::string raw = read_file(store_dir_ + "/manifest.json");
    if (raw.empty()) return;

    try {
        auto root = json_parse(raw);
        if (!root || !root->is_object()) return;
        auto arr = root->get("conversations");
        if (!arr || !arr->is_array()) return;
        for (const auto& v : arr->as_array()) {
            if (!v || !v->is_object()) continue;
            ConversationEntry e;
            e.id = v->get_string("id");
            e.title = v->get_string("title", "Untitled");
            e.cwd = v->get_string("cwd");
            e.created_at = static_cast<std::int64_t>(v->get_number("created_at"));
            e.updated_at = static_cast<std::int64_t>(v->get_number("updated_at"));
            if (e.id.empty()) continue;
            entries_.push_back(std::move(e));
        }
    } catch (...) {
        entries_.clear();
    }

    std::sort(entries_.begin(), entries_.end(),
              [](const ConversationEntry& a, const ConversationEntry& b) {
                  return a.updated_at > b.updated_at;
              });
}

void ConversationStore::save_manifest() const {
    JsonArray arr;
    for (const auto& e : entries_) {
        auto obj = jobj();
        auto& m = obj->as_object_mut();
        m["id"] = jstr(e.id);
        m["title"] = jstr(e.title);
        m["cwd"] = jstr(e.cwd);
        m["created_at"] = jnum(static_cast<double>(e.created_at));
        m["updated_at"] = jnum(static_cast<double>(e.updated_at));
        arr.push_back(obj);
    }
    auto root = jobj();
    root->as_object_mut()["conversations"] = jarr(std::move(arr));
    write_file(store_dir_ + "/manifest.json", json_serialize(*root));
}

std::vector<ConversationEntry> ConversationStore::list() const {
    return entries_;
}

std::string ConversationStore::session_path(const std::string& id) const {
    return store_dir_ + "/" + id + ".json";
}

std::string ConversationStore::create(const std::string& cwd) {
    const std::string id = new_conversation_id();
    const std::int64_t ts = now_epoch();

    ConversationEntry e;
    e.id = id;
    e.title = "Untitled";
    e.cwd = cwd;
    e.created_at = ts;
    e.updated_at = ts;
    entries_.insert(entries_.begin(), e);

    auto empty = jobj();
    empty->as_object_mut()["version"] = jnum(1);
    empty->as_object_mut()["index"] = jarr();
    empty->as_object_mut()["agents"] = jobj();
    write_file(session_path(id), json_serialize(*empty));

    save_manifest();
    set_active(id);
    return id;
}

bool ConversationStore::load(const std::string& id, Orchestrator& orch) {
    return orch.load_session(session_path(id));
}

void ConversationStore::save(const std::string& id, Orchestrator& orch) {
    orch.save_session(session_path(id));
    const std::int64_t ts = now_epoch();
    for (auto& e : entries_) {
        if (e.id == id) {
            e.updated_at = ts;
            break;
        }
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const ConversationEntry& a, const ConversationEntry& b) {
                  return a.updated_at > b.updated_at;
              });
    save_manifest();
}

void ConversationStore::set_active(const std::string& id) {
    active_id_ = id;
    write_file(store_dir_ + "/active", id + "\n");
}

void ConversationStore::set_title(const std::string& id, const std::string& title) {
    if (title.empty()) return;
    for (auto& e : entries_) {
        if (e.id == id) {
            e.title = title;
            e.updated_at = now_epoch();
            break;
        }
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const ConversationEntry& a, const ConversationEntry& b) {
                  return a.updated_at > b.updated_at;
              });
    save_manifest();
}

} // namespace arbiter
