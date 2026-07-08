#include "repl/conversation_store.h"

#include "atomic_file.h"
#include "json.h"
#include "orchestrator.h"
#include "repl/conversation_titling.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
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

void sort_entries(std::vector<ConversationEntry>& entries) {
    // updated_at has second resolution, so two conversations touched in the
    // same second tie constantly (e.g. save-then-create during a switch).
    // Without a tie-break, std::sort's unspecified ordering of equal keys
    // makes the history sidebar's row order nondeterministic — keyboard
    // navigation in tests (and muscle memory in real use) lands on the
    // wrong row.  Ids are fixed-width hex of (epoch seconds, per-process
    // counter), so comparing them descending breaks ties by creation order.
    std::sort(entries.begin(), entries.end(),
              [](const ConversationEntry& a, const ConversationEntry& b) {
                  if (a.updated_at != b.updated_at) {
                      return a.updated_at > b.updated_at;
                  }
                  return a.id > b.id;
              });
}

} // namespace

ConversationStore::ConversationStore(std::string config_dir)
    : config_dir_(std::move(config_dir)),
      store_dir_(config_dir_ + "/conversations") {
    ensure_initialized();
    save_thread_ = std::thread(&ConversationStore::save_worker_loop, this);
    title_thread_ = std::thread(&ConversationStore::title_worker_loop, this);
}

ConversationStore::~ConversationStore() {
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        stop_ = true;
    }
    async_cv_.notify_all();
    if (save_thread_.joinable()) save_thread_.join();

    // Titling is best-effort metadata, not durability-critical like saves —
    // don't make shutdown wait on a queue of network calls. Any job already
    // mid-flight when we set the flag finishes on its own (see
    // run_title_job's timeout comment for why that's an acceptable risk).
    {
        std::lock_guard<std::mutex> lk(title_mu_);
        title_stop_ = true;
    }
    title_cv_.notify_all();
    if (title_thread_.joinable()) title_thread_.join();
}

void ConversationStore::save_worker_loop() {
    for (;;) {
        std::string id;
        Orchestrator* orch = nullptr;
        {
            std::unique_lock<std::mutex> lk(async_mu_);
            async_cv_.wait(lk, [&] { return pending_ || stop_; });
            if (pending_) {
                id = pending_id_;
                orch = pending_orch_;
                pending_ = false;
                pending_orch_ = nullptr;
                busy_ = true;
            } else {
                return;
            }
        }

        save(id, *orch);

        {
            std::lock_guard<std::mutex> lk(async_mu_);
            busy_ = false;
        }
        async_cv_.notify_all();
    }
}

void ConversationStore::save_async(const std::string& id, Orchestrator& orch) {
    {
        std::lock_guard<std::mutex> lk(async_mu_);
        pending_id_ = id;
        pending_orch_ = &orch;
        pending_ = true;
    }
    async_cv_.notify_all();
}

void ConversationStore::flush() {
    std::unique_lock<std::mutex> lk(async_mu_);
    async_cv_.wait(lk, [&] { return !pending_ && !busy_; });
}

void ConversationStore::ensure_initialized() {
    fs::create_directories(store_dir_);

    const std::string manifest_path = store_dir_ + "/manifest.json";
    if (!fs::exists(manifest_path)) {
        migrate_legacy_sessions();
    }

    load_manifest_unlocked();

    active_id_ = read_file(store_dir_ + "/active");
    while (!active_id_.empty()
           && (active_id_.back() == '\n' || active_id_.back() == '\r')) {
        active_id_.pop_back();
    }

    gc_stale_empty_unlocked();

    auto has_id = [&](const std::string& id) {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&](const ConversationEntry& e) { return e.id == id; });
    };

    if (active_id_.empty() || !has_id(active_id_)) {
        if (!entries_.empty()) {
            active_id_ = entries_.front().id;
        } else {
            // Single-threaded constructor context: no lock is held, so it is
            // safe to call the lock-taking public create() here (unlike the
            // old code, which called this same path from inside create()
            // itself while already holding mu_ — the original deadlock).
            active_id_ = create(fs::current_path().string());
            return;
        }
        set_active_unlocked(active_id_);
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

    if (!preferred_id.empty()) set_active_unlocked(preferred_id);
}

