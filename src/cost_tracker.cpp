// index_ai/src/cost_tracker.cpp

#include "cost_tracker.h"
#include <sstream>
#include <iomanip>

namespace index_ai {

// ─── Pricing table ───────────────────────────────────────────────────────────
// Source: https://models.dev/api.json — a community-maintained registry that
// aggregates provider pricing pages.  Refreshed at build time; re-run the
// scripted refresh when new models ship.
//
// Columns: { input $/MTok, output $/MTok, cache_read $/MTok, cache_write $/MTok }.
// Lookup is a substring search against the model string (see pricing_for),
// so longer/more-specific prefixes must come first — otherwise "gpt-4o-mini"
// would match the "gpt-4o" row, and "gpt-5.1" would match "gpt-5".
//
// OpenAI's prompt caching is implicit (no explicit breakpoints, no write
// cost) so cache_write is 0 for openai rows; cached input is reported as
// resp.cache_read_tokens via prompt_tokens_details.cached_tokens.  Where
// models.dev lists no cache_read rate (older gpt-4 / -turbo / o1-pro / o3-pro),
// we leave cache_read = 0 so those models behave as if caching weren't
// billed for them (which matches reality today).

static const struct {
    const char* prefix;
    ModelPricing pricing;
} kPricingEntries[] = {
    // ── Anthropic ─────────────────────────────────────────────────────────
    // Most specific first: Opus 4.5/4.6/4.7 repriced lower than earlier Opus;
    // Haiku 4.5 repriced above legacy Haiku.
    { "claude-opus-4-7",    { 5.00,  25.00, 0.50,  6.25  } },
    { "claude-opus-4-6",    { 5.00,  25.00, 0.50,  6.25  } },
    { "claude-opus-4-5",    { 5.00,  25.00, 0.50,  6.25  } },
    { "claude-opus",        { 15.00, 75.00, 1.50,  18.75 } },   // 4.0 / 4.1 / older
    { "claude-haiku-4",     { 1.00,  5.00,  0.10,  1.25  } },   // 4.5
    { "claude-3-haiku",     { 0.25,  1.25,  0.03,  0.30  } },
    { "claude-haiku",       { 0.80,  4.00,  0.08,  1.00  } },   // 3.5 / latest
    { "claude-sonnet",      { 3.00,  15.00, 0.30,  3.75  } },

    // ── OpenAI ────────────────────────────────────────────────────────────
    // gpt-5.x families — longer patterns first, then the family default.
    { "gpt-5.5",            { 5.00,  30.00, 0.50,  0.0 } },
    { "gpt-5.4-pro",        { 30.00, 180.00, 0.0,  0.0 } },
    { "gpt-5.4-mini",       { 0.75,  4.50,  0.075, 0.0 } },
    { "gpt-5.4-nano",       { 0.20,  1.25,  0.02,  0.0 } },
    { "gpt-5.4",            { 2.50,  15.00, 0.25,  0.0 } },
    { "gpt-5.3",            { 1.75,  14.00, 0.175, 0.0 } },
    { "gpt-5.2-pro",        { 21.00, 168.00, 0.0,  0.0 } },
    { "gpt-5.2",            { 1.75,  14.00, 0.175, 0.0 } },
    { "gpt-5.1-codex-mini", { 0.25,  2.00,  0.025, 0.0 } },
    { "gpt-5.1",            { 1.25,  10.00, 0.125, 0.0 } },
    { "gpt-5-pro",          { 15.00, 120.00, 0.0,  0.0 } },
    { "gpt-5-mini",         { 0.25,  2.00,  0.025, 0.0 } },
    { "gpt-5-nano",         { 0.05,  0.40,  0.005, 0.0 } },
    { "gpt-5-codex",        { 1.25,  10.00, 0.125, 0.0 } },
    { "gpt-5",              { 1.25,  10.00, 0.125, 0.0 } },

    // gpt-4.x.
    { "gpt-4.1-nano",       { 0.10,  0.40,  0.03,  0.0 } },
    { "gpt-4.1-mini",       { 0.40,  1.60,  0.10,  0.0 } },
    { "gpt-4.1",            { 2.00,  8.00,  0.50,  0.0 } },
    { "gpt-4o-mini",        { 0.15,  0.60,  0.08,  0.0 } },
    { "gpt-4o",             { 2.50,  10.00, 1.25,  0.0 } },
    { "gpt-4-turbo",        { 10.00, 30.00, 0.0,   0.0 } },
    { "gpt-4",              { 30.00, 60.00, 0.0,   0.0 } },
    { "gpt-3.5",            { 0.50,  1.50,  0.0,   0.0 } },

    // o-series reasoning.  -deep-research is metered at its own higher rate.
    { "o4-mini-deep-research", { 2.00,  8.00,  0.50,  0.0 } },
    { "o4-mini",            { 1.10,  4.40,  0.28,  0.0 } },
    { "o3-deep-research",   { 10.00, 40.00, 2.50,  0.0 } },
    { "o3-pro",             { 20.00, 80.00, 0.0,   0.0 } },
    { "o3-mini",            { 1.10,  4.40,  0.55,  0.0 } },
    { "o3",                 { 2.00,  8.00,  0.50,  0.0 } },
    { "o1-pro",             { 150.00, 600.00, 0.0, 0.0 } },
    { "o1-mini",            { 1.10,  4.40,  0.55,  0.0 } },
    { "o1-preview",         { 15.00, 60.00, 7.50,  0.0 } },
    { "o1",                 { 15.00, 60.00, 7.50,  0.0 } },
};

static const ModelPricing kDefaultPricing = { 3.00, 15.00, 0.30, 3.75 };

const ModelPricing& CostTracker::pricing_for(const std::string& model) {
    for (auto& e : kPricingEntries) {
        if (model.find(e.prefix) != std::string::npos) return e.pricing;
    }
    return kDefaultPricing;
}

CostTracker::CostBreakdown
CostTracker::compute_cost_breakdown(const std::string& model,
                                     const ApiResponse& resp) {
    CostBreakdown b;
    if (!is_priced(model)) return b;

    auto& p = pricing_for(model);

    int plain_input = resp.input_tokens
                    - resp.cache_read_tokens
                    - resp.cache_creation_tokens;
    if (plain_input < 0) plain_input = 0;

    b.input        = (plain_input                / 1e6) * p.input_per_mtok;
    b.output       = (resp.output_tokens         / 1e6) * p.output_per_mtok;
    b.cache_read   = (resp.cache_read_tokens     / 1e6) * p.cache_read_per_mtok;
    b.cache_create = (resp.cache_creation_tokens / 1e6) * p.cache_write_per_mtok;
    return b;
}

double CostTracker::compute_cost(const std::string& model, const ApiResponse& resp) {
    // Local providers (ollama/…) are free — don't invent a bogus price by
    // falling through to the default pricing table.
    auto b = compute_cost_breakdown(model, resp);
    return b.input + b.output + b.cache_read + b.cache_create;
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
