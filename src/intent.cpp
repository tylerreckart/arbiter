// src/intent.cpp — Pre-dispatch intent classifier (heuristic + optional LLM).
//
// Kept free of ApiClient so unit_intent can pin the parser and cue matcher
// without the provider stack.  Callers that want an LLM wrap complete() in
// an IntentLlmFn.

#include "intent.h"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace arbiter {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_copy(std::string s) {
    size_t i = 0, j = s.size();
    while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

std::string extract_tag(const std::string& s, const std::string& tag) {
    std::string open  = "<"  + tag + ">";
    std::string close = "</" + tag + ">";
    auto a = s.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    auto b = s.find(close, a);
    if (b == std::string::npos) return {};
    return trim_copy(s.substr(a, b - a));
}

std::string normalize_query(const std::string& q) {
    std::string norm;
    norm.reserve(q.size() + 2);
    norm.push_back(' ');
    for (char c : q) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc))
            norm.push_back(static_cast<char>(std::tolower(uc)));
        else
            norm.push_back(' ');
    }
    norm.push_back(' ');
    return norm;
}

bool has_cue(const std::string& norm, const char* needle) {
    return norm.find(needle) != std::string::npos;
}

bool roster_has(const std::vector<IntentRosterEntry>& roster,
                const std::string& id) {
    for (const auto& e : roster) {
        if (e.id == id) return true;
    }
    return false;
}

// Preferred starter id for a kind; empty if kind has no default agent.
const char* preferred_agent(const std::string& kind) {
    if (kind == "research")  return "research";
    if (kind == "review")    return "reviewer";
    if (kind == "write")     return "writer";
    if (kind == "ops")       return "devops";
    if (kind == "frontend")  return "frontend";
    if (kind == "backend")   return "backend";
    if (kind == "plan")      return "planner";
    if (kind == "market")    return "marketer";
    if (kind == "social")    return "social";
    if (kind == "multi")     return "planner";
    return "";
}

bool role_matches_kind(const std::string& role_lc, const std::string& kind) {
    if (kind == "research")  return role_lc.find("research") != std::string::npos;
    if (kind == "review")    return role_lc.find("review") != std::string::npos;
    if (kind == "write")
        return role_lc.find("writer") != std::string::npos ||
               role_lc.find("writing") != std::string::npos ||
               role_lc.find("content") != std::string::npos;
    if (kind == "ops")
        return role_lc.find("devops") != std::string::npos ||
               role_lc.find("infrastructure") != std::string::npos ||
               role_lc.find("ops") != std::string::npos;
    if (kind == "frontend")
        return role_lc.find("frontend") != std::string::npos ||
               role_lc.find("front-end") != std::string::npos;
    if (kind == "backend")
        return role_lc.find("backend") != std::string::npos ||
               role_lc.find("back-end") != std::string::npos;
    if (kind == "plan")
        return role_lc.find("planner") != std::string::npos ||
               role_lc.find("planning") != std::string::npos;
    if (kind == "market")    return role_lc.find("market") != std::string::npos;
    if (kind == "social")    return role_lc.find("social") != std::string::npos;
    if (kind == "multi")
        return role_lc.find("planner") != std::string::npos ||
               role_lc.find("planning") != std::string::npos;
    return false;
}

std::string match_roster_agent(const std::vector<IntentRosterEntry>& roster,
                               const std::string& kind) {
    const char* pref = preferred_agent(kind);
    if (pref && pref[0] && roster_has(roster, pref)) return pref;

    std::string found;
    for (const auto& e : roster) {
        if (e.id == "index") continue;
        std::string role_lc = to_lower(e.role);
        std::string id_lc   = to_lower(e.id);
        if (id_lc == kind || role_matches_kind(role_lc, kind) ||
            (pref && pref[0] && id_lc == pref)) {
            if (!found.empty() && found != e.id) return {};  // ambiguous
            found = e.id;
        }
    }
    return found;
}