std::string ConversationStore::import_legacy_file(const std::string& src_path,
                                                  const std::string& cwd_hint,
                                                  bool set_active_flag) {
    const std::string raw = read_file(src_path);
    if (raw.empty()) return {};

    const std::string id = new_conversation_id();
    const std::string dst = session_path_unlocked(id);
    atomic_write_file(dst, raw);

    ConversationEntry e;
    e.id = id;
    e.title = "Untitled";
    e.cwd = cwd_hint;
    e.created_at = now_epoch();
    e.updated_at = e.created_at;
    entries_.push_back(e);

    if (set_active_flag) active_id_ = id;

    save_manifest_unlocked();
    if (set_active_flag) atomic_write_file(store_dir_ + "/active", active_id_ + "\n");
    return id;
}

void ConversationStore::load_manifest_unlocked() {
    entries_.clear();
    const std::string raw = read_file(store_dir_ + "/manifest.json");
    if (raw.empty()) return;

    bool parsed_ok = false;
    try {
        auto root = json_parse(raw);
        if (root && root->is_object()) {
            auto arr = root->get("conversations");
            if (arr && arr->is_array()) {
                for (const auto& v : arr->as_array()) {
                    if (!v || !v->is_object()) continue;
                    ConversationEntry e;
                    e.id = v->get_string("id");
                    e.title = v->get_string("title", "Untitled");
                    e.cwd = v->get_string("cwd");
                    e.created_at = static_cast<std::int64_t>(v->get_number("created_at"));
                    e.updated_at = static_cast<std::int64_t>(v->get_number("updated_at"));
                    e.deleted_at = static_cast<std::int64_t>(v->get_number("deleted_at"));
                    e.titled = v->get_bool("titled", false);
                    if (e.id.empty()) continue;
                    entries_.push_back(std::move(e));
                }
                parsed_ok = true;
            }
        }
    } catch (...) {
        parsed_ok = false;
    }

    if (!parsed_ok) {
        // A corrupt manifest must not orphan every session file on disk.
        // Rebuild entries by scanning the store directory for session JSON.
        entries_.clear();
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator(store_dir_, ec)) {
            if (ec) break;
            if (!ent.is_regular_file()) continue;
            const std::string name = ent.path().filename().string();
            if (name == "manifest.json" || name.size() < 6
                || name.substr(name.size() - 5) != ".json") {
                continue;
            }
            ConversationEntry e;
            e.id = name.substr(0, name.size() - 5);
            e.title = "Untitled (recovered)";
            std::error_code time_ec;
            const auto mtime = fs::last_write_time(ent.path(), time_ec);
            std::int64_t ts = now_epoch();
            if (!time_ec) {
                const auto sys_time = std::chrono::file_clock::to_sys(mtime);
                ts = std::chrono::duration_cast<std::chrono::seconds>(
                         sys_time.time_since_epoch())
                         .count();
            }
            e.created_at = ts;
            e.updated_at = ts;
            entries_.push_back(std::move(e));
        }
        save_manifest_unlocked();
    }

    sort_entries(entries_);
}

void ConversationStore::save_manifest_unlocked() const {
    JsonArray arr;
    for (const auto& e : entries_) {
        auto obj = jobj();
        auto& m = obj->as_object_mut();
        m["id"] = jstr(e.id);
        m["title"] = jstr(e.title);
        m["cwd"] = jstr(e.cwd);
        m["created_at"] = jnum(static_cast<double>(e.created_at));
        m["updated_at"] = jnum(static_cast<double>(e.updated_at));
        if (e.deleted_at != 0) m["deleted_at"] = jnum(static_cast<double>(e.deleted_at));
        if (e.titled) m["titled"] = jbool(true);
        arr.push_back(obj);
    }
    auto root = jobj();
    root->as_object_mut()["conversations"] = jarr(std::move(arr));
    atomic_write_file(store_dir_ + "/manifest.json", json_serialize(*root));
}

