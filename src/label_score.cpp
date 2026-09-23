// src/label_score.cpp — Closed-set label distribution. No provider I/O.

#include "label_score.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace arbiter {

namespace {

bool is_ascii_letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

char upper_ascii(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    return c;
}

// Trimmed token that is exactly one letter, uppercased. Empty otherwise.
std::string letter_token(const std::string& token) {
    size_t b = 0;
    size_t e = token.size();
    while (b < e && std::isspace(static_cast<unsigned char>(token[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(token[e - 1]))) --e;
    if (e != b + 1) return {};
    if (!is_ascii_letter(token[b])) return {};
    return std::string(1, upper_ascii(token[b]));
}

bool ollama_model(const std::string& model) {
    return model.rfind("ollama/", 0) == 0 && model.size() > std::string("ollama/").size();
}

double parse_margin(const char* s) {
    if (!s || !*s) return 0;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s) return 0;
    if (!std::isfinite(v) || v <= 0) return 0;
    if (v > 1) return 1;
    return v;
}

DecisionFilterConfig config_from(const char* model,
                                 const char* presence_margin,
                                 const char* intent_margin) {
    DecisionFilterConfig c;
    if (model && *model) c.model = model;
    c.presence_silence_margin = parse_margin(presence_margin);
    c.intent_route_margin = parse_margin(intent_margin);
    return c;
}

}  // namespace

LabelDistribution distribution_from_logprobs(
    const std::vector<LabelSpec>& specs,
    const std::vector<TokenLogprob>& tokens) {
    LabelDistribution out;
    if (specs.size() < 2) return out;

    std::vector<char> letters;
    letters.reserve(specs.size());
    for (const auto& spec : specs) {
        if (!is_ascii_letter(spec.letter) || spec.name.empty()) return out;
        char letter = upper_ascii(spec.letter);
        if (std::find(letters.begin(), letters.end(), letter) != letters.end())
            return out;
        letters.push_back(letter);
    }

    std::vector<double> best(specs.size(), 0);
    std::vector<bool> seen(specs.size(), false);
    for (const auto& tok : tokens) {
        std::string letter = letter_token(tok.token);
        if (letter.size() != 1) continue;
        if (!std::isfinite(tok.logprob)) continue;
        auto it = std::find(letters.begin(), letters.end(), letter[0]);
        if (it == letters.end()) continue;
        size_t i = static_cast<size_t>(it - letters.begin());
        if (!seen[i] || tok.logprob > best[i]) {
            best[i] = tok.logprob;
            seen[i] = true;
        }
    }
    for (bool hit : seen) {
        if (!hit) return out;
    }

    double m = best[0];
    for (double lp : best) m = std::max(m, lp);
    double sum = 0;
    for (double lp : best) sum += std::exp(lp - m);
    if (!(sum > 0) || !std::isfinite(sum)) return out;

    out.probs.reserve(specs.size());
    size_t winner = 0;
    double second = -1;
    for (size_t i = 0; i < specs.size(); ++i) {
        double p = std::exp(best[i] - m) / sum;
        out.probs.emplace_back(specs[i].name, p);
        if (i == 0 || p > out.probs[winner].second) winner = i;
    }
    out.peak = out.probs[winner].second;
    for (size_t i = 0; i < out.probs.size(); ++i) {
        if (i == winner) continue;
        second = std::max(second, out.probs[i].second);
    }
    out.margin = (second < 0) ? out.peak : (out.peak - second);
    if (out.margin < 0) out.margin = 0;
    out.argmax = specs[winner].name;
    out.ok = true;
    return out;
}

LabelDistribution distribution_from_openai_body(
    const std::vector<LabelSpec>& specs,
    const std::string& body) {
    try {
        auto root = json_parse(body);
        auto choices = root->get("choices");
        if (!choices || !choices->is_array() || choices->as_array().empty())
            return {};
        auto ch0 = choices->as_array().front();
        if (!ch0) return {};
        auto logprobs = ch0->get("logprobs");
        if (!logprobs || !logprobs->is_object()) return {};
        auto content = logprobs->get("content");
        if (!content || !content->is_array() || content->as_array().empty())
            return {};
        auto first = content->as_array().front();
        if (!first) return {};
        auto top = first->get("top_logprobs");
        if (!top || !top->is_array()) return {};

        std::vector<TokenLogprob> tokens;
        tokens.reserve(top->as_array().size());
        for (const auto& entry : top->as_array()) {
            if (!entry || !entry->is_object()) continue;
            TokenLogprob tp;
            tp.token = entry->get_string("token");
            auto lp = entry->get("logprob");
            if (!lp || !lp->is_number()) continue;
            tp.logprob = lp->as_number();
            tokens.push_back(std::move(tp));
        }
        return distribution_from_logprobs(specs, tokens);
    } catch (...) {
        return {};
    }
}

bool label_distribution_is_extreme(const LabelDistribution& d,
                                   double margin_floor) {
    if (!d.ok) return false;
    if (!(margin_floor > 0.0)) return false;
    if (d.peak + 1e-12 < kDecisionPeakFloor) return false;
    if (d.margin + 1e-12 < margin_floor) return false;
    if (d.argmax.empty()) return false;
    return true;
}

const char* label_score_system_prompt() {
    return "Choose one option. Reply with that option's letter and nothing else.";
}

std::string label_score_user_prompt(const std::string& state,
                                    const std::vector<LabelSpec>& specs) {
    std::ostringstream ss;
    ss << state << "\n\n";
    for (const auto& spec : specs) {
        ss << upper_ascii(spec.letter) << ". " << spec.name << " — "
           << spec.description << "\n";
    }
    return ss.str();
}

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

bool DecisionFilterConfig::enabled_for_presence() const {
    return ollama_model(model) && presence_silence_margin > 0.0;
}

bool DecisionFilterConfig::enabled_for_intent() const {
    return ollama_model(model) && intent_route_margin > 0.0;
}

DecisionFilterConfig decision_filter_from_env() {
    return config_from(std::getenv("ARBITER_DECISION_MODEL"),
                       std::getenv("ARBITER_PRESENCE_SILENCE_MARGIN"),
                       std::getenv("ARBITER_INTENT_ROUTE_MARGIN"));
}

DecisionFilterConfig decision_filter_from_values(const char* model,
                                                 const char* presence_margin,
                                                 const char* intent_margin) {
    return config_from(model, presence_margin, intent_margin);
}

std::vector<LabelSpec> intent_decision_labels() {
    return {
        {'R', "research",
         "The user wants sources, facts, or a survey."},
        {'V', "review",
         "The user wants a critique of code or a diff."},
        {'W', "write",
         "The user wants prose, docs, or a draft."},
        {'O', "ops",
         "The user wants infrastructure, deploy, or runtime work."},
        {'F', "frontend",
         "The user wants UI or client-side code."},
        {'B', "backend",
         "The user wants server, API, or data-store work."},
        {'P', "plan",
         "The user wants the work decomposed into steps."},
        {'M', "market",
         "The user wants positioning or a campaign."},
        {'S', "social",
         "The user wants a social post or audience copy."},
        {'U', "unknown",
         "None of the other labels fit, or the request is too unclear to route."},
    };
}

}  // namespace arbiter