std::string default_brief(const std::string& kind, const std::string& agent) {
    if (kind == "unknown" || kind.empty()) return {};
    std::ostringstream ss;
    if (!agent.empty())
        ss << "Route to " << agent << " (" << kind << ").";
    else
        ss << "Classified as " << kind << ".";
    return ss.str();
}

std::vector<std::string> cue_kinds(const std::string& norm) {
    std::vector<std::string> kinds;
    auto push = [&](const std::string& k) {
        for (const auto& x : kinds) if (x == k) return;
        kinds.push_back(k);
    };

    if (has_cue(norm, " research ") || has_cue(norm, " look up ") ||
        has_cue(norm, " lookup ") || has_cue(norm, " primary source ") ||
        has_cue(norm, " primary sources ") || has_cue(norm, " competitive analysis ") ||
        has_cue(norm, " literature ") || has_cue(norm, " survey of ") ||
        has_cue(norm, " fact check ") ||
        has_cue(norm, " citation ") ||
        has_cue(norm, " citations ") || has_cue(norm, " sources for ") ||
        has_cue(norm, " gather facts ") || has_cue(norm, " find sources ")) {
        push("research");
    }

    if (has_cue(norm, " code review ") || has_cue(norm, " review this pr ") ||
        has_cue(norm, " review the pr ") || has_cue(norm, " pull request ") ||
        has_cue(norm, " pr feedback ") || has_cue(norm, " review this diff ") ||
        has_cue(norm, " review the diff ") || has_cue(norm, " find defects ") ||
        has_cue(norm, " code review this ")) {
        push("review");
    }

    if (has_cue(norm, " write an essay ") || has_cue(norm, " write a readme ") ||
        has_cue(norm, " write the readme ") || has_cue(norm, " draft a ") ||
        has_cue(norm, " write documentation ") || has_cue(norm, " write a blog ") ||
        has_cue(norm, " write a prd ") || has_cue(norm, " write the docs ") ||
        has_cue(norm, " polish this prose ") || has_cue(norm, " creative writing ")) {
        push("write");
    }

    if (has_cue(norm, " docker ") || has_cue(norm, " kubernetes ") ||
        has_cue(norm, " k8s ") || has_cue(norm, " ci cd ") ||
        has_cue(norm, " terraform ") ||
        has_cue(norm, " systemd ") || has_cue(norm, " kubectl ") ||
        has_cue(norm, " deploy to ") || has_cue(norm, " incident response ") ||
        has_cue(norm, " infrastructure ") || has_cue(norm, " dockerfile ")) {
        push("ops");
    }

    if (has_cue(norm, " react component ") || has_cue(norm, " react app ") ||
        has_cue(norm, " react native ") || has_cue(norm, " typescript app ") ||
        has_cue(norm, " typescript component ") || has_cue(norm, " css layout ") ||
        has_cue(norm, " stylesheet ") || has_cue(norm, " accessibility ") ||
        has_cue(norm, " next js ") || has_cue(norm, " nextjs ") ||
        has_cue(norm, " tailwind ") || has_cue(norm, " frontend ") ||
        has_cue(norm, " front end ") || has_cue(norm, " component tree ")) {
        push("frontend");
    }

    if (has_cue(norm, " postgres ") || has_cue(norm, " postgresql ") ||
        has_cue(norm, " migration ") || has_cue(norm, " db schema ") ||
        has_cue(norm, " database schema ") || has_cue(norm, " sql query ") ||
        has_cue(norm, " sql schema ") ||
        has_cue(norm, " distributed systems ") || has_cue(norm, " backend api ") ||
        has_cue(norm, " rest api ") || has_cue(norm, " grpc ")) {
        push("backend");
    }

    if (has_cue(norm, " marketing ") || has_cue(norm, " positioning ") ||
        has_cue(norm, " go to market ") ||
        has_cue(norm, " marketing campaign ") || has_cue(norm, " campaign brief ") ||
        has_cue(norm, " messaging ") ||
        has_cue(norm, " acquisition ")) {
        push("market");
    }

    if (has_cue(norm, " twitter ") || has_cue(norm, " write a tweet ") ||
        has_cue(norm, " twitter thread ") ||
        has_cue(norm, " linkedin ") || has_cue(norm, " instagram ") ||
        has_cue(norm, " tiktok ") || has_cue(norm, " social media ") ||
        has_cue(norm, " thread about ")) {
        push("social");
    }

    if (has_cue(norm, " multi step ") ||
        has_cue(norm, " decompose ") || has_cue(norm, " break this down ") ||
        has_cue(norm, " break it down ") || has_cue(norm, " make a plan ") ||
        has_cue(norm, " write a plan ") || has_cue(norm, " roadmap for ") ||
        has_cue(norm, " step by step plan ") || has_cue(norm, " phases for ")) {
        push("plan");
    }

    return kinds;
}