bool ConversationStore::session_is_empty_unlocked(const std::string& id) const {
    if (id.empty()) return true;
    const std::string raw = read_file(session_path_unlocked(id));
    if (raw.empty()) return true;
    try {
        auto root = json_parse(raw);
        if (!root || !root->is_object()) return true;
        auto idx = root->get("index");
        if (idx && idx->is_array() && !idx->as_array().empty()) return false;
        auto agents = root->get("agents");
        if (agents && agents->is_object() && !agents->as_object().empty()) return false;
        return true;
    } catch (...) {
        return true;
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
            std::error_code ec;
            fs::remove(session_path_unlocked(e.id), ec);
            changed = true;
            continue;
        }
        keep.push_back(std::move(e));
    }
    // Always commit `keep` back, even when nothing was pruned: the loop
    // above moves every surviving entry out of entries_, so leaving the
    // reassignment conditional on `changed` would strand entries_ with
    // moved-from (blanked) elements whenever gc found nothing stale.
    entries_ = std::move(keep);
    if (changed) save_manifest_unlocked();
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

namespace {

std::string lowercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Count matches of `needle_lc` in `haystack` (both compared lowercase) and
// capture a flattened excerpt around the first one.
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
            const size_t end = std::min(haystack.size(), pos + needle_lc.size() + kAfter);
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

// Sum matches across every message array in a parsed session file.
int count_session_matches(const JsonValue& root, const std::string& needle_lc,
                          std::string* first_snippet) {
    int total = 0;
    auto scan_messages = [&](const JsonValue* arr) {
        if (!arr || !arr->is_array()) return;
        for (const auto& m : arr->as_array()) {
            if (!m || !m->is_object()) continue;
            total += count_matches(m->get_string("content"), needle_lc, first_snippet);
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

} // namespace

std::vector<ConversationSearchHit>
ConversationStore::search(const std::string& term, size_t max_hits) const {
    std::vector<ConversationSearchHit> out;
    if (term.empty()) return out;
    const std::string needle_lc = lowercase(term);

    // Snapshot entries under the lock; read session files outside it so a
    // large store doesn't stall saves/renames for the whole scan.
    std::vector<ConversationEntry> entries = list();

    for (const auto& e : entries) {
        if (out.size() >= max_hits) break;
        const std::string raw = read_file(session_path(e.id));
        if (raw.empty()) continue;
        std::shared_ptr<JsonValue> root;
        try {
            root = json_parse(raw);
        } catch (...) {
            continue;   // corrupt/mid-write session — skip, don't fail the search
        }
        if (!root || !root->is_object()) continue;

        ConversationSearchHit hit;
        hit.match_count = count_session_matches(*root, needle_lc, &hit.snippet);
        // Title matches count too — a conversation renamed "curl timeout
        // notes" should surface for "curl" even if the transcript never
        // repeats the word.
        hit.match_count += count_matches(e.title, needle_lc, &hit.snippet);
        if (hit.match_count == 0) continue;
        hit.id = e.id;
        hit.title = e.title;
        out.push_back(std::move(hit));
    }
    return out;
}

std::string ConversationStore::active_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_id_;
}

std::string ConversationStore::session_path(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_path_unlocked(id);
}

std::string ConversationStore::create_unlocked(const std::string& cwd) {
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
    atomic_write_file(session_path_unlocked(id), json_serialize(*empty));

    save_manifest_unlocked();
    set_active_unlocked(id);
    return id;
}

std::string ConversationStore::create(const std::string& cwd) {
    std::lock_guard<std::mutex> lk(mu_);
    return create_unlocked(cwd);
}

std::string ConversationStore::create_or_reuse(const std::string& cwd) {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_is_empty_unlocked(active_id_)) {
        return active_id_;
    }
    return create_unlocked(cwd);
}

bool ConversationStore::load(const std::string& id, Orchestrator& orch) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        path = session_path_unlocked(id);
    }
    return orch.load_session(path);
}

