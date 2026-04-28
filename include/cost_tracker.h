#pragma once
// index/include/cost_tracker.h — Token usage and cost estimation per agent

#include "api_client.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

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

    // USD cost of a single turn at list pricing — stateless.  Exposed
    // publicly so the API server's billing path can compute provider_uc
    // per turn without needing a CostTracker instance.
    static double compute_cost(const std::string& model, const ApiResponse& resp);

    // One entry from the static pricing table.  The HTTP API exposes
    // this as GET /v1/models so clients can render a model picker without
    // shipping their own pricing data.
    struct ModelEntry {
        std::string  id;                      // the family prefix — matches what the router expects
        ModelPricing pricing;
    };
    static std::vector<ModelEntry> all_models();

    // Per-token-type cost breakdown in USD.  Sum equals compute_cost.
    // Local providers (ollama/...) return all-zero.  Used by the API
    // server to record each component into usage_log so analytics can
    // chart input vs output vs cache spend over time.
    struct CostBreakdown {
        double input        = 0.0;
        double output       = 0.0;
        double cache_read   = 0.0;
        double cache_create = 0.0;
    };
    static CostBreakdown compute_cost_breakdown(const std::string& model,
                                                 const ApiResponse& resp);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, AgentCostRecord> agents_;
    double session_total_  = 0.0;
    int    session_input_  = 0;
    int    session_output_ = 0;

    static const ModelPricing& pricing_for(const std::string& model);
    static std::string fmt_int(int n);
    static std::string fmt_dollars(double d);
};

} // namespace index_ai
