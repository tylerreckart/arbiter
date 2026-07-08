// arbiter/src/loop_manager.cpp — see loop_manager.h

#include "loop_manager.h"
#include "markdown.h"
#include "theme.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace arbiter {

// Per-entry truncation so a single iteration with a huge agent response can't
// dominate memory; combined with a total-bytes cap applied by trim_log_bytes()
// this bounds a loop's log footprint regardless of per-turn output size.
static constexpr size_t kMaxLogEntryBytes = 32 * 1024;   //  32 KB per entry
static constexpr size_t kMaxLogTotalBytes = 2 * 1024 * 1024;  // 2 MB total

// Self-prompt envelope for inter-iteration continuations.  Mirrors the advisor
// redirect envelope so the executor sees a consistent synthetic-user-turn
// shape; the orchestrator's in-loop gate still fires on each terminating turn.
static const char kLoopContinuation[] =
    "[loop continuation — synthetic user turn]\n"
    "Continue working on the original task. Prior turns are in your "
    "conversation history — do not repeat completed work unless needed "
    "for context.\n"
    "[end loop continuation]";

static bool gate_mode_active(Orchestrator& orch, const std::string& agent_id) {
    try {
        const auto& cfg = orch.get_constitution(agent_id);
        return cfg.advisor.mode == "gate" && !cfg.advisor.model.empty();
    } catch (...) {
        return false;
    }
}

static std::string truncate_entry(const std::string& s) {
    if (s.size() <= kMaxLogEntryBytes) return s;
    std::string out = s.substr(0, kMaxLogEntryBytes);
    out += "\n... [loop-log entry truncated at 32 KB]\n";
    return out;
}

// Drop oldest entries until the aggregate byte count fits under
// kMaxLogTotalBytes.  Called with the entry mutex held.
static void trim_log_bytes(std::vector<std::string>& log) {
    size_t total = 0;
    for (const auto& s : log) total += s.size();
    while (total > kMaxLogTotalBytes && !log.empty()) {
        total -= log.front().size();
        log.erase(log.begin());
    }
}

const char* loop_state_str(LoopState s) {
    switch (s) {
        case LoopState::Running:   return "running";
        case LoopState::Suspended: return "suspended";
        case LoopState::Stopped:   return "stopped";
    }
    return "?";
}

LoopManager::~LoopManager() {
    // Take ownership of every entry under the map lock, then signal and
    // join outside it.  Owning the entries locally means a concurrent
    // kill()/reap_stopped() can't erase (and destroy) an entry while we
    // still hold a pointer to its thread.
    std::map<std::string, std::unique_ptr<LoopEntry>> loops;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loops.swap(loops_);
    }
    for (auto& [id, e] : loops) {
        { std::lock_guard<std::mutex> ek(e->mu); e->stop_req = true; }
        e->cv.notify_all();
    }
    for (auto& [id, e] : loops) {
        if (e->thread.joinable()) e->thread.join();
    }
}

std::string LoopManager::start(Orchestrator& orch,
                               const std::string& agent_id,
                               const std::string& initial_prompt,
                               OutputQueue* oq) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string lid = "loop-" + std::to_string(next_id_++);
    auto e = std::make_unique<LoopEntry>();
    e->loop_id  = lid;
    e->agent_id = agent_id;
    e->started  = std::chrono::steady_clock::now();
    e->state    = LoopState::Running;
    e->oq       = oq;
    e->thread   = std::thread(run_loop, e.get(), std::ref(orch), initial_prompt);
    loops_[lid] = std::move(e);
    return lid;
}

bool LoopManager::kill(const std::string& lid) {
    // Move the entry out of the map first so a second concurrent kill()
    // (or the destructor) can't find it and race the join/destroy below.
    std::unique_ptr<LoopEntry> e;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return false;
        e = std::move(it->second);
        loops_.erase(it);
    }
    { std::lock_guard<std::mutex> ek(e->mu); e->stop_req = true; }
    e->cv.notify_all();
    if (e->thread.joinable()) e->thread.join();
    return true;
}

bool LoopManager::suspend(const std::string& lid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return false;
    std::lock_guard<std::mutex> ek(it->second->mu);
    if (it->second->state != LoopState::Running) return false;
    it->second->suspend_req = true;
    it->second->state = LoopState::Suspended;
    return true;
}

bool LoopManager::resume(const std::string& lid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return false;
    {
        std::lock_guard<std::mutex> ek(it->second->mu);
        if (it->second->state != LoopState::Suspended) return false;
        it->second->suspend_req = false;
        it->second->state = LoopState::Running;
    }
    it->second->cv.notify_all();
    return true;
}

bool LoopManager::inject(const std::string& lid, const std::string& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return false;
    { std::lock_guard<std::mutex> ek(it->second->mu); it->second->injected.push(msg); }
    it->second->cv.notify_all();
    return true;
}

