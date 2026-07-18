#pragma once
// arbiter/include/agent.h — Individual agent with conversation history + constitution

#include "constitution.h"
#include "api_client.h"
#include "agent_conversation.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
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
    // Clear the current ConversationScope's history (keep constitution).
    void reset_history();
    // Drop every conversation slot (used when tearing down an agent).
    void reset_all_histories();
    // Drop one conversation's history without touching others.
    void erase_conversation(const std::string& conversation_id);
    // Replace history for the current ConversationScope (session restore).
    void set_history(std::vector<Message> h) {
        std::lock_guard<std::mutex> lk(history_mu_);
        histories_[agent_conversation_key()] = std::move(h);
    }

    // Accessors
    const std::string& id() const { return id_; }
    const Constitution& config() const { return config_; }
    Constitution& config_mut() { return config_; }
    const AgentStats& stats() const { return stats_; }
    // Returns a copy, not a reference: a background save (ConversationStore's
    // autosave thread) reads this concurrently with a pane's exec thread
    // appending to history mid-turn, so callers must not hold a reference
    // into live state.  Reads the current ConversationScope's slot.
    std::vector<Message> history() const {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = histories_.find(agent_conversation_key());
        if (it == histories_.end()) return {};
        return it->second;
    }
    // True if any messages are stored under `conversation_id`.
    [[nodiscard]] bool has_conversation(const std::string& conversation_id) const {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = histories_.find(conversation_id);
        return it != histories_.end() && !it->second.empty();
    }

    std::string status_summary() const;

    std::string to_json() const;

private:
    std::string id_;
    Constitution config_;
    ApiClient& client_;
    mutable std::mutex history_mu_;
    // Histories keyed by ConversationScope id ("" outside a scope).
    std::unordered_map<std::string, std::vector<Message>> histories_;
    AgentStats stats_;

    // Concat continuation turns onto `resp` until the model actually finishes
    // (stop_reason != "max_tokens") or a cap is hit.  Pushes partial assistant
    // + "continue" prompts into the scoped history during the loop and pops
    // them before returning so the caller can commit a single merged
    // assistant turn.  `cb` may be null — null triggers the blocking
    // client_.complete() path, non-null triggers client_.stream() so
    // additional chunks flow through.
    void continue_until_done(ApiResponse& resp, StreamCallback cb);
};

} // namespace arbiter