std::string build_llm_user_prompt(const IntentInput& in, const Intent& hint) {
    std::ostringstream q;
    q << "[REQUEST]\n" << in.text << "\n[END REQUEST]\n\n";
    q << "[ROSTER]\n";
    if (in.roster.empty()) {
        q << "(none)\n";
    } else {
        for (const auto& e : in.roster) {
            if (e.id == "index") continue;
            q << "- " << e.id;
            if (!e.role.empty()) q << " [" << e.role << "]";
            if (!e.goal.empty()) q << " — " << e.goal;
            q << "\n";
        }
    }
    q << "[END ROSTER]\n";
    if (!hint.kind.empty() && hint.kind != "unknown") {
        q << "\n[HEURISTIC HINT] kind=" << hint.kind
          << " conf=" << hint.confidence;
        if (!hint.target_agent.empty())
            q << " agent=" << hint.target_agent;
        q << "\n";
    }
    return q.str();
}

Intent drop_unknown_agent(Intent out, const std::vector<IntentRosterEntry>& roster) {
    if (out.target_agent.empty()) return out;
    if (out.target_agent == "index" || !roster_has(roster, out.target_agent)) {
        out.target_agent.clear();
    }
    return out;
}

}  // namespace

const char* default_intent_prompt() {
    return
        "You classify a user or event request for a multi-agent runtime.\n"
        "Reply with EXACTLY this tagged form (no preamble):\n\n"
        "<intent>\n"
        "<kind>...</kind>\n"
        "<confidence>0.00-1.00</confidence>\n"
        "<agent>agent-id-or-empty</agent>\n"
        "<brief>one-line enriched brief</brief>\n"
        "<todo>optional todo title</todo>\n"
        "<phase agent=\"id\" name=\"name\">optional phase task</phase>\n"
        "</intent>\n\n"
        "kind must be one of: research, review, write, ops, frontend, backend, "
        "plan, market, social, multi, unknown.\n"
        "<agent> must be an id from the roster, or empty if the master (index) "
        "should handle it. Do not invent agent ids.\n"
        "Use kind=multi and agent=planner (if present) when the work needs "
        "decomposition. Emit <todo> / <phase> only for multi or plan; they are "
        "seeds, not orders to execute.\n"
        "Default to unknown + empty agent when unsure.";
}

bool intent_kind_is_valid(const std::string& kind) {
    return kind == "research" || kind == "review" || kind == "write" ||
           kind == "ops" || kind == "frontend" || kind == "backend" ||
           kind == "plan" || kind == "market" || kind == "social" ||
           kind == "multi" || kind == "unknown";
}

