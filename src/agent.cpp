// arbiter/src/agent.cpp
#include "agent.h"
#include "json.h"
#include <sstream>

namespace arbiter {

Agent::Agent(const std::string& id, Constitution config, ApiClient& client)
    : id_(id), config_(std::move(config)), client_(client)
{
    stats_.created = std::chrono::steady_clock::now();
}

void Agent::continue_until_done(ApiResponse& resp, StreamCallback cb) {
    // Long responses hit the per-turn max_tokens ceiling and stop mid-sentence
    // (or worse, mid-/write block).  We feed the partial back as the assistant
    // turn, ask "continue", and concatenate until the model finishes cleanly.
    // The caller sees one merged response and one merged assistant history
    // entry — the intermediate partials are pushed here and popped before we
    // return so history stays clean.
    static constexpr int kMaxContinues = 3;
    static const std::string kContinuePrompt =
        "Continue exactly where you left off mid-generation.  Do NOT repeat "
        "anything already written.  Do NOT add preamble, headers, or "
        "acknowledgements.  Pick up at the exact character where the previous "
        "response was cut off — even mid-word or mid-line — and keep going "
        "until the response is genuinely complete.";

    int continues = 0;
    while (resp.ok && resp.stop_reason == "max_tokens" && continues < kMaxContinues) {
        ++continues;

        ApiRequest req;
        req.model         = config_.model;
        req.system_prompt = config_.build_system_prompt();
        req.max_tokens    = config_.max_tokens;
        req.temperature   = config_.temperature;
        {
            std::lock_guard<std::mutex> lk(history_mu_);
            auto& hist = histories_[agent_conversation_key()];
            hist.push_back(Message{"assistant", resp.content});
            hist.push_back(Message{"user", kContinuePrompt});
            req.messages = hist;
        }

        ApiResponse more = cb ? client_.stream(req, cb) : client_.complete(req);

        {
            std::lock_guard<std::mutex> lk(history_mu_);
            auto& hist = histories_[agent_conversation_key()];
            hist.pop_back();   // remove continue prompt
            hist.pop_back();   // remove partial assistant
        }

        if (!more.ok) {
            // Keep whatever we already accumulated; stop_reason stays
            // "max_tokens" so the caller knows the response is unfinished,
            // and resp.error surfaces the continuation failure.
            resp.error = more.error.empty() ? "continuation failed"
                                            : "continuation failed: " + more.error;
            break;
        }

        resp.content               += more.content;
        resp.reasoning             += more.reasoning;
        resp.input_tokens          += more.input_tokens;
        resp.output_tokens         += more.output_tokens;
        resp.cache_read_tokens     += more.cache_read_tokens;
        resp.cache_creation_tokens += more.cache_creation_tokens;
        resp.stop_reason            = more.stop_reason;
    }
}

// Concatenate the TEXT parts of a multipart message into a single string.
// Used for the legacy text-only fields on Message that downstream code
// still inspects (tombstoning, [TOOL RESULTS] detection, history bytes).
// Image parts contribute nothing to the concatenation — they're carried
// alongside in `parts` for the wire path and for re-entry forwarding.
static std::string concatenate_text(const std::vector<ContentPart>& parts) {
    std::string out;
    for (auto& p : parts) {
        if (p.kind == ContentPart::TEXT) {
            if (!out.empty() && out.back() != '\n') out += '\n';
            out += p.text;
        }
    }
    return out;
}

ApiResponse Agent::send(const std::string& user_message) {
    std::vector<ContentPart> parts;
    ContentPart p;
    p.kind = ContentPart::TEXT;
    p.text = user_message;
    parts.push_back(std::move(p));
    return send(std::move(parts));
}

ApiResponse Agent::send(std::vector<ContentPart> parts) {
    // Add user message to history.  Invariant: `parts` is populated only
    // when the message is genuinely multipart (image-bearing or multi-text).
    // A single text part collapses back to the legacy `content`-only shape
    // so downstream inspection and the body builders' fast path keep working. Images
    // contribute zero bytes to `content` regardless.
    Message user_msg;
    user_msg.role = "user";
    const bool is_single_text =
        parts.size() == 1 && parts.front().kind == ContentPart::TEXT;
    if (is_single_text) {
        user_msg.content = std::move(parts.front().text);
    } else {
        user_msg.content = concatenate_text(parts);
        user_msg.parts   = std::move(parts);
    }

    // Build request
    ApiRequest req;
    req.model         = config_.model;
    req.system_prompt = config_.build_system_prompt();
    req.max_tokens    = config_.max_tokens;
    req.temperature   = config_.temperature;
    {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto& hist = histories_[agent_conversation_key()];
        hist.push_back(std::move(user_msg));
        req.messages = hist;
    }

    auto resp = client_.complete(req);

    if (resp.ok) {
        continue_until_done(resp, nullptr);
        // Add assistant response to history
        std::lock_guard<std::mutex> lk(history_mu_);
        Message am{"assistant", resp.content};
        am.thinking = resp.reasoning;
        histories_[agent_conversation_key()].push_back(std::move(am));
        stats_.total_input_tokens  += resp.input_tokens;
        stats_.total_output_tokens += resp.output_tokens;
        stats_.total_requests++;
    }

    return resp;
}

ApiResponse Agent::stream(const std::string& user_message, StreamCallback cb) {
    std::vector<ContentPart> parts;
    ContentPart p;
    p.kind = ContentPart::TEXT;
    p.text = user_message;
    parts.push_back(std::move(p));
    return stream(std::move(parts), std::move(cb));
}

ApiResponse Agent::stream(std::vector<ContentPart> parts, StreamCallback cb) {
    // Same invariant as Agent::send — collapse single-text parts back to the
    // legacy content shape so we don't bloat text-only messages.
    Message user_msg;
    user_msg.role = "user";
    const bool is_single_text =
        parts.size() == 1 && parts.front().kind == ContentPart::TEXT;
    if (is_single_text) {
        user_msg.content = std::move(parts.front().text);
    } else {
        user_msg.content = concatenate_text(parts);
        user_msg.parts   = std::move(parts);
    }

    ApiRequest req;
    req.model         = config_.model;
    req.system_prompt = config_.build_system_prompt();
    req.max_tokens    = config_.max_tokens;
    req.temperature   = config_.temperature;
    {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto& hist = histories_[agent_conversation_key()];
        hist.push_back(std::move(user_msg));
        req.messages = hist;
    }

    auto resp = client_.stream(req, cb);

    if (resp.ok) {
        continue_until_done(resp, cb);
        std::lock_guard<std::mutex> lk(history_mu_);
        Message am{"assistant", resp.content};
        am.thinking = resp.reasoning;
        histories_[agent_conversation_key()].push_back(std::move(am));
        stats_.total_input_tokens  += resp.input_tokens;
        stats_.total_output_tokens += resp.output_tokens;
        stats_.total_requests++;
    }

    return resp;
}

void Agent::append_tool_trace(ToolTraceEntry entry) {
    std::lock_guard<std::mutex> lk(history_mu_);
    auto it = histories_.find(agent_conversation_key());
    if (it == histories_.end() || it->second.empty()) return;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (rit->role == "assistant") {
            rit->tool_trace.push_back(std::move(entry));
            return;
        }
    }
}

