#pragma once
// index/include/loop_manager.h
//
// LoopManager owns autonomous agent "loops": an agent is invoked repeatedly in
// a dedicated thread, each iteration running until the agent stops producing
// tool calls for a few consecutive turns (or hits a hard ceiling, or is killed
// by the user).  Output is buffered per-loop and surfaced lazily via /log and
// /watch; only fatal failures and completions push back into OutputQueue so
// they surface in the REPL immediately.
//
// Public surface:
//   start()      — spawn a new loop, return its id
//   kill()       — stop a loop and join its thread
//   suspend()    — pause before the next iteration
//   resume()     — wake a suspended loop
//   inject()     — hand the next iteration a new prompt
//   list()       — human-readable table for /loops
//   log()        — buffered output for /log
//   log_since()  — incremental tail for /watch
//   is_stopped() — poll for exit
//   reap_stopped()/has_active()/active_count() — lifecycle queries
//
// Lifecycle: the destructor stops all loops and joins their threads so the
// manager is safe to let go of scope.

#include "orchestrator.h"
#include "cost_tracker.h"
#include "repl/queues.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace index_ai {

enum class LoopState { Running, Suspended, Stopped };
const char* loop_state_str(LoopState s);

struct LoopEntry {
    std::string loop_id;
    std::string agent_id;
    LoopState   state   = LoopState::Running;
    int         iter    = 0;
    std::string last_output;
    std::chrono::steady_clock::time_point started;

    std::mutex              mu;
    std::condition_variable cv;
    std::queue<std::string> injected;
    bool stop_req    = false;
    bool suspend_req = false;

    // Buffered output — loop runs silently; pull via /log <id>.
    std::vector<std::string> output_log;

    // Terminal-visible error message set when loop exits abnormally.
    std::string stop_reason;

    // Optional cost tracker for per-loop cost accounting.
    CostTracker* tracker = nullptr;

    // Output queue: loop threads push failure / completion banners here.
    OutputQueue* oq = nullptr;

    std::thread thread;

    LoopEntry() = default;
    LoopEntry(const LoopEntry&) = delete;
    LoopEntry& operator=(const LoopEntry&) = delete;
};

class LoopManager {
public:
    ~LoopManager();

    std::string start(Orchestrator& orch,
                      const std::string& agent_id,
                      const std::string& initial_prompt,
                      CostTracker* tracker = nullptr,
                      OutputQueue* oq = nullptr);

    bool kill   (const std::string& lid);
    bool suspend(const std::string& lid);
    bool resume (const std::string& lid);
    bool inject (const std::string& lid, const std::string& msg);

    std::string list() const;
    std::string log(const std::string& lid, int last_n = 0) const;
    size_t      log_count(const std::string& lid) const;
    std::string log_since(const std::string& lid, size_t offset) const;
    bool        is_stopped(const std::string& lid) const;

    void reap_stopped();
    std::vector<std::string> list_ids() const;
    bool has_active()  const;
    int  active_count() const;

private:
    static void run_loop(LoopEntry* e, Orchestrator& orch, std::string initial_prompt);

    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<LoopEntry>> loops_;
    int next_id_ = 0;
};

} // namespace index_ai
