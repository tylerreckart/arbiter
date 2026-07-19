#pragma once
// Encode/decode Message vectors in the session / conversation-store shape
// (role, content, optional thinking, optional tool_trace).  Shared by
// Orchestrator::save/load_session and unit tests for round-trip fidelity.

#include "api_client.h"

#include <string>
#include <vector>

namespace arbiter {

[[nodiscard]] std::string encode_messages_json(const std::vector<Message>& msgs);
[[nodiscard]] std::vector<Message> decode_messages_json(const std::string& json);

} // namespace arbiter
