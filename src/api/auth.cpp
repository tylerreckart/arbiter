// arbiter/src/api/auth.cpp

#include "api/auth.h"
#include "api/http_response.h"

#include "tenant_store.h"

namespace arbiter {

// Extract the bearer token from an Authorization header, or empty if missing.
std::string extract_bearer(const HttpRequest& req) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return {};
    static constexpr const char* kPrefix = "Bearer ";
    static constexpr size_t      kPrefixLen = 7;
    const std::string& hdr = it->second;
    if (hdr.size() <= kPrefixLen ||
        hdr.compare(0, kPrefixLen, kPrefix) != 0)
        return {};
    return hdr.substr(kPrefixLen);
}

// Single-threaded helper for request handlers: refresh a Tenant snapshot
// when still authorized.  Provider I/O kill-switch uses TenantGate
// (thread-safe, bound to ApiClient preflight) — do not call this from
// parallel workers or preflight callbacks.
bool refresh_active_tenant(TenantStore& tenants, Tenant& tenant) {
    auto fresh = tenants.get_tenant(tenant.id);
    if (!fresh || fresh->disabled) return false;
    if (fresh->api_key_hash != tenant.api_key_hash) return false;
    tenant = *fresh;
    return true;
}

void reject_disabled_tenant(int fd) {
    write_plain_response(fd, 401, "Unauthorized",
                         "missing or invalid bearer token\n");
}

TenantPreflight::TenantPreflight(std::shared_ptr<TenantGate> g, ApiClient& c)
    : gate(std::move(g)), client(c) {
    auto held = gate;
    client.set_preflight([held]() { return held->alive(); });
}

TenantPreflight::~TenantPreflight() { client.clear_preflight(); }

} // namespace arbiter
