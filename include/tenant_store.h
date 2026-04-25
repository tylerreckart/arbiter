#pragma once
// index/include/tenant_store.h
//
// SQLite-backed tenant + usage accounting for the HTTP API.
//
// Each tenant carries:
//   • An opaque plaintext API token (shown to the user once at creation,
//     stored in the DB only as a SHA-256 hex digest).
//   • A display name for CLI reporting.
//   • An optional monthly usage cap in micro-cents (0 = unlimited).
//   • A rolling month-to-date usage total, reset at the start of each
//     billing month (computed from wall-clock UTC on record_usage).
//   • A disabled flag for admin kill-switches.
//
// Usage is logged per LLM turn with provider cost + our markup, both in
// micro-cents.  The API server increments month_to_date inline; the
// usage_log table stays append-only for invoicing.
//
// "Micro-cents" (µ¢): 1 USD = 1_000_000 µ¢.  Keeps everything integer
// while faithfully representing fractional-cent LLM costs.  Display
// conversions live at the bottom of this header.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace index_ai {

struct Tenant {
    int64_t     id                 = 0;
    std::string api_key_hash;            // SHA-256 hex of the plaintext token
    std::string name;
    int64_t     monthly_cap_uc     = 0;  // 0 = unlimited
    std::string month_yyyymm;            // e.g. "2026-04" — the MTD period
    int64_t     month_to_date_uc   = 0;
    bool        disabled           = false;
    int64_t     created_at         = 0;  // epoch seconds
    int64_t     last_used_at       = 0;  // epoch seconds (0 if never)
};

// One row from the append-only usage_log table.  All monetary fields
// in micro-cents.  Returned by TenantStore::list_usage for the admin
// API's billing-ledger read path.
//
// Cost breakdown rationale: we capture per-token-type cost at write time
// rather than recomputing from tokens × pricing on read.  Pricing tables
// drift (vendors raise/lower rates, we update kPricingEntries); historical
// rows must reflect the rate that was actually billed at the time of the
// call.  provider_uc is the denormalized sum of the four component costs.
struct UsageEntry {
    int64_t     id                  = 0;
    int64_t     tenant_id           = 0;
    int64_t     timestamp           = 0;  // epoch seconds
    std::string model;
    int64_t     input_tokens        = 0;  // total input (incl. cached)
    int64_t     output_tokens       = 0;
    int64_t     cache_read_tokens   = 0;  // subset of input that hit a cache
    int64_t     cache_create_tokens = 0;  // tokens written to cache (Anthropic only)
    int64_t     input_uc            = 0;  // cost for plain (non-cached) input
    int64_t     output_uc           = 0;  // cost for output tokens
    int64_t     cache_read_uc       = 0;  // cost for cache-read tokens (cheaper rate)
    int64_t     cache_create_uc     = 0;  // cost for cache-write tokens (premium rate)
    int64_t     provider_uc         = 0;  // sum of the four above
    int64_t     markup_uc           = 0;  // 20% over provider_uc, rounded up
    std::string request_id;               // empty if unset
};

class TenantStore {
public:
    TenantStore() = default;
    ~TenantStore();

    TenantStore(const TenantStore&)            = delete;
    TenantStore& operator=(const TenantStore&) = delete;

    // Open (or create) the SQLite file at `path`.  Runs migrations on
    // every open — safe to re-run.
    void open(const std::string& path);

    // Create a tenant.  Returns the resulting Tenant record plus the
    // plaintext token — the only time the plaintext is ever visible;
    // subsequent startups only hold the hash.
    struct CreatedTenant { Tenant tenant; std::string token; };
    CreatedTenant create_tenant(const std::string& name, int64_t monthly_cap_uc);

    // Disable or re-enable a tenant.  `key` matches either the numeric id
    // or the display name (first hit wins).  Returns true on success.
    bool set_disabled(const std::string& key, bool disabled);

    // Update the monthly cap (µ¢; 0 = unlimited).  Admin-only; no effect on
    // in-flight requests that have already passed the pre-flight check.
    // Returns true if a tenant with this id exists.
    bool set_cap(int64_t tenant_id, int64_t cap_uc);

