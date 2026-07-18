#pragma once
// arbiter/include/agent.h — Individual agent with conversation history + constitution

#include "constitution.h"
#include "api_client.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>

namespace arbiter {

struct AgentStats {
    // Atomic: written by the request thread mid-turn while other threads
    // (status_summary from the REPL, to_json from the autosave worker)
    // read concurrently.
    std::atomic<int> total_input_tokens{0};
    std::atomic<int> total_output_tokens{0};
    std::atomic<int> total_requests{0};
    std::chrono::steady_clock::time_point created;   // set once in the ctor
};

class Agent {
public:
    Agent(const std::string& id, Constitution config, ApiClient& client);

    // Send a message and get response (blocking).
    ApiResponse send(const std::string& user_message);
    // Multipart variant.  Used by vision input and by tool-result re-entry
    // when one or more tool results returned image content.  The text-only
    // overload above wraps in a single text part and calls this one.
    ApiResponse send(std::vector<ContentPart> parts);

    // Send with streaming — chunks delivered via callback as they arrive.
    ApiResponse stream(const std::string& user_message, StreamCallback cb);
    ApiResponse stream(std::vector<ContentPart> parts, StreamCallback cb);
    // Clear conversation history (keep constitution)
    void reset_history();
    // Replace history (used for session restore)
    void set_history(std::vector<Message> h) {
        std::lock_guard<std::mutex> lk(history_mu_);
        history_ = std::move(h);
    }

    // Accessors
    const std::string& id() const { return id_; }
    const Constitution& config() const { return config_; }
    Constitution& config_mut() { return config_; }
    const AgentStats& stats() const { return stats_; }
    // Returns a copy, not a reference: a background save (ConversationStore's
    // autosave thread) reads this concurrently with a pane's exec thread
    // appending to history_ mid-turn, so callers must not hold a reference
    // into live state.
    std::vector<Message> history() const {
        std::lock_guard<std::mutex> lk(history_mu_);
        return history_;
    }

    std::string status_summary() const;

    std::string to_json() const;

private:
    std::string id_;
    Constitution config_;
    ApiClient& client_;
    mutable std::mutex history_mu_;
    std::vector<Message> history_;
    AgentStats stats_;

    // Concat continuation turns onto `resp` until the model actually finishes
    // (stop_reason != "max_tokens") or a cap is hit.  Pushes partial assistant
    // + "continue" prompts into history_ during the loop and pops them before
    // returning so the caller can commit a single merged assistant turn.
    // `cb` may be null — null triggers the blocking client_.complete() path,
    // non-null triggers client_.stream() so additional chunks flow through.
    void continue_until_done(ApiResponse& resp, StreamCallback cb);
};

} // namespace arbiter