void Agent::reset_history() {
    std::lock_guard<std::mutex> lk(history_mu_);
    histories_[agent_conversation_key()].clear();
}

void Agent::reset_all_histories() {
    std::lock_guard<std::mutex> lk(history_mu_);
    histories_.clear();
}

void Agent::erase_conversation(const std::string& conversation_id) {
    std::lock_guard<std::mutex> lk(history_mu_);
    histories_.erase(conversation_id);
}

std::string Agent::status_summary() const {
    size_t msg_count;
    {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = histories_.find(agent_conversation_key());
        msg_count = (it == histories_.end()) ? 0 : it->second.size();
    }
    std::ostringstream ss;
    ss << id_ << " | " << config_.role
       << " | msgs:" << msg_count
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
    {
        std::lock_guard<std::mutex> lk(history_mu_);
        auto it = histories_.find(agent_conversation_key());
        if (it != histories_.end()) {
            for (auto& msg : it->second) {
                auto mo = jobj();
                mo->as_object_mut()["role"] = jstr(msg.role);
                mo->as_object_mut()["content"] = jstr(msg.content);
                if (!msg.thinking.empty()) {
                    mo->as_object_mut()["thinking"] = jstr(msg.thinking);
                }
                if (!msg.tool_trace.empty()) {
                    auto tarr = jarr();
                    for (const auto& t : msg.tool_trace) {
                        auto to = jobj();
                        auto& tm = to->as_object_mut();
                        tm["id"] = jstr(t.id);
                        tm["label"] = jstr(t.label);
                        tm["kind"] = jstr(t.kind);
                        tm["detail"] = jstr(t.detail);
                        tm["ok"] = jbool(t.ok);
                        tm["result_preview"] = jstr(t.result_preview);
                        tarr->as_array_mut().push_back(to);
                    }
                    mo->as_object_mut()["tool_trace"] = tarr;
                }
                hist->as_array_mut().push_back(mo);
            }
        }
    }
    m["history"] = hist;

    auto st = jobj();
    st->as_object_mut()["input_tokens"]  = jnum(static_cast<double>(stats_.total_input_tokens));
    st->as_object_mut()["output_tokens"] = jnum(static_cast<double>(stats_.total_output_tokens));
    st->as_object_mut()["requests"]      = jnum(static_cast<double>(stats_.total_requests));
    m["stats"] = st;

    return json_serialize(*obj);
}

} // namespace arbiter
