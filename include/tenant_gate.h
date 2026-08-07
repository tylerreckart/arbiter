#pragma once
// TenantGate — durable kill-switch probe for multi-tenant API requests.
//
// Created once after bearer auth with the tenant id + api_key_hash snapshot.
// `alive()` re-reads TenantStore and returns false when the tenant is
// disabled or the digest has rotated.  It never mutates caller state, so
// it is safe to call concurrently from /parallel worker threads and from
// ApiClient streaming read loops.
//
// Bind to an ApiClient by capturing the shared_ptr in set_preflight:
//   auto gate = TenantGate::create(store, tenant);
//   client.set_preflight([gate]() { return gate->alive(); });
// /parallel children inherit that preflight via ApiClient::copy_preflight_from.

#include "tenant_store.h"

#include <cstdint>
#include <memory>
#include <string>

namespace arbiter {

class TenantGate : public std::enable_shared_from_this<TenantGate> {
public:
    static std::shared_ptr<TenantGate> create(TenantStore& store,
                                              const Tenant& tenant) {
        return std::shared_ptr<TenantGate>(new TenantGate(store, tenant));
    }

    int64_t id() const { return id_; }
    const std::string& api_key_hash() const { return api_key_hash_; }

    // Thread-safe DB probe.  Never mutates.  Safe for /parallel workers.
    bool alive() const {
        auto fresh = store_.get_tenant(id_);
        return fresh && !fresh->disabled &&
               fresh->api_key_hash == api_key_hash_;
    }

    // Single-threaded helper for request handlers: update a Tenant
    // snapshot when still authorized, else return false.
    bool refresh(Tenant& tenant) const {
        auto fresh = store_.get_tenant(id_);
        if (!fresh || fresh->disabled) return false;
        if (fresh->api_key_hash != api_key_hash_) return false;
        tenant = *fresh;
        return true;
    }

private:
    TenantGate(TenantStore& store, const Tenant& tenant)
        : store_(store),
          id_(tenant.id),
          api_key_hash_(tenant.api_key_hash) {}

    TenantStore&      store_;
    const int64_t     id_;
    const std::string api_key_hash_;
};

}  // namespace arbiter
