#pragma once
// index/include/agent.h — Individual agent with conversation history + constitution

#include "constitution.h"
#include "api_client.h"
#include <functional>
#include <string>
#include <vector>
#include <chrono>

namespace index_ai {

struct AgentStats {
    int total_input_tokens  = 0;
    int total_output_tokens = 0;
    int total_requests      = 0;
    std::chrono::steady_clock::time_point created;
};

class Agent {
public:
    Agent(const std::string& id, Constitution config, ApiClient& client);

    // Send a message and get response (blocking)
    ApiResponse send(const std::string& user_message);
    // Send with streaming — chunks delivered via callback as they arrive
    ApiResponse stream(const std::string& user_message, StreamCallback cb);
    // Clear conversation history (keep constitution)
    void reset_history();
    // Replace history (used for session restore)
    void set_history(std::vector<Message> h) { history_ = std::move(h); }
    // Truncate history to keep token usage bounded
    void trim_history(int keep_last_n = 10);
    // Context compaction
    std::string compact();

    // Accessors
    const std::string& id() const { return id_; }
    const Constitution& config() const { return config_; }
    Constitution& config_mut() { return config_; }
    const AgentStats& stats() const { return stats_; }
    const std::vector<Message>& history() const { return history_; }
    const std::string& context_summary() const { return context_summary_; }

    std::string status_summary() const;

    using CompactCallback = std::function<void(const std::string& agent_id,
                                                size_t history_size)>;
    void set_compact_callback(CompactCallback cb) { compact_cb_ = std::move(cb); }

    std::string to_json() const;

private:
    std::string id_;
    Constitution config_;
    ApiClient& client_;
    std::vector<Message> history_;
    AgentStats stats_;
    std::string context_summary_;
    CompactCallback compact_cb_;
};

} // namespace index_ai