void ConversationStore::save(const std::string& id, Orchestrator& orch) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        path = session_path_unlocked(id);
    }
    orch.save_session(path);
    std::lock_guard<std::mutex> lk(mu_);
    const std::int64_t ts = now_epoch();
    for (auto& e : entries_) {
        if (e.id == id) {
            e.updated_at = ts;
            break;
        }
    }
    sort_entries(entries_);
    save_manifest_unlocked();
}

void ConversationStore::set_active_unlocked(const std::string& id) {
    active_id_ = id;
    atomic_write_file(store_dir_ + "/active", id + "\n");
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
            if (e.titled) return; // locked — the deterministic path never overwrites a real title
            e.title = title;
            e.updated_at = now_epoch();
            break;
        }
    }
    sort_entries(entries_);
    save_manifest_unlocked();
}

void ConversationStore::set_title_locked(const std::string& id, const std::string& title) {
    if (title.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            e.title = title;
            e.titled = true;
            e.updated_at = now_epoch();
            break;
        }
    }
    sort_entries(entries_);
    save_manifest_unlocked();
}

void ConversationStore::lock_title(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        if (e.id == id) {
            e.titled = true;
            break;
        }
    }
    save_manifest_unlocked();
}

bool ConversationStore::is_titled(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : entries_) {
        if (e.id == id) return e.titled;
    }
    return false;
}

void ConversationStore::remove_and_reassign_active_unlocked(const std::string& id,
                                                             bool delete_file) {
    if (delete_file) {
        std::error_code ec;
        fs::remove(session_path_unlocked(id), ec);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const ConversationEntry& e) { return e.id == id; }),
                       entries_.end());
    }
    save_manifest_unlocked();

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
    remove_and_reassign_active_unlocked(id, /*delete_file=*/false);
}

void ConversationStore::purge(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    remove_and_reassign_active_unlocked(id, /*delete_file=*/true);
}

void ConversationStore::enqueue_title_job(const std::string& id,
                                          const std::string& user_msg,
                                          const std::string& assistant_msg,
                                          const std::string& model,
                                          Orchestrator& orch) {
    if (is_titled(id)) return;
    {
        std::lock_guard<std::mutex> lk(title_mu_);
        title_queue_.push_back(TitleJob{id, user_msg, assistant_msg, model, &orch});
    }
    title_cv_.notify_all();
}

void ConversationStore::title_worker_loop() {
    for (;;) {
        TitleJob job;
        {
            std::unique_lock<std::mutex> lk(title_mu_);
            title_cv_.wait(lk, [&] { return !title_queue_.empty() || title_stop_; });
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

std::string build_title_prompt(const std::string& user_msg, const std::string& assistant_msg) {
    constexpr size_t kMaxTotal = 2000;
    std::string prompt = "User: " + user_msg + "\n\nAssistant: " + assistant_msg;
    if (prompt.size() > kMaxTotal) prompt.resize(kMaxTotal);
    return prompt;
}

} // namespace

void ConversationStore::run_title_job(const TitleJob& job) {
    const std::string prompt = build_title_prompt(job.user_msg, job.assistant_msg);
    const std::string model = job.model;

    // Mint a standalone client for this one-shot call *now*, synchronously,
    // while we're still on the title worker thread (which ~ConversationStore
    // joins before the Orchestrator that owns job.orch is destroyed).  The
    // detached worker below then captures only this owned client — never
    // job.orch — so if the title call is still in flight past our 10s wait
    // when the process exits or the Orchestrator is torn down, the worker
    // keeps using its own client and destroys it when it returns, instead of
    // dereferencing a dangling Orchestrator (issue #51).
    std::shared_ptr<ApiClient> client = job.orch->make_side_client();

    // The completion runs on its own detached thread so we can enforce the
    // 10s timeout from our side (ApiClient has no per-request timeout of its
    // own). The promise is heap-allocated and captured by the detached
    // thread, so if we give up waiting, that thread can still safely call
    // set_value() later — a std::promise's destructor (unlike std::async's
    // shared state) never blocks.
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
        // Timed out: abandon the result, but interrupt the abandoned request
        // so the detached worker returns promptly and drops its client rather
        // than lingering on a slow/hung provider connection.  Safe because the
        // worker holds its own shared_ptr to `client`, keeping it alive across
        // this cancel().
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
