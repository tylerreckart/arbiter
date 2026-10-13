// arbiter-decision-smoke — manual live check of the Luna logprob backend.
//
// Classifies a few sample requests against the agent JSON files in agents/
// (or the directory given as argv[1]). Prints choice, distribution,
// confidence, latency, and cost, then nearest-rank p50 and p95 over the
// samples that were actually sent. Does not invent results.
//
//   OPENROUTER_API_KEY=sk-or-... ./arbiter-decision-smoke agents
//
// Exit 0: every sample returned a logprob distribution.
// Exit 1: a key was present but a call failed (including "logprobs not
//         supported"). Remaining samples are skipped when the failure is
//         clearly "logprobs are unsupported", so a rejected parameter is
//         not billed four times.
// Exit 2: no OpenRouter key. Nothing was sent.

#include "decision.h"
#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string load_openrouter_key() {
    if (const char* env = std::getenv("OPENROUTER_API_KEY"); env && *env) {
        return trim(env);
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return trim(read_file(fs::path(home) / ".arbiter" / "openrouter_api_key"));
}

struct Sample {
    const char* name;
    const char* text;
};

constexpr Sample kSamples[] = {
    {"research",
     "Look up primary sources on the RISC-V memory model and cite them."},
    {"review",
     "Please review this pull request for bugs before we merge."},
    {"ops",
     "Write a Dockerfile and a kubernetes deployment for the API server."},
    {"write",
     "Draft a short launch announcement for the new public API."},
};

bool logprobs_unsupported(const std::string& error) {
    std::string e = error;
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool mentions = e.find("logprob") != std::string::npos;
    const bool rejected = e.find("not supported") != std::string::npos ||
                          e.find("unsupported") != std::string::npos ||
                          e.find("invalid_request") != std::string::npos;
    return mentions && rejected;
}

void print_result(const Sample& sample, const arbiter::DecisionResult& result) {
    std::cout << "--- " << sample.name << " ---\n";
    std::cout << "state: " << sample.text << "\n";
    std::cout << "ok: " << (result.ok ? "true" : "false") << "\n";
    std::cout << "choice: " << (result.choice.empty() ? "-" : result.choice);
    if (!result.choice_label.empty()) std::cout << " (" << result.choice_label << ")";
    std::cout << "\n";
    std::cout << "probability: " << result.probability << "\n";
    std::cout << "confidence: " << result.confidence << "\n";
    std::cout << "latency_ms: " << result.latency_ms << "\n";
    std::cout << "cost_usd: ";
    if (result.cost_known) std::cout << result.cost_usd << "\n";
    else std::cout << "(not returned)\n";
    std::cout << "requested_model: " << result.requested_model << "\n";
    std::cout << "served_model: " << (result.model.empty() ? "-" : result.model) << "\n";
    std::cout << "sampled_token: " << result.sampled_token << "\n";
    std::cout << "labels_observed: " << result.labels_observed << "\n";
    std::cout << "top_logprobs: " << result.top_logprobs << "\n";
    std::cout << "attempts: " << result.attempts << "\n";
    if (!result.note.empty()) std::cout << "note: " << result.note << "\n";
    if (!result.error.empty()) std::cout << "error: " << result.error << "\n";
    std::cout << "distribution:\n";
    if (result.distribution.empty()) {
        std::cout << "  (none)\n";
    } else {
        for (const auto& [id, p] : result.distribution) {
            std::cout << "  " << id << " ";
            if (p) std::cout << *p << "\n";
            else std::cout << "unknown\n";
        }
    }
    std::cout << "\n";
}

// Nearest-rank percentile. rank = ceil(p * n), clamped to 1..n.
double percentile_nearest_rank(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const double n = static_cast<double>(values.size());
    double rank = std::ceil(p * n);
    if (rank < 1.0) rank = 1.0;
    if (rank > n) rank = n;
    return values[static_cast<std::size_t>(rank) - 1];
}

