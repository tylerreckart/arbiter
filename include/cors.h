#pragma once
// arbiter/include/cors.h
//
// CORS policy for the HTTP API.  Default is permissive (`*`) so a SPA on
// any origin can hit the API in local development.  Set
// ARBITER_CORS_ORIGINS to a comma-separated allowlist to echo only
// matching Origin values (documented alternative to putting the
// allowlist in a reverse proxy).

#include <string>
#include <vector>

namespace arbiter {

struct CorsPolicy {
    // When true (unset ARBITER_CORS_ORIGINS), emit ACAO: *.
    // When false (env set), only echo Origin for allowlist matches.
    bool allow_all = true;
    std::vector<std::string> allowed_origins;
};

// Parse ARBITER_CORS_ORIGINS (CSV).  Whitespace around entries is trimmed.
// Empty / unset ⇒ permissive policy.
CorsPolicy load_cors_policy_from_env();

// Parse a CSV allowlist string (used by load_cors_policy_from_env and tests).
CorsPolicy cors_policy_from_csv(const std::string& csv);

// True when the policy is permissive (`*`) or `origin` is on the allowlist.
// Empty origin never matches a non-permissive allowlist.
bool cors_origin_allowed(const CorsPolicy& policy, const std::string& origin);

// Build the CORS response-header block (including trailing CRLF on each
// line, no final blank line).  When the policy is an allowlist and
// `origin` is not allowed, omits Access-Control-Allow-Origin entirely
// (browser will block the cross-origin read).
std::string cors_headers_for(const CorsPolicy& policy,
                             const std::string& origin);

} // namespace arbiter
