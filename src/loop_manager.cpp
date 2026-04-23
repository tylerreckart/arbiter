// index_ai/src/loop_manager.cpp — see loop_manager.h

#include "loop_manager.h"
#include "markdown.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace index_ai {

// Per-entry truncation so a single iteration with a huge agent response can't
// dominate memory; combined with a total-bytes cap applied by trim_log_bytes()
// this bounds a loop's log footprint regardless of per-turn output size.
static constexpr size_t kMaxLogEntryBytes = 32 * 1024;   //  32 KB per entry
static constexpr size_t kMaxLogTotalBytes = 2 * 1024 * 1024;  // 2 MB total

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
    std::unique_lock<std::mutex> lk(mu_);
    for (auto& [id, e] : loops_) {
        std::lock_guard<std::mutex> ek(e->mu);
        e->stop_req = true;
        e->cv.notify_all();
    }
    // Collect threads to join outside the lock.
    std::vector<std::thread*> threads;
    for (auto& [id, e] : loops_) threads.push_back(&e->thread);
    lk.unlock();
    for (auto* t : threads) if (t->joinable()) t->join();
}

std::string LoopManager::start(Orchestrator& orch,
                               const std::string& agent_id,
                               const std::string& initial_prompt,
                               CostTracker* tracker,
                               OutputQueue* oq) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string lid = "loop-" + std::to_string(next_id_++);
    auto e = std::make_unique<LoopEntry>();
    e->loop_id  = lid;
    e->agent_id = agent_id;
    e->started  = std::chrono::steady_clock::now();
    e->state    = LoopState::Running;
    e->tracker  = tracker;
    e->oq       = oq;
    e->thread   = std::thread(run_loop, e.get(), std::ref(orch), initial_prompt);
    loops_[lid] = std::move(e);
    return lid;
}

bool LoopManager::kill(const std::string& lid) {
    std::unique_lock<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return false;
    auto* e = it->second.get();
    { std::lock_guard<std::mutex> ek(e->mu); e->stop_req = true; }
    e->cv.notify_all();
    auto& t = e->thread;
    lk.unlock();
    if (t.joinable()) t.join();
    lk.lock();
    loops_.erase(lid);
    return true;
}

bool LoopManager::suspend(const std::string& lid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return false;
    if (it->second->state != LoopState::Running) return false;
    { std::lock_guard<std::mutex> ek(it->second->mu); it->second->suspend_req = true; }
    it->second->state = LoopState::Suspended;
    return true;
}

bool LoopManager::resume(const std::string& lid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return false;
    if (it->second->state != LoopState::Suspended) return false;
    { std::lock_guard<std::mutex> ek(it->second->mu); it->second->suspend_req = false; }
    it->second->state = LoopState::Running;
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
        ss << "  " << lid
           << "  agent:" << e->agent_id
           << "  state:" << loop_state_str(e->state)
           << "  iter:" << e->iter
           << "  elapsed:" << secs << "s\n";
        {
            std::lock_guard<std::mutex> ek(e->mu);
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
    }
    return ss.str();
}

std::string LoopManager::log(const std::string& lid, int last_n) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return "ERR: no loop '" + lid + "'\n";
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
    return it->second->output_log.size();
}

std::string LoopManager::log_since(const std::string& lid, size_t offset) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = loops_.find(lid);
    if (it == loops_.end()) return "";
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
    return it->second->state == LoopState::Stopped;
}

void LoopManager::reap_stopped() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = loops_.begin(); it != loops_.end(); ) {
        if (it->second->state == LoopState::Stopped) {
            if (it->second->thread.joinable()) it->second->thread.join();
            it = loops_.erase(it);
        } else {
            ++it;
        }
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
    for (auto& [_, e] : loops_)
        if (e->state != LoopState::Stopped) return true;
    return false;
}

int LoopManager::active_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto& [_, e] : loops_)
        if (e->state != LoopState::Stopped) ++n;
    return n;
}

void LoopManager::run_loop(LoopEntry* e, Orchestrator& orch,
                           std::string initial_prompt) {
    // First iteration uses the initial prompt.  Subsequent iterations send
    // "Continue." — the agent already has its prior output in conversation
    // history, so feeding the raw output back as a user message would make
    // it appear to be echoed by a human.
    std::string prompt = initial_prompt;
    bool first = true;
    bool stopped_by_request = false;
    int  consecutive_idle = 0;
    int  total_iters      = 0;
    static constexpr int kMaxIdle  = 2;
    static constexpr int kMaxIters = 20;

    while (true) {
        if (total_iters >= kMaxIters) {
            e->stop_reason = "max iterations reached (" + std::to_string(kMaxIters) + ")";
            if (e->oq) {
                e->oq->push("\n\033[33m[" + e->loop_id + "/" + e->agent_id +
                            " MAX ITERS]\033[0m " + e->stop_reason +
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

        auto resp = orch.send(e->agent_id, prompt);
        e->iter++;
        total_iters++;

        if (resp.ok) {
            if (resp.had_tool_calls) consecutive_idle = 0;
            else                     consecutive_idle++;
        }

        {
            std::ostringstream entry;
            entry << "[" << e->loop_id << "/" << e->agent_id
                  << " #" << e->iter << "]\n";
            if (resp.ok) {
                entry << render_markdown(resp.content) << "\n";
                if (e->tracker) {
                    std::string model = orch.get_agent_model(e->agent_id);
                    e->tracker->record(e->agent_id, model, resp);
                    entry << "  " << e->tracker->format_footer(resp, model) << "\n";
                } else {
                    entry << "  [in:" << resp.input_tokens
                          << " out:" << resp.output_tokens << "]\n";
                }
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
                e->oq->push("\n\033[1;31m[" + e->loop_id + "/" +
                            e->agent_id + " FAILED]\033[0m " +
                            resp.error +
                            "\n  Use /log " + e->loop_id +
                            " to see output, /kill " + e->loop_id +
                            " to dismiss.\n");
            }
            break;
        }

        if (consecutive_idle >= kMaxIdle) {
            {
                std::lock_guard<std::mutex> ek(e->mu);
                e->stop_reason = "task complete (idle after " +
                                 std::to_string(consecutive_idle) + " turns)";
            }
            if (e->oq) {
                e->oq->push("\n\033[1;32m[" + e->loop_id + "/" +
                            e->agent_id + " DONE]\033[0m " +
                            e->stop_reason +
                            "\n  Use /log " + e->loop_id +
                            " to review, /kill " + e->loop_id + " to dismiss.\n");
            }
            break;
        }

        if (first) first = false;
        prompt = "Continue.";

        { std::lock_guard<std::mutex> lk(e->mu); if (e->stop_req) { stopped_by_request = true; break; } }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    (void)stopped_by_request;
    e->state = LoopState::Stopped;
}

} // namespace index_ai