Intent parse_intent_signal(const std::string& reply) {
    Intent out;
    out.source = "llm";
    out.llm_used = true;

    std::string body = reply;
    auto inner = extract_tag(reply, "intent");
    if (!inner.empty()) body = inner;

    out.kind = to_lower(extract_tag(body, "kind"));
    if (!intent_kind_is_valid(out.kind)) {
        out.kind = "unknown";
        out.malformed = true;
    }

    auto conf_s = extract_tag(body, "confidence");
    if (!conf_s.empty()) {
        try {
            out.confidence = std::stod(conf_s);
        } catch (...) {
            out.malformed = true;
            out.confidence = 0;
        }
        if (out.confidence < 0.0) out.confidence = 0.0;
        if (out.confidence > 1.0) out.confidence = 1.0;
    } else {
        out.malformed = true;
    }

    out.target_agent = trim_copy(extract_tag(body, "agent"));
    if (out.target_agent == "index") out.target_agent.clear();
    out.brief = extract_tag(body, "brief");

    // Repeated <todo> tags.
    {
        constexpr std::size_t kMaxIntentSeedTodos = 32;
        std::string hay = body;
        const std::string open = "<todo>";
        const std::string close = "</todo>";
        size_t pos = 0;
        while (out.todo_seeds.size() < kMaxIntentSeedTodos) {
            auto a = hay.find(open, pos);
            if (a == std::string::npos) break;
            a += open.size();
            auto b = hay.find(close, a);
            if (b == std::string::npos) break;
            IntentSeedTodo t;
            t.title = trim_copy(hay.substr(a, b - a));
            if (!t.title.empty()) out.todo_seeds.push_back(std::move(t));
            pos = b + close.size();
        }
    }

    // <phase agent="..." name="...">task</phase>
    {
        constexpr std::size_t kMaxIntentSeedPhases = 16;
        std::string hay = body;
        const std::string open = "<phase";
        const std::string close = "</phase>";
        size_t pos = 0;
        while (out.plan_seeds.size() < kMaxIntentSeedPhases) {
            auto a = hay.find(open, pos);
            if (a == std::string::npos) break;
            auto tag_end = hay.find('>', a);
            if (tag_end == std::string::npos) break;
            std::string attrs = hay.substr(a + open.size(), tag_end - (a + open.size()));
            auto b = hay.find(close, tag_end);
            if (b == std::string::npos) break;

            IntentSeedPhase ph;
            ph.task = trim_copy(hay.substr(tag_end + 1, b - (tag_end + 1)));
            auto grab_attr = [&](const char* key) -> std::string {
                std::string pat = std::string(key) + "=\"";
                auto i = attrs.find(pat);
                if (i == std::string::npos) {
                    pat = std::string(key) + "='";
                    i = attrs.find(pat);
                    if (i == std::string::npos) return {};
                    i += pat.size();
                    auto j = attrs.find('\'', i);
                    if (j == std::string::npos) return {};
                    return attrs.substr(i, j - i);
                }
                i += pat.size();
                auto j = attrs.find('"', i);
                if (j == std::string::npos) return {};
                return attrs.substr(i, j - i);
            };
            ph.agent = grab_attr("agent");
            ph.name  = grab_attr("name");
            if (!ph.task.empty() || !ph.name.empty())
                out.plan_seeds.push_back(std::move(ph));
            pos = b + close.size();
        }
    }

    return out;
}

Intent heuristic_classify(const IntentInput& in) {
    Intent out;
    out.source = "heuristic";
    out.kind = "unknown";

    if (in.text.empty()) return out;

    auto kinds = cue_kinds(normalize_query(in.text));
    if (kinds.empty()) return out;

    if (kinds.size() == 1) {
        out.kind = kinds[0];
        out.target_agent = match_roster_agent(in.roster, out.kind);
        out.confidence = out.target_agent.empty() ? 0.55 : 0.9;
        out.brief = default_brief(out.kind, out.target_agent);
        return out;
    }

    // Multiple cue families.  Same target → keep that kind; else multi.
    std::string shared;
    bool same = true;
    for (const auto& k : kinds) {
        auto t = match_roster_agent(in.roster, k);
        if (t.empty()) { same = false; continue; }
        if (shared.empty()) shared = t;
        else if (shared != t) same = false;
    }
    if (same && !shared.empty()) {
        out.kind = kinds[0];
        out.target_agent = shared;
        out.confidence = 0.85;
        out.brief = default_brief(out.kind, out.target_agent);
        return out;
    }

    out.kind = "multi";
    out.target_agent = match_roster_agent(in.roster, "multi");
    out.confidence = out.target_agent.empty() ? 0.5 : 0.55;
    out.brief = default_brief(out.kind, out.target_agent);
    return out;
}

