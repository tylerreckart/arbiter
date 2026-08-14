#pragma once
// arbiter/include/api/in_flight_scope.h
//
// RAII registration of in-flight orchestrations for cancel routing.

#include "api_server.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace arbiter {

class Orchestrator;

class InFlightScope {
public:
    InFlightScope(InFlightRegistry& reg, std::string id,
                  Orchestrator* orch, int64_t tenant_id,
                  std::atomic<bool>* cancel_flag = nullptr);
    ~InFlightScope();
    InFlightScope(const InFlightScope&)            = delete;
    InFlightScope& operator=(const InFlightScope&) = delete;

private:
    InFlightRegistry& reg_;
    std::string       id_;
};

} // namespace arbiter
