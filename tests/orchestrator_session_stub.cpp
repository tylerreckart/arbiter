// Minimal stand-in for the two Orchestrator methods ConversationStore calls
// (load_session/save_session). Keeps unit_conversation_store from having to
// link the full orchestrator.cpp dependency chain (agent, api_client,
// advisor, commands, ...) since those tests never construct a real
// Orchestrator — they only need these two symbols to resolve at link time.
#include "orchestrator.h"

namespace arbiter {

bool Orchestrator::load_session(const std::string&) { return false; }
void Orchestrator::save_session(const std::string&) const {}

} // namespace arbiter