bool intent_should_apply(const IntentConfig& cfg,
                         const Intent& intent,
                         const std::string& requested_agent,
                         bool fresh_ingress) {
    if (!fresh_ingress) return false;
    if (!cfg.apply_routing) return false;
    if (cfg.mode == "off" || cfg.mode.empty()) return false;
    std::string req = requested_agent.empty() ? "index" : requested_agent;
    if (req != "index") return false;
    if (intent.target_agent.empty() || intent.target_agent == "index") return false;
    if (intent.confidence + 1e-9 < cfg.min_confidence) return false;
    return true;
}

Intent resolve_intent(const IntentInput& in,
                      const IntentConfig& cfg,
                      const IntentLlmFn& llm) {
    Intent out;
    out.kind = "unknown";
    out.source = "none";

    std::string mode = cfg.mode;
    if (mode != "off" && mode != "heuristic" && mode != "hybrid" && mode != "llm")
        mode = "off";

    const bool explicit_specialist =
        !in.requested_agent.empty() && in.requested_agent != "index";

    if (mode == "off") {
        if (explicit_specialist) {
            out.source = "explicit";
            out.target_agent = in.requested_agent;
        }
        return out;
    }

    if (explicit_specialist) {
        out = heuristic_classify(in);
        out.source = "explicit";
        out.target_agent = in.requested_agent;
        if (out.kind == "unknown") out.confidence = 0;
        return out;
    }

    Intent hint = heuristic_classify(in);
    const bool heuristic_confident =
        !hint.target_agent.empty() &&
        hint.confidence + 1e-9 >= cfg.min_confidence;

    if (mode == "heuristic" || (mode == "hybrid" && heuristic_confident)) {
        out = hint;
        if (in.source_hint == "event") out.source = "event";
        return drop_unknown_agent(std::move(out), in.roster);
    }

    if ((mode == "hybrid" || mode == "llm") && llm) {
        std::string reply = llm(build_llm_user_prompt(in, hint));
        if (reply.empty()) {
            // Fail open: keep heuristic (possibly unconfident) rather than
            // inventing a route.
            out = hint;
            out.llm_used = true;
            if (in.source_hint == "event") out.source = "event";
            return drop_unknown_agent(std::move(out), in.roster);
        }
        out = parse_intent_signal(reply);
        out = drop_unknown_agent(std::move(out), in.roster);
        if (out.malformed) {
            // Fail open: do not trust a malformed target.
            out.target_agent.clear();
            if (out.kind != "unknown" && !intent_kind_is_valid(out.kind))
                out.kind = "unknown";
        }
        if (in.source_hint == "event") out.source = "event";
        if (out.brief.empty())
            out.brief = default_brief(out.kind, out.target_agent);
        return out;
    }

    // hybrid/llm with no llm fn, or heuristic-only miss.
    out = hint;
    if (in.source_hint == "event") out.source = "event";
    return drop_unknown_agent(std::move(out), in.roster);
}

std::string format_intent_preamble(const Intent& intent, bool for_specialist) {
    if (intent.source == "none" || intent.source.empty()) return {};
    if (intent.kind.empty() || (intent.kind == "unknown" && intent.source != "llm" &&
                                intent.source != "explicit")) {
        if (intent.source != "heuristic" && intent.source != "event") return {};
        if (intent.kind == "unknown" && intent.confidence <= 0.0 &&
            intent.target_agent.empty())
            return {};
    }

    std::ostringstream ss;
    ss << "[INTENT] kind=" << (intent.kind.empty() ? "unknown" : intent.kind);
    ss << " conf=" << std::fixed << std::setprecision(2) << intent.confidence
       << " source=" << intent.source;
    if (!intent.target_agent.empty())
        ss << " agent=" << intent.target_agent;

    if (for_specialist && !intent.brief.empty())
        ss << "\nGOAL: " << intent.brief;
    return ss.str();
}

} // namespace arbiter
