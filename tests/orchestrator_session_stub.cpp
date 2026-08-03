// Minimal stand-in for the Orchestrator session methods ConversationStore
// calls. Keeps unit_conversation_store from having to link the full
// orchestrator.cpp dependency chain.
#include "orchestrator.h"

namespace arbiter {

bool Orchestrator::load_session(const std::string&) { return false; }
void Orchestrator::save_session(const std::string&) const {}
bool Orchestrator::load_session_json(const std::string&) { return false; }
std::string Orchestrator::session_to_json() const {
    return R"({"version":2,"index":[],"agents":{},"compaction":{}})";
}

} // namespace arbiter
