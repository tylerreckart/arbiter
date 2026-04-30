#pragma once
// index/include/billing_client.h
//
// Thin client for the external billing service that arbiter delegates
// eligibility checks and per-turn usage tracking to.  Wraps three
// runtime hot-path endpoints:
//
//   POST /v1/runtime/auth/validate   — exchange an `atr_…` bearer token for
//                                       its workspace_id + tenant_id.
//   POST /v1/runtime/quota/check     — pre-flight allow/deny against the
//                                       tenant's plan cap + credit balance.
//   POST /v1/runtime/usage/record    — post-turn fire-and-forget telemetry.
//
// When `base_url` is empty the runtime treats billing as unconfigured:
// every call becomes a no-op that returns "allowed" so requests still
// route through to the configured provider API keys.  This is the
// documented escape hatch for self-hosted deploys that don't run an
// external billing service.
//
// The protocol above is what an operator's billing service must
// implement; arbiter ships no reference implementation under this
// repository.
//
// Auth/validate results are cached in-process by SHA-256(token) for
// `ttl_seconds` (whatever the server returned) so back-to-back requests
// from the same tenant don't pay an extra round-trip per call.  The
// cache is bounded; entries are evicted lazily on next lookup.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace index_ai {

class BillingClient {
public:
    // base_url like "http://localhost:4000"; empty = disabled.
    explicit BillingClient(std::string base_url);

    bool enabled() const { return !base_url_.empty(); }

    struct AuthResult {
        bool        ok           = false;
        int         http_status  = 0;
        std::string workspace_id;        // "ws_…" on ok
        std::string tenant_id;           // "ten_…" on ok
        int         ttl_seconds  = 0;
        std::string error_code;          // e.g. "invalid_token", "tenant_suspended"
        std::string message;             // human-readable, optional
    };

    // POST /v1/runtime/auth/validate.  Cached by sha256(token) for the
    // server-returned TTL.  Disabled-mode callers should not invoke
    // this; the api server only routes here when enabled().
    AuthResult validate(const std::string& token);

    struct QuotaResult {
        bool        ok           = false;   // wire success (HTTP 200/4xx parsed)
        int         http_status  = 0;
        bool        allow        = true;    // default-allow when disabled
        std::string reason;                  // tenant_suspended | tenant_disabled | insufficient_budget
        std::string message;                 // safe to surface to end users
        int64_t     estimated_cost_uc = 0;
        int64_t     plan_remaining_uc = 0;   // -1 sentinel ⇒ unlimited (wire null)
        int64_t     credit_balance_uc = 0;
        int64_t     total_budget_uc   = 0;   // -1 sentinel ⇒ unlimited
    };

    // POST /v1/runtime/quota/check.  When disabled, returns allow=true
    // with all numbers zeroed — caller should just proceed.
    QuotaResult check_quota(const std::string& workspace_id,
                             const std::string& model,
                             int est_input_tokens,
                             int est_output_tokens,
                             const std::string& request_id = "");

    struct UsageRecord {
        std::string request_id;       // idempotency key — must be stable across retries
        std::string workspace_id;
        std::string model;
        int         input_tokens   = 0;
        int         output_tokens  = 0;
        int         cached_tokens  = 0;
        int         duration_ms    = 0;
        std::string agent_id;
        int         depth          = 0;
    };

    // POST /v1/runtime/usage/record.  Fire-and-forget — this method
    // returns immediately; the actual HTTP call runs on a detached
    // background thread and is allowed to fail (idempotent retries
    // happen at the caller's discretion).  When disabled, the call
    // is dropped without contacting the network.
    void record_usage(const UsageRecord& rec);

private:
    // Single libcurl-driven POST.  Caller-supplied endpoint is appended
    // to base_url_; body is the request payload, body_out captures the
    // response body.  Returns the HTTP status, or 0 on transport error
    // (caller's `error_code` should then be populated as "transport_error").
    int post_json(const std::string& path,
                  const std::string& body,
                  std::string&        body_out,
                  int                 timeout_seconds = 5);

    std::string base_url_;             // e.g. "http://localhost:4000"

    struct AuthEntry {
        AuthResult                                       result;
        std::chrono::steady_clock::time_point            expires_at;
    };
    std::mutex                                  auth_mu_;
    std::unordered_map<std::string, AuthEntry>  auth_cache_;   // key = sha256(token)
};

} // namespace index_ai
