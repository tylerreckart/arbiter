#pragma once
// arbiter/include/api/auth.h
//
// Bearer auth and tenant kill-switch preflight for HTTP handlers.

#include "api/http_request.h"
#include "api_client.h"
#include "tenant_gate.h"
#include "tenant_store.h"

#include <memory>
#include <string>

namespace arbiter {

struct Tenant;

std::string extract_bearer(const HttpRequest& req);
bool refresh_active_tenant(TenantStore& tenants, Tenant& tenant);
void reject_disabled_tenant(int fd);

struct TenantPreflight {
    std::shared_ptr<TenantGate> gate;
    ApiClient&                  client;
    TenantPreflight(std::shared_ptr<TenantGate> g, ApiClient& c);
    ~TenantPreflight();
    TenantPreflight(const TenantPreflight&)            = delete;
    TenantPreflight& operator=(const TenantPreflight&) = delete;
};

} // namespace arbiter
