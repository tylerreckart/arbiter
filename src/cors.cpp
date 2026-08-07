// arbiter/src/cors.cpp

#include "cors.h"

#include <cstdlib>
#include <cctype>

namespace arbiter {

namespace {

std::string trim(std::string s) {
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() &&
           std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

constexpr const char* kCorsMethods =
    "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n";
constexpr const char* kCorsAllowHeaders =
    "Access-Control-Allow-Headers: Authorization, Content-Type, Accept, "
    "Idempotency-Key, If-None-Match\r\n";
constexpr const char* kCorsMaxAge =
    "Access-Control-Max-Age: 86400\r\n";

} // namespace

CorsPolicy cors_policy_from_csv(const std::string& csv) {
    CorsPolicy p;
    if (csv.empty()) return p;
    size_t i = 0;
    while (i < csv.size()) {
        size_t comma = csv.find(',', i);
        std::string entry = trim(
            csv.substr(i, comma == std::string::npos ? std::string::npos
                                                     : comma - i));
        if (!entry.empty()) p.allowed_origins.push_back(std::move(entry));
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return p;
}

CorsPolicy load_cors_policy_from_env() {
    const char* v = std::getenv("ARBITER_CORS_ORIGINS");
    if (!v || !*v) return CorsPolicy{};
    CorsPolicy p = cors_policy_from_csv(v);
    p.allow_all = false;
    return p;
}

bool cors_origin_allowed(const CorsPolicy& policy,
                         const std::string& origin) {
    if (policy.allow_all) return true;
    if (origin.empty()) return false;
    for (const auto& o : policy.allowed_origins) {
        if (o == origin) return true;
    }
    return false;
}

std::string cors_headers_for(const CorsPolicy& policy,
                             const std::string& origin) {
    std::string h;
    if (policy.allow_all) {
        h += "Access-Control-Allow-Origin: *\r\n";
    } else if (cors_origin_allowed(policy, origin)) {
        h += "Access-Control-Allow-Origin: " + origin + "\r\n";
        // Vary so caches don't serve one origin's ACAO to another.
        h += "Vary: Origin\r\n";
    }
    // When allowlisted but origin is missing/mismatched, omit ACAO.
    h += kCorsMethods;
    h += kCorsAllowHeaders;
    h += kCorsMaxAge;
    return h;
}

} // namespace arbiter
