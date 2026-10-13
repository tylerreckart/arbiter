// src/decision.cpp — Closed-set decision interface and the Luna logprob backend.

#include "decision.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace arbiter {

namespace {

std::string clip_utf8(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::size_t keep = max_bytes;
    while (keep > 0 &&
           (static_cast<unsigned char>(text[keep]) & 0xC0) == 0x80) {
        --keep;
    }
    text.resize(keep);
    return text;
}

std::string canon_label(const std::string& label) {
    if (label.size() == 1) {
        unsigned char c = static_cast<unsigned char>(label[0]);
        if (c >= 'a' && c <= 'z') return std::string(1, static_cast<char>(c - 'a' + 'A'));
    }
    return label;
}

// Trimmed token that is exactly one ASCII letter or digit. Letters are
// uppercased so " A" and "a" both match the label "A".
std::string token_label(const std::string& token) {
    size_t b = 0;
    size_t e = token.size();
    while (b < e && std::isspace(static_cast<unsigned char>(token[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(token[e - 1]))) --e;
    if (e != b + 1) return {};
    return canon_label(token.substr(b, 1));
}

bool contains_ci(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return false;
    std::string h = hay;
    std::string n = needle;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

std::string fmt_num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string format_distribution(const DecisionResult& result) {
    std::ostringstream ss;
    for (size_t i = 0; i < result.distribution.size(); ++i) {
        if (i) ss << ',';
        ss << result.distribution[i].first << '='
           << fmt_num(result.distribution[i].second);
    }
    return ss.str();
}

void add_cost(DecisionResult& result, const ApiResponse& resp) {
    if (!resp.has_cost) return;
    if (!result.cost_known) result.cost_usd = 0;
    result.cost_known = true;
    result.cost_usd += resp.cost_usd;
}

} // namespace

DecisionResult decide(DecisionBackend& backend,
                      const std::string& state,
                      const std::string& question,
                      const std::vector<DecisionOption>& options) {
    DecisionRequest req;
    req.state = state;
    req.question = question;
    req.options = options;
    return backend.decide(req);
}

std::string single_token_label(std::size_t index) {
    static constexpr char kLabels[] = "ABCDEFGHIJKLMNOPQRST";
    if (index >= kMaxLunaOptions) return {};
    return std::string(1, kLabels[index]);
}

bool is_single_token_label(const std::string& label) {
    if (label.size() != 1) return false;
    unsigned char c = static_cast<unsigned char>(label[0]);
    return std::isalnum(c) != 0;
}

double decision_confidence(double p_max, std::size_t n) {
    if (n <= 1) return 1.0;
    if (!std::isfinite(p_max)) return 0;
    const double uniform = 1.0 / static_cast<double>(n);
    const double denom = 1.0 - uniform;
    if (!(denom > 0)) return 0;
    double c = (p_max - uniform) / denom;
    if (c < 0) return 0;
    if (c > 1) return 1;
    return c;
}

DecisionResult classify_logprobs(const std::vector<DecisionOption>& options,
                                 const std::vector<TokenLogprobs>& content) {
    DecisionResult result;
    if (options.size() < 2) {
        result.error = "need at least two options";
        return result;
    }
    if (content.empty()) {
        result.error = "response did not include logprobs";
        return result;
    }

    const TokenLogprobs& first = content.front();
    result.sampled_token = first.token;

    std::vector<double> best(options.size(), 0);
    std::vector<char> seen(options.size(), 0);

    auto consider = [&](const std::string& token, double logprob) {
        if (!std::isfinite(logprob)) return;
        std::string label = token_label(token);
        if (label.empty()) return;
        for (size_t i = 0; i < options.size(); ++i) {
            if (canon_label(options[i].label) != label) continue;
            if (!seen[i] || logprob > best[i]) {
                best[i] = logprob;
                seen[i] = 1;
            }
        }
    };

    if (first.has_logprob) consider(first.token, first.logprob);
    for (const auto& alt : first.top_logprobs) consider(alt.token, alt.logprob);

    int observed = 0;
    for (char hit : seen) if (hit) ++observed;
    result.labels_observed = observed;
    if (observed == 0) {
        result.error = "no option label in logprobs";
        if (!result.sampled_token.empty()) {
            result.error += " (sampled token: " + result.sampled_token + ")";
        }
        return result;
    }

    double m = 0;
    bool have_m = false;
    for (size_t i = 0; i < options.size(); ++i) {
        if (!seen[i]) continue;
        if (!have_m || best[i] > m) {
            m = best[i];
            have_m = true;
        }
    }
    double sum = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        if (!seen[i]) continue;
        sum += std::exp(best[i] - m);
    }
    if (!(sum > 0) || !std::isfinite(sum)) {
        result.error = "logprobs could not be normalized";
        return result;
    }

    result.distribution.reserve(options.size());
    size_t winner = 0;
    bool have_winner = false;
    for (size_t i = 0; i < options.size(); ++i) {
        double p = 0;
        if (seen[i]) p = std::exp(best[i] - m) / sum;
        result.distribution.emplace_back(options[i].id, p);
        if (!have_winner || p > result.distribution[winner].second) {
            winner = i;
            have_winner = true;
        }
    }
    result.choice = options[winner].id;
    result.choice_label = options[winner].label;
    result.probability = result.distribution[winner].second;
    result.confidence = decision_confidence(result.probability, options.size());
    result.ok = true;
    return result;
}

std::string luna_decision_system_prompt() {
    return "You classify a request by choosing one option. "
           "Reply with that option's single-character label and nothing else.";
}

std::string luna_decision_prompt(const DecisionRequest& request) {
    // Options and the question are the stable prefix (same roster, same
    // question) so a provider prompt cache can reuse them. The request
    // text is the suffix and is the only part that changes per turn.
    std::ostringstream ss;
    ss << "Options:\n";
    for (const auto& opt : request.options) {
        ss << opt.label << ". " << opt.id;
        if (!opt.description.empty()) {
            ss << " — " << clip_utf8(opt.description, 240);
        }
        ss << "\n";
    }
    ss << "\nQuestion:\n";
    if (request.question.empty()) ss << "Which option is best?";
    else ss << request.question;
    ss << "\n\nState:\n" << clip_utf8(request.state, 8192);
    ss << "\n\nReply with exactly one option label.";
    return ss.str();
}

LunaLogprobBackend::LunaLogprobBackend(ApiClient& client, std::string model)
    : client_(&client), model_(std::move(model)) {}

LunaLogprobBackend::LunaLogprobBackend(CompleteFn complete, std::string model)
    : complete_(std::move(complete)), model_(std::move(model)) {}

ApiResponse LunaLogprobBackend::complete(const ApiRequest& req) {
    if (complete_) return complete_(req);
    if (!client_) {
        ApiResponse r;
        r.ok = false;
        r.error = "no API client";
        return r;
    }
    return client_->complete(req);
}

DecisionResult LunaLogprobBackend::decide(const DecisionRequest& request) {
    DecisionResult result;
    result.backend = name();
    result.requested_model = model_;

    if (request.options.size() < 2) {
        result.error = "need at least two options";
        return result;
    }
    if (request.options.size() > kMaxLunaOptions) {
        result.error = "luna logprob backend accepts at most 20 options "
                       "(OpenAI top_logprobs cap)";
        return result;
    }
    std::vector<std::string> seen_labels;
    for (const auto& opt : request.options) {
        if (opt.id.empty() || !is_single_token_label(opt.label)) {
            result.error = "each option needs an id and a single-token label";
            return result;
        }
        std::string label = canon_label(opt.label);
        if (std::find(seen_labels.begin(), seen_labels.end(), label) != seen_labels.end()) {
            result.error = "duplicate option label " + opt.label;
            return result;
        }
        seen_labels.push_back(std::move(label));
    }

    ApiRequest req;
    req.model = model_;
    req.max_tokens = 1;
    req.include_temperature = false;
    req.logprobs = true;
    int top = top_logprobs_ > 0 ? top_logprobs_ : 20;
    if (top > 20) top = 20;
    if (top < 1) top = 1;
    req.top_logprobs = top;
    req.reasoning_effort = "none";
    req.system_prompt = luna_decision_system_prompt();
    Message msg;
    msg.role = "user";
    msg.content = luna_decision_prompt(request);
    req.messages.push_back(std::move(msg));

    const auto started = std::chrono::steady_clock::now();
    ApiResponse resp = complete(req);
    ++result.attempts;
    add_cost(result, resp);
    if (!resp.served_model.empty()) result.model = resp.served_model;

    // Some newer OpenAI routes reject top_logprobs greater than 1. Retry
    // once at 1 so a live smoke test can tell "no alternatives" apart from
    // "logprobs are unsupported".
    if (!resp.ok && req.top_logprobs > 1 && contains_ci(resp.error, "top_logprobs")) {
        result.note = "retried with top_logprobs=1 after: " + resp.error;
        req.top_logprobs = 1;
        resp = complete(req);
        ++result.attempts;
        add_cost(result, resp);
        if (!resp.served_model.empty()) result.model = resp.served_model;
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    result.latency_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    result.top_logprobs = req.top_logprobs;

    if (!resp.ok) {
        result.error = resp.error.empty() ? "request failed" : resp.error;
        result.ok = false;
        return result;
    }

    DecisionResult classified = classify_logprobs(request.options, resp.logprobs);
    classified.backend = result.backend;
    classified.requested_model = result.requested_model;
    classified.model = result.model;
    classified.cost_usd = result.cost_usd;
    classified.cost_known = result.cost_known;
    classified.latency_ms = result.latency_ms;
    classified.top_logprobs = result.top_logprobs;
    classified.attempts = result.attempts;
    classified.note = result.note;
    if (classified.labels_observed < static_cast<int>(request.options.size()) &&
        classified.ok) {
        std::string coverage = "observed " + std::to_string(classified.labels_observed) +
                               " of " + std::to_string(request.options.size()) +
                               " option labels in top_logprobs";
        if (!classified.note.empty()) classified.note += "; ";
        classified.note += coverage;
    }
    return classified;
}

std::vector<DecisionOption> options_from_roster(
    const std::vector<IntentRosterEntry>& roster) {
    std::vector<IntentRosterEntry> sorted = roster;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const IntentRosterEntry& a, const IntentRosterEntry& b) {
                         return a.id < b.id;
                     });
    std::vector<DecisionOption> out;
    out.reserve(sorted.size());
    for (const auto& entry : sorted) {
        if (entry.id.empty() || entry.id == "index") continue;
        DecisionOption opt;
        opt.id = entry.id;
        opt.label = single_token_label(out.size());
        opt.description = entry.role;
        if (!entry.goal.empty()) {
            if (!opt.description.empty()) opt.description += ". ";
            opt.description += entry.goal;
        }
        opt.description = clip_utf8(std::move(opt.description), 240);
        out.push_back(std::move(opt));
    }
    return out;
}

bool intent_shadow_luna_enabled() {
    const char* v = std::getenv("ARBITER_INTENT_SHADOW");
    if (!v || !*v) return false;
    std::string s(v);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "luna" || s == "1" || s == "true" || s == "on";
}

void shadow_intent_decision(ApiClient& client,
                            const std::string& text,
                            const std::vector<IntentRosterEntry>& roster,
                            const Intent& heuristic) {
    if (!intent_shadow_luna_enabled()) return;
    try {
        if (text.empty()) {
            Logger::global().info("intent_shadow", {
                {"backend", "luna-logprob"},
                {"skipped", "empty_text"},
            });
            return;
        }
        auto options = options_from_roster(roster);
        if (options.size() < 2 || options.size() > kMaxLunaOptions) {
            Logger::global().info("intent_shadow", {
                {"backend", "luna-logprob"},
                {"skipped", options.size() < 2 ? "roster" : "too_many_options"},
                {"options", std::to_string(options.size())},
                {"heuristic_kind", heuristic.kind},
                {"heuristic_agent", heuristic.target_agent},
            });
            return;
        }
        std::string model = kLunaModelId;
        if (const char* m = std::getenv("ARBITER_INTENT_SHADOW_MODEL"); m && *m) {
            model = m;
        }
        LunaLogprobBackend backend(client, model);
        DecisionRequest req;
        req.state = text;
        req.question = "Which specialist should handle this request?";
        req.options = std::move(options);
        DecisionResult result = backend.decide(req);
        Logger::global().info("intent_shadow", {
            {"backend", "luna-logprob"},
            {"ok", result.ok ? "true" : "false"},
            {"choice", result.choice},
            {"choice_label", result.choice_label},
            {"probability", fmt_num(result.probability)},
            {"confidence", fmt_num(result.confidence)},
            {"latency_ms", fmt_num(result.latency_ms)},
            {"cost_usd", result.cost_known ? fmt_num(result.cost_usd) : "unknown"},
            {"model", result.model},
            {"labels_observed", std::to_string(result.labels_observed)},
            {"top_logprobs", std::to_string(result.top_logprobs)},
            {"attempts", std::to_string(result.attempts)},
            {"distribution", format_distribution(result)},
            {"heuristic_kind", heuristic.kind},
            {"heuristic_source", heuristic.source},
            {"heuristic_agent", heuristic.target_agent},
            {"heuristic_confidence", fmt_num(heuristic.confidence)},
            {"error", result.error},
            {"note", result.note},
        });
    } catch (const std::exception& e) {
        Logger::global().info("intent_shadow", {
            {"backend", "luna-logprob"},
            {"ok", "false"},
            {"error", e.what()},
        });
    } catch (...) {
        Logger::global().info("intent_shadow", {
            {"backend", "luna-logprob"},
            {"ok", "false"},
            {"error", "unknown"},
        });
    }
}

} // namespace arbiter
