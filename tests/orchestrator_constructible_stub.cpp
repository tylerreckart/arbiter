// Like orchestrator_session_stub.cpp, but also provides a real constructor
// so tests can hold an actual Orchestrator object (needed to exercise
// ConversationStore::save_async/flush end to end). client_ is a real
// ApiClient value member, so whichever target uses this stub must also link
// api_client.cpp + circuit_breaker.cpp + metrics.cpp (and OpenSSL/libcurl) —
// still far short of the full orchestrator.cpp chain (advisor, commands,
// mcp, a2a, scheduler, ...), which send()/execute_slash_command() need but
// construction and session save/load do not.
#include "orchestrator.h"

namespace arbiter {

Orchestrator::Orchestrator(std::map<std::string, std::string> api_keys)
    : client_(api_keys), api_keys_(std::move(api_keys)) {}

bool Orchestrator::load_session(const std::string&) { return false; }
void Orchestrator::save_session(const std::string&) const {}

} // namespace arbiter
