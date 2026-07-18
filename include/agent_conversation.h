#pragma once
// Thread-local conversation key for Agent history isolation.
//
// Pane exec threads (and ConversationStore load/save) enter a
// ConversationScope so Agent::history/set_history/reset_history operate on
// that conversation's slot.  Outside a scope the default "" key is used
// (API server, unit tests, single-conversation paths).

#include <string>

namespace arbiter {

namespace detail {
inline thread_local const std::string* g_agent_conversation_key = nullptr;
} // namespace detail

inline const std::string& agent_conversation_key() {
    static const std::string kDefault;
    return detail::g_agent_conversation_key ? *detail::g_agent_conversation_key
                                            : kDefault;
}

// RAII binder for the current thread's conversation history key.
class ConversationScope {
public:
    explicit ConversationScope(std::string id)
        : id_(std::move(id)), prev_(detail::g_agent_conversation_key) {
        detail::g_agent_conversation_key = &id_;
    }
    ~ConversationScope() { detail::g_agent_conversation_key = prev_; }

    ConversationScope(const ConversationScope&) = delete;
    ConversationScope& operator=(const ConversationScope&) = delete;

    [[nodiscard]] const std::string& id() const { return id_; }

private:
    std::string id_;
    const std::string* prev_;
};

} // namespace arbiter