std::string LoopManager::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (loops_.empty()) return "  (no active loops)\n";
    std::ostringstream ss;
    auto now = std::chrono::steady_clock::now();
    for (auto& [lid, e] : loops_) {
        long secs = std::chrono::duration_cast<std::chrono::seconds>(
            now - e->started).count();
        std::lock_guard<std::mutex> ek(e->mu);
        ss << "  " << lid
           << "  agent:" << e->agent_id
           << "  state:" << loop_state_str(e->state)
           << "  iter:" << e->iter
           << "  elapsed:" << secs << "s\n";
        if (e->state == LoopState::Stopped && !e->stop_reason.empty()) {
            ss << "    stop: " << e->stop_reason << "\n";
        } else if (!e->last_output.empty()) {
            std::string preview = e->last_output.substr(
                0, std::min<size_t>(120, e->last_output.size()));
            for (char& c : preview) if (c == '\n') c = ' ';
            ss << "    last: " << preview;
            if (e->last_output.size() > 120) ss << "...";
            ss << "\n";
        }
    }
    return ss.str();
}

std::string LoopManager::log(const std::string& lid, int last_n) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return "ERR: no loop '" + lid + "'\n";
    std::lock_guard<std::mutex> ek(it->second->mu);
    const auto& entries = it->second->output_log;
    if (entries.empty()) return "  (no output yet)\n";

    std::ostringstream ss;
    int start = 0;
    if (last_n > 0 && static_cast<int>(entries.size()) > last_n)
        start = static_cast<int>(entries.size()) - last_n;
    for (int i = start; i < static_cast<int>(entries.size()); ++i)
        ss << entries[i];
    return ss.str();
}

size_t LoopManager::log_count(const std::string& lid) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return 0;
    std::lock_guard<std::mutex> ek(it->second->mu);
    return it->second->output_log.size();
}

std::string LoopManager::log_since(const std::string& lid, size_t offset) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return "";
    std::lock_guard<std::mutex> ek(it->second->mu);
    const auto& entries = it->second->output_log;
    std::ostringstream ss;
    for (size_t i = offset; i < entries.size(); ++i)
        ss << entries[i];
    return ss.str();
}

bool LoopManager::is_stopped(const std::string& lid) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return true;
    std::lock_guard<std::mutex> ek(it->second->mu);
    return it->second->state == LoopState::Stopped;
}

void LoopManager::reap_stopped() {
    // Move stopped entries out of the map under the lock, join outside it
    // (join of a finished thread is quick, but there's no reason to make
    // other API calls wait on it).
    std::vector<std::unique_ptr<LoopEntry>> stopped;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = loops_.begin(); it != loops_.end(); ) {
            bool is_stopped;
            {
                std::lock_guard<std::mutex> ek(it->second->mu);
                is_stopped = it->second->state == LoopState::Stopped;
            }
            if (is_stopped) {
                stopped.push_back(std::move(it->second));
                it = loops_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& e : stopped) {
        if (e->thread.joinable()) e->thread.join();
    }
}

std::vector<std::string> LoopManager::list_ids() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> ids;
    for (auto& [lid, _] : loops_) ids.push_back(lid);
    return ids;
}

bool LoopManager::has_active() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [_, e] : loops_) {
        std::lock_guard<std::mutex> ek(e->mu);
        if (e->state != LoopState::Stopped) return true;
    }
    return false;
}

int LoopManager::active_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto& [_, e] : loops_) {
        std::lock_guard<std::mutex> ek(e->mu);
        if (e->state != LoopState::Stopped) ++n;
    }
    return n;
}

std::vector<LoopManager::LoopBrief> LoopManager::briefs() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<LoopBrief> out;
    out.reserve(loops_.size());
    for (auto& [lid, e] : loops_) {
        std::lock_guard<std::mutex> ek(e->mu);
        if (e->state == LoopState::Stopped) continue;
        LoopBrief b;
        b.id       = lid;
        b.agent_id = e->agent_id;
        b.state    = loop_state_str(e->state);
        b.iter     = e->iter;
        out.push_back(std::move(b));
    }
    return out;
}

