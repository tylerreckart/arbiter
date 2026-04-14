#pragma once
// index/include/cost_tracker.h — Token usage and cost estimation per agent

#include "api_client.h"
#include <string>
#include <unordered_map>
#include <mutex>

namespace index_ai {

struct ModelPricing {
    double input_per_mtok;        // $ per 1M plain input tokens
    double output_per_mtok;       // $ per 1M output tokens
    double cache_read_per_mtok;   // $ per 1M cache-read tokens
    double cache_write_per_mtok;  // $ per 1M cache-write tokens
};

struct AgentCostRecord {
    std::string model;
    int    total_input         = 0;
    int    total_output        = 0;
    int    total_cache_read    = 0;
    int    total_cache_create  = 0;
    int    total_requests      = 0;
    double total_cost          = 0.0;
};

class CostTracker {
public:
    // Record a completed request for an agent.
    void record(const std::string& agent_id,
                const std::string& model,
                const ApiResponse& resp);

    // Format the per-response footer line.
    // e.g. "[in:1,234 out:567 cache:890/12 | $0.0084 | session:$0.48]"
    std::string format_footer(const ApiResponse& resp,
                              const std::string& model) const;

    // Format the full /tokens breakdown table.
    std::string format_summary() const;

    // Session total cost accumulated across all agents.
    double session_cost() const;

    // Compact session-level stats for the TUI header.
    // e.g. "in:12,681 out:2,085 | $0.02 | session:$0.04"
    std::string format_session_stats() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, AgentCostRecord> agents_;
    double session_total_  = 0.0;
    int    session_input_  = 0;
    int    session_output_ = 0;

    static double compute_cost(const std::string& model, const ApiResponse& resp);
    static const ModelPricing& pricing_for(const std::string& model);
    static std::string fmt_int(int n);
    static std::string fmt_dollars(double d);
};

} // namespace index_ai
