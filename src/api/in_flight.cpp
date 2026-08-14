// arbiter/src/api/in_flight.cpp

#include "api/in_flight_scope.h"

#include "orchestrator.h"

#include <mutex>

namespace arbiter {

InFlightScope::InFlightScope(InFlightRegistry& reg, std::string id,
                             Orchestrator* orch, int64_t tenant_id,
                             std::atomic<bool>* cancel_flag)
    : reg_(reg), id_(std::move(id)) {
    std::lock_guard<std::mutex> lk(reg_.mu);
    reg_.by_id[id_] = {orch, tenant_id, cancel_flag};
}

InFlightScope::~InFlightScope() {
    std::lock_guard<std::mutex> lk(reg_.mu);
    reg_.by_id.erase(id_);
}

void InFlightRegistry::cancel_for_tenant(int64_t tenant_id) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& [_, entry] : by_id) {
        if (entry.tenant_id == tenant_id) {
            if (entry.orch) entry.orch->cancel();
            if (entry.cancel_flag) entry.cancel_flag->store(true);
        }
    }
}

} // namespace arbiter