void LoopManager::run_loop(LoopEntry* e, Orchestrator& orch,
                           std::string initial_prompt) {
    const std::string original_task = initial_prompt;
    const bool gate_active = gate_mode_active(orch, e->agent_id);
    std::string prompt = initial_prompt;
    bool first = true;
    bool stopped_by_request = false;
    int  consecutive_idle = 0;
    int  total_iters      = 0;
    static constexpr int kMaxIdle  = 2;
    static constexpr int kMaxIters = 20;

    while (true) {
        if (total_iters >= kMaxIters) {
            std::string reason =
                "max iterations reached (" + std::to_string(kMaxIters) + ")";
            { std::lock_guard<std::mutex> ek(e->mu); e->stop_reason = reason; }
            if (e->oq) {
                e->oq->push("\n" + theme().accent_warning + "[" +
                            e->loop_id + "/" + e->agent_id +
                            " MAX ITERS]" + theme().reset + " " +
                            reason +
                            "\n  Use /log " + e->loop_id + " to review.\n");
            }
            break;
        }

        {
            std::unique_lock<std::mutex> lk(e->mu);
            e->cv.wait(lk, [e]{ return !e->suspend_req || e->stop_req; });
            if (e->stop_req) { stopped_by_request = true; break; }
            if (!e->injected.empty()) {
                prompt = e->injected.front();
                e->injected.pop();
                first = true;
                consecutive_idle = 0;
            }
        }

        {
            std::ostringstream pre;
            pre << "[" << e->loop_id << "/" << e->agent_id
                << " thinking...]\n";
            std::lock_guard<std::mutex> ek(e->mu);
            e->output_log.push_back(truncate_entry(pre.str()));
            trim_log_bytes(e->output_log);
        }

        // Pin original_task on every iteration so the in-loop advisor gate
        // evaluates against the loop's real goal, not the continuation prompt.
        auto resp = orch.send(e->agent_id, prompt, original_task);
        int iter_now;
        { std::lock_guard<std::mutex> ek(e->mu); iter_now = ++e->iter; }
        total_iters++;

        if (resp.ok) {
            if (resp.had_tool_calls) consecutive_idle = 0;
            else                     consecutive_idle++;
        }

        {
            std::ostringstream entry;
            entry << "[" << e->loop_id << "/" << e->agent_id
                  << " #" << iter_now << "]\n";
            if (resp.ok) {
                entry << render_markdown(resp.content) << "\n";
                entry << "  [in:" << resp.input_tokens
                      << " out:" << resp.output_tokens << "]\n";
            } else {
                entry << "ERR: " << resp.error << "\n";
            }
            std::lock_guard<std::mutex> ek(e->mu);
            if (resp.ok) e->last_output = resp.content;
            std::string e_str = truncate_entry(entry.str());
            if (!e->output_log.empty()) {
                e->output_log.back() = std::move(e_str);
            } else {
                e->output_log.push_back(std::move(e_str));
            }
            trim_log_bytes(e->output_log);
        }

        if (!resp.ok) {
            { std::lock_guard<std::mutex> ek(e->mu); e->stop_reason = resp.error; }
            if (e->oq) {
                e->oq->push("\n" + theme().bold + theme().accent_error + "[" +
                            e->loop_id + "/" + e->agent_id + " FAILED]" +
                            theme().reset + " " +
                            resp.error +
                            "\n  Use /log " + e->loop_id +
                            " to see output, /kill " + e->loop_id +
                            " to dismiss.\n");
            }
            break;
        }

        // Gate mode: the in-loop advisor gate is the termination authority.
        // CONTINUE (gate_approved) means the task is done; REDIRECT/HALT and
        // tool dispatch all resolve inside this send() call.  When the gate
        // never fires (kMaxTurns truncation mid tool-chain), gate_approved
        // stays false and we self-prompt for another top-level iteration.
        if (gate_active && resp.gate_approved) {
            {
                std::lock_guard<std::mutex> ek(e->mu);
                e->stop_reason = "task complete (advisor gate approved)";
            }
            if (e->oq) {
                e->oq->push("\n" + theme().bold + theme().accent_success + "[" +
                            e->loop_id + "/" + e->agent_id + " DONE]" +
                            theme().reset + " " +
                            e->stop_reason +
                            "\n  Use /log " + e->loop_id +
                            " to review, /kill " + e->loop_id + " to dismiss.\n");
            }
            break;
        }

        if (!gate_active && consecutive_idle >= kMaxIdle) {
            {
                std::lock_guard<std::mutex> ek(e->mu);
                e->stop_reason = "task complete (idle after " +
                                 std::to_string(consecutive_idle) + " turns)";
            }
            if (e->oq) {
                e->oq->push("\n" + theme().bold + theme().accent_success + "[" +
                            e->loop_id + "/" + e->agent_id + " DONE]" +
                            theme().reset + " " +
                            e->stop_reason +
                            "\n  Use /log " + e->loop_id +
                            " to review, /kill " + e->loop_id + " to dismiss.\n");
            }
            break;
        }

        if (first) first = false;
        prompt = kLoopContinuation;

        { std::lock_guard<std::mutex> lk(e->mu); if (e->stop_req) { stopped_by_request = true; break; } }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    (void)stopped_by_request;
    std::lock_guard<std::mutex> ek(e->mu);
    e->state = LoopState::Stopped;
}

} // namespace arbiter