std::vector<arbiter::IntentRosterEntry> load_agents(const fs::path& dir) {
    std::vector<arbiter::IntentRosterEntry> roster;
    if (!fs::is_directory(dir)) return roster;
    for (const auto& ent : fs::directory_iterator(dir)) {
        if (!ent.is_regular_file()) continue;
        if (ent.path().extension() != ".json") continue;
        std::string body = read_file(ent.path());
        if (body.empty()) continue;
        try {
            auto root = arbiter::json_parse(body);
            arbiter::IntentRosterEntry entry;
            entry.id = ent.path().stem().string();
            entry.role = root->get_string("role");
            entry.goal = root->get_string("goal");
            if (entry.id.empty()) continue;
            roster.push_back(std::move(entry));
        } catch (const std::exception& e) {
            std::cerr << "skip " << ent.path().string() << ": " << e.what() << "\n";
        }
    }
    return roster;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path agents_dir = (argc > 1) ? fs::path(argv[1]) : fs::path("agents");

    std::cout << "Luna logprob smoke test\n";
    std::cout << "requested model: " << arbiter::kLunaModelId << "\n";
    std::cout << "reasoning: effort=none\n";
    std::cout << "max output tokens: 1\n";
    std::cout << "OpenRouter catalogue (2026-10-13) does not list logprobs or\n";
    std::cout << "top_logprobs for this model. This command sends them anyway.\n";
    std::cout << "A provider error is printed as returned. Distributions are\n";
    std::cout << "computed only from logprobs the response actually contains.\n\n";

    std::string key = load_openrouter_key();
    if (key.empty()) {
        std::cout << "OPENROUTER_API_KEY is not set and ~/.arbiter/openrouter_api_key\n";
        std::cout << "is missing or empty. No request was sent. This output is not\n";
        std::cout << "a model result.\n";
        std::cout << "Re-run with a key:\n";
        std::cout << "  OPENROUTER_API_KEY=sk-or-... arbiter-decision-smoke "
                  << agents_dir.string() << "\n";
        return 2;
    }
    key.clear();

    if (!fs::is_directory(agents_dir)) {
        std::cerr << "agents directory not found: " << agents_dir.string() << "\n";
        return 1;
    }
    auto roster = load_agents(agents_dir);
    auto options = arbiter::options_from_roster(roster);
    std::cout << "agents: " << options.size() << " from " << agents_dir.string() << "\n";
    for (const auto& opt : options) {
        std::cout << "  " << opt.label << ". " << opt.id << "\n";
    }
    std::cout << "\n";
    if (options.size() < 2) {
        std::cerr << "need at least two agent definitions\n";
        return 1;
    }
    if (options.size() > arbiter::kMaxLunaOptions) {
        std::cerr << "more than " << arbiter::kMaxLunaOptions
                  << " agents; the logprob backend refuses a wider set\n";
        return 1;
    }

    // Reload the key for the client. The earlier copy was cleared so a
    // failure path above would not keep it live.
    key = load_openrouter_key();
    // Named map, then move. `ApiClient({{...}})` is ambiguous on GCC 11
    // between the map constructor and the deleted copy.
    std::map<std::string, std::string> keys{{"openrouter", key}};
    arbiter::ApiClient client(std::move(keys));
    key.assign(key.size(), '\0');

    arbiter::LunaLogprobBackend backend(client);
    int failures = 0;
    std::vector<double> latencies;
    for (const Sample& sample : kSamples) {
        arbiter::DecisionResult result = arbiter::decide(
            backend, sample.text,
            "Which specialist should handle this request?",
            options);
        print_result(sample, result);
        latencies.push_back(result.latency_ms);
        if (!result.ok) {
            ++failures;
            if (logprobs_unsupported(result.error)) {
                std::cout << "Stopped: logprobs were rejected. Remaining samples\n";
                std::cout << "were not sent.\n";
                break;
            }
        }
    }
    if (!latencies.empty()) {
        std::cout << "latency_ms samples: " << latencies.size() << "\n";
        std::cout << "latency_ms p50: "
                  << percentile_nearest_rank(latencies, 0.50) << "\n";
        std::cout << "latency_ms p95: "
                  << percentile_nearest_rank(latencies, 0.95) << "\n";
    }
    return failures == 0 ? 0 : 1;
}
