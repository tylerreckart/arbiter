// index_ai/src/agent.cpp
#include "agent.h"
#include "json.h"
#include <sstream>

namespace index_ai {

Agent::Agent(const std::string& id, Constitution config, ApiClient& client)
    : id_(id), config_(std::move(config)), client_(client)
{
    stats_.created = std::chrono::steady_clock::now();
}

// Prepend context summary as a synthetic leading exchange so the model has
// continuity without re-paying the token cost of the full prior history.
static std::vector<Message> inject_summary(const std::string& summary,
                                           std::vector<Message> msgs) {
    if (summary.empty()) return msgs;
    msgs.insert(msgs.begin(), Message{"assistant", "Context loaded."});
    msgs.insert(msgs.begin(),
        Message{"user", "[CONTEXT SUMMARY]\n" + summary + "\n[END CONTEXT SUMMARY]"});
    return msgs;
}

static constexpr size_t kAutoCompactAt  = 20;
static constexpr size_t kHardTrimAt     = 28;
static constexpr int    kKeepAfterTrim  = 16;

ApiResponse Agent::send(const std::string& user_message) {
    // Auto-compact before adding the new message so the summary reflects the
    // complete prior conversation, not a half-appended state.
    if (history_.size() >= kAutoCompactAt) {
        if (compact_cb_) compact_cb_(id_, history_.size());
        compact();
    }

    // Add user message to history
    history_.push_back(Message{"user", user_message});

    // Hard-trim safety net: if compact() was skipped (e.g. it failed) or
    // history grew very large via tool-result injections, drop the oldest pairs.
    if (history_.size() > kHardTrimAt) {
        size_t before = history_.size();
        trim_history(kKeepAfterTrim);
        fprintf(stderr, "[%s] context trimmed: %zu → %zu messages (oldest dropped)\n",
                id_.c_str(), before, history_.size());
    }

    // Build request
    ApiRequest req;
    req.model         = config_.model;
    req.system_prompt = config_.build_system_prompt();
    req.max_tokens    = config_.max_tokens;
    req.temperature   = config_.temperature;
    req.messages      = inject_summary(context_summary_, history_);
    req.advisor_model = config_.advisor_model;

    auto resp = client_.complete(req);

    if (resp.ok) {
        // Tombstone: if the user message we just sent was a large [TOOL RESULTS]
        // block, compress it in history now that the agent has processed it.
        // The agent's response already incorporates those results; carrying the
        // raw content forward is pure token waste.
        static constexpr size_t kTombstoneThreshold = 4096;
        if (!history_.empty()) {
            auto& last_user = history_.back();
            if (last_user.role == "user" &&
                last_user.content.size() > kTombstoneThreshold &&
                last_user.content.find("[TOOL RESULTS]") != std::string::npos) {
                last_user.content =
                    "[TOOL RESULTS - processed, " +
                    std::to_string(last_user.content.size()) +
                    " bytes, results incorporated in prior response]";
            }
        }

        // Add assistant response to history
        history_.push_back(Message{"assistant", resp.content});
        stats_.total_input_tokens  += resp.input_tokens;
        stats_.total_output_tokens += resp.output_tokens;
        stats_.total_requests++;
    }

    return resp;
}

ApiResponse Agent::stream(const std::string& user_message, StreamCallback cb) {
    if (history_.size() >= kAutoCompactAt) {
        if (compact_cb_) compact_cb_(id_, history_.size());
        compact();
    }

    history_.push_back(Message{"user", user_message});

    if (history_.size() > kHardTrimAt) {
        size_t before = history_.size();
        trim_history(kKeepAfterTrim);
        fprintf(stderr, "[%s] context trimmed: %zu → %zu messages (oldest dropped)\n",
                id_.c_str(), before, history_.size());
    }

    ApiRequest req;
    req.model         = config_.model;
    req.system_prompt = config_.build_system_prompt();
    req.max_tokens    = config_.max_tokens;
    req.temperature   = config_.temperature;
    req.messages      = inject_summary(context_summary_, history_);
    req.advisor_model = config_.advisor_model;

    auto resp = client_.stream(req, cb);

    if (resp.ok) {
        static constexpr size_t kTombstoneThreshold = 4096;
        if (!history_.empty()) {
            auto& last_user = history_.back();
            if (last_user.role == "user" &&
                last_user.content.size() > kTombstoneThreshold &&
                last_user.content.find("[TOOL RESULTS]") != std::string::npos) {
                last_user.content =
                    "[TOOL RESULTS - processed, " +
                    std::to_string(last_user.content.size()) +
                    " bytes, results incorporated in prior response]";
            }
        }
        history_.push_back(Message{"assistant", resp.content});
        stats_.total_input_tokens  += resp.input_tokens;
        stats_.total_output_tokens += resp.output_tokens;
        stats_.total_requests++;
    }

    return resp;
}

void Agent::reset_history() {
    history_.clear();
}

void Agent::trim_history(int keep_last_n) {
    if (static_cast<int>(history_.size()) > keep_last_n) {
        history_.erase(
            history_.begin(),
            history_.begin() + (history_.size() - keep_last_n)
        );
        // Ensure first message is user (API requirement)
        while (!history_.empty() && history_.front().role != "user") {
            history_.erase(history_.begin());
        }
    }
}

std::string Agent::compact() {
    if (history_.empty()) return "";

    // Build a plain-text transcript of the current history.
    std::ostringstream transcript;
    for (auto& m : history_) {
        transcript << m.role << ": " << m.content << "\n\n";
    }

    // One-shot summarization request — does NOT touch history_.
    ApiRequest req;
    req.model       = config_.model;
    req.max_tokens  = 1024;
    req.temperature = 0.3;
    req.system_prompt =
        "You are a context compactor. Given a conversation transcript, produce a "
        "concise continuity summary. Capture: decisions made, facts established, "
        "tasks completed or in-progress, open questions, and current working state. "
        "Plain text only. No markdown. No pleasantries. Be as brief as the content allows.";
    req.messages = {{
        "user",
        "Summarize this conversation for context continuity:\n\n"
        "[TRANSCRIPT]\n" + transcript.str() + "[END TRANSCRIPT]"
    }};

    auto resp = client_.complete(req);
    if (!resp.ok || resp.content.empty()) return "";

    // Store as session memory and clear the window.
    context_summary_ = resp.content;
    history_.clear();

    return context_summary_;
}

std::string Agent::status_summary() const {
    std::ostringstream ss;
    ss << id_ << " | " << config_.role
       << " | msgs:" << history_.size()
       << " | in:" << stats_.total_input_tokens
       << " out:" << stats_.total_output_tokens
       << " | reqs:" << stats_.total_requests;
    if (!config_.advisor_model.empty())
        ss << " | advisor:" << config_.advisor_model;
    return ss.str();
}

std::string Agent::to_json() const {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["id"] = jstr(id_);
    m["config"] = json_parse(config_.to_json());

    auto hist = jarr();
    for (auto& msg : history_) {
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);
        mo->as_object_mut()["content"] = jstr(msg.content);
        hist->as_array_mut().push_back(mo);
    }
    m["history"] = hist;

    auto st = jobj();
    st->as_object_mut()["input_tokens"]  = jnum(static_cast<double>(stats_.total_input_tokens));
    st->as_object_mut()["output_tokens"] = jnum(static_cast<double>(stats_.total_output_tokens));
    st->as_object_mut()["requests"]      = jnum(static_cast<double>(stats_.total_requests));
    m["stats"] = st;

    return json_serialize(*obj);
}

} // namespace index_ai
