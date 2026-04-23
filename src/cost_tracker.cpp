// index_ai/src/cost_tracker.cpp

#include "cost_tracker.h"
#include <sstream>
#include <iomanip>

namespace index_ai {

// ─── Pricing table ───────────────────────────────────────────────────────────
// https://platform.claude.com/docs/en/about-claude/pricing

static const struct {
    const char* prefix;
    ModelPricing pricing;
} kPricingEntries[] = {
    { "claude-haiku",  { 0.80,  4.00,  0.08,  1.00  } },
    { "claude-opus",   { 15.00, 75.00, 1.50,  18.75 } },
    { "claude-sonnet", { 3.00,  15.00, 0.30,  3.75  } },
};

static const ModelPricing kDefaultPricing = { 3.00, 15.00, 0.30, 3.75 };

const ModelPricing& CostTracker::pricing_for(const std::string& model) {
    for (auto& e : kPricingEntries) {
        if (model.find(e.prefix) != std::string::npos) return e.pricing;
    }
    return kDefaultPricing;
}

double CostTracker::compute_cost(const std::string& model, const ApiResponse& resp) {
    // Local providers (ollama/…) are free — don't invent a bogus price by
    // falling through to the default pricing table.
    if (!is_priced(model)) return 0.0;

    auto& p = pricing_for(model);

    int plain_input = resp.input_tokens
                    - resp.cache_read_tokens
                    - resp.cache_creation_tokens;
    if (plain_input < 0) plain_input = 0;

    double cost = 0.0;
    cost += (plain_input                  / 1e6) * p.input_per_mtok;
    cost += (resp.output_tokens           / 1e6) * p.output_per_mtok;
    cost += (resp.cache_read_tokens       / 1e6) * p.cache_read_per_mtok;
    cost += (resp.cache_creation_tokens   / 1e6) * p.cache_write_per_mtok;
    return cost;
}

// ─── Formatting helpers ───────────────────────────────────────────────────────

std::string CostTracker::fmt_int(int n) {
    // Build right-to-left into a fixed buffer — avoids O(n²) shifts from
    // insert() in the middle of a growing string.  32 bytes covers any
    // signed 32-bit int comfortably (incl. sign + commas).
    if (n == 0) return "0";
    char out[32];
    int pos = sizeof(out);
    bool neg = n < 0;
    unsigned u = neg ? static_cast<unsigned>(-(long long)n) : static_cast<unsigned>(n);
    int digits = 0;
    while (u > 0 && pos > 0) {
        if (digits > 0 && digits % 3 == 0) out[--pos] = ',';
        out[--pos] = static_cast<char>('0' + (u % 10));
        u /= 10;
        ++digits;
    }
    if (neg && pos > 0) out[--pos] = '-';
    return std::string(out + pos, sizeof(out) - pos);
}

std::string CostTracker::fmt_dollars(double d) {
    std::ostringstream ss;
    if (d < 0.01) ss << std::fixed << std::setprecision(4);
    else          ss << std::fixed << std::setprecision(2);
    ss << "$" << d;
    return ss.str();
}

// ─── Public API ──────────────────────────────────────────────────────────────

void CostTracker::record(const std::string& agent_id,
                         const std::string& model,
                         const ApiResponse& resp) {
    double cost = compute_cost(model, resp);
    std::lock_guard<std::mutex> lk(mu_);
    auto& rec              = agents_[agent_id];
    rec.model               = model;
    rec.total_input        += resp.input_tokens;
    rec.total_output       += resp.output_tokens;
    rec.total_cache_read   += resp.cache_read_tokens;
    rec.total_cache_create += resp.cache_creation_tokens;
    rec.total_requests++;
    rec.total_cost         += cost;
    session_total_         += cost;
    session_input_         += resp.input_tokens;
    session_output_        += resp.output_tokens;
}

std::string CostTracker::format_footer(const ApiResponse& resp,
                                       const std::string& model) const {
    double req_cost = compute_cost(model, resp);
    std::lock_guard<std::mutex> lk(mu_);

    std::ostringstream ss;
    ss << "[in:"  << fmt_int(resp.input_tokens)
       << " out:" << fmt_int(resp.output_tokens);

    if (resp.cache_read_tokens > 0 || resp.cache_creation_tokens > 0) {
        ss << " cache:" << fmt_int(resp.cache_read_tokens)
           << "/"       << fmt_int(resp.cache_creation_tokens);
    }

    ss << " | " << fmt_dollars(req_cost)
       << " | session:" << fmt_dollars(session_total_)
       << "]";
    return ss.str();
}

std::string CostTracker::format_summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (agents_.empty()) return "  No requests recorded yet.\n";

    std::ostringstream ss;
    ss << "\nSession Cost: " << fmt_dollars(session_total_) << "\n\n";
    ss << "Costs are estimates based on list pricing.\n\n";

    ss << std::left
       << "  " << std::setw(16) << "Agent"
       << std::setw(24) << "Model"
       << std::setw(10) << "In"
       << std::setw(10) << "Out"
       << std::setw(9)  << "Cache%"
       << std::setw(6)  << "Reqs"
       << "Cost\n";
    ss << "  " << std::string(75, '-') << "\n";

    for (auto& [id, rec] : agents_) {
        double cache_pct = (rec.total_input > 0)
            ? 100.0 * rec.total_cache_read / rec.total_input : 0.0;

        std::ostringstream cpct;
        cpct << std::fixed << std::setprecision(1) << cache_pct << "%";

        std::string model_disp = rec.model.size() > 22
            ? rec.model.substr(0, 19) + "..."
            : rec.model;

        ss << "  " << std::left
           << std::setw(16) << id
           << std::setw(24) << model_disp
           << std::setw(10) << fmt_int(rec.total_input)
           << std::setw(10) << fmt_int(rec.total_output)
           << std::setw(9)  << cpct.str()
           << std::setw(6)  << rec.total_requests
           << fmt_dollars(rec.total_cost) << "\n";
    }

    ss << "  " << std::string(75, '-') << "\n";
    ss << std::right << std::setw(73) << "Total: "
       << fmt_dollars(session_total_) << "\n";
    return ss.str();
}

double CostTracker::session_cost() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_total_;
}

std::string CostTracker::format_session_stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_input_ == 0 && session_output_ == 0)
        return "";
    std::ostringstream ss;
    ss << "in:" << fmt_int(session_input_)
       << " out:" << fmt_int(session_output_)
       << " | " << fmt_dollars(session_total_);
    return ss.str();
}

} // namespace index_ai