    // Look up by plaintext token.  Returns nullopt if the token isn't
    // valid, the tenant is disabled, or the DB is closed.  Updates
    // last_used_at in the process.
    std::optional<Tenant> find_by_token(const std::string& token);

    // Per-token-type cost breakdown captured at write time.  All values in
    // micro-cents.  The four component fields must sum to provider_uc — the
    // record_usage caller is responsible for that math (CostTracker::
    // compute_cost_breakdown does it).
    struct CostParts {
        int64_t input_uc        = 0;
        int64_t output_uc       = 0;
        int64_t cache_read_uc   = 0;
        int64_t cache_create_uc = 0;
    };

    // Record one LLM turn's usage.  Handles month-rollover by resetting
    // month_to_date_uc when the current UTC month differs from the
    // tenant's stored month_yyyymm.  Returns the post-update MTD in
    // micro-cents — callers use this to check against monthly_cap_uc
    // and abort mid-stream if needed.
    int64_t record_usage(int64_t tenant_id,
                         const std::string& model,
                         int input_tokens,
                         int output_tokens,
                         int cache_read_tokens,
                         int cache_create_tokens,
                         const CostParts& parts,
                         int64_t markup_uc,
                         const std::string& request_id = "");

    std::vector<Tenant> list_tenants() const;
    std::optional<Tenant> get_tenant(int64_t id) const;

    // Read rows from usage_log, newest first.  Any filter argument set
    // to 0 (or negative, for `limit`) is ignored:
    //   tenant_id == 0  → all tenants
    //   since     == 0  → no lower bound
    //   until     == 0  → no upper bound
    //   limit    <= 0   → default cap (1000)
    // Used by the admin /v1/admin/usage endpoint so a sibling billing
    // service can paginate through the ledger without touching SQLite
    // directly.
    std::vector<UsageEntry> list_usage(int64_t tenant_id,
                                       int64_t since_ts,
                                       int64_t until_ts,
                                       int     limit) const;

    // Aggregated rollup for analytics: one bucket per distinct value of
    // `group_by` (model | day | tenant), summing tokens + costs over the
    // filtered window.  Saves a sibling service from pulling N raw rows
    // just to render a chart.  group_by:
    //   "model"  → key = the model string                    ("claude-sonnet-4-6")
    //   "day"    → key = "YYYY-MM-DD" (UTC)                  ("2026-04-23")
    //   "tenant" → key = tenant id as string                  ("3")
    // Any unrecognized group_by falls back to "model".
    struct UsageBucket {
        std::string key;
        int64_t     calls               = 0;
        int64_t     input_tokens        = 0;
        int64_t     output_tokens       = 0;
        int64_t     cache_read_tokens   = 0;
        int64_t     cache_create_tokens = 0;
        int64_t     input_uc            = 0;
        int64_t     output_uc           = 0;
        int64_t     cache_read_uc       = 0;
        int64_t     cache_create_uc     = 0;
        int64_t     provider_uc         = 0;
        int64_t     markup_uc           = 0;
    };
    std::vector<UsageBucket> usage_summary(int64_t tenant_id,
                                            int64_t since_ts,
                                            int64_t until_ts,
                                            const std::string& group_by) const;

private:
    sqlite3* db_ = nullptr;

    // Re-read a tenant row into `t`.  Used internally after mutations.
    bool reload_tenant(int64_t id, Tenant& t) const;
};

// ─── Unit conversions ──────────────────────────────────────────────────────

inline int64_t usd_to_uc(double usd) {
    // Round half-away-from-zero so a 0.0000049 USD call still registers.
    return usd >= 0 ? static_cast<int64_t>(usd * 1'000'000.0 + 0.5)
                    : -static_cast<int64_t>(-usd * 1'000'000.0 + 0.5);
}
inline double uc_to_usd(int64_t uc) {
    return static_cast<double>(uc) / 1'000'000.0;
}
// Round up to the nearest whole cent — always billed in our favor so
// fractional-cent accumulations round toward the charge.
inline int64_t uc_to_cents_ceil(int64_t uc) {
    return (uc + 9999) / 10000;
}

// Markup: 20% over provider cost, rounded up to the nearest µ¢ so we
// never undercharge due to integer truncation.
inline int64_t markup_uc(int64_t provider_uc) {
    return (provider_uc * 20 + 99) / 100;
}

} // namespace index_ai
