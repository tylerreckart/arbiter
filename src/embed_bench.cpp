// arbiter/src/embed_bench.cpp — accuracy, Brier, ECE, warm latency.
//
// Exits 0 when the pinned model is absent so offline CI can build the
// stub and still run this target. Exits 1 when the eval file itself is
// missing. Does not invent a score for a model it did not run.

#include "embed_route.h"
#include "intent.h"
#include "json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef ARBITER_SOURCE_DIR
#define ARBITER_SOURCE_DIR "."
#endif

namespace {

struct Row {
    std::string text;
    std::string agent;
};

struct Score {
    bool ok = false;
    bool correct = false;
    double probability = 0;
    double confidence = 0;
    double brier = 0;
    std::string choice;
};

double nearest_rank(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const double n = static_cast<double>(values.size());
    double rank = std::ceil(p * n);
    if (rank < 1.0) rank = 1.0;
    if (rank > n) rank = n;
    return values[static_cast<std::size_t>(rank) - 1];
}

std::vector<Row> load_eval(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::vector<Row> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto parsed = arbiter::json_parse(line);
        if (!parsed) continue;
        Row row;
        row.text = parsed->get_string("text");
        row.agent = parsed->get_string("agent");
        if (!row.text.empty() && !row.agent.empty()) rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<arbiter::IntentRosterEntry> load_roster(const std::filesystem::path& dir) {
    std::vector<arbiter::IntentRosterEntry> roster;
    if (!std::filesystem::is_directory(dir)) return roster;
    for (const auto& ent : std::filesystem::directory_iterator(dir)) {
        if (!ent.is_regular_file() || ent.path().extension() != ".json") continue;
        std::ifstream in(ent.path());
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        try {
            auto root = arbiter::json_parse(ss.str());
            arbiter::IntentRosterEntry entry;
            entry.id = ent.path().stem().string();
            entry.role = root->get_string("role");
            entry.goal = root->get_string("goal");
            auto examples = root->get("routing_examples");
            if (examples && examples->is_array()) {
                for (const auto& value : examples->as_array()) {
                    if (value && value->is_string())
                        entry.routing_examples.push_back(value->as_string());
                }
            }
            if (!entry.id.empty()) roster.push_back(std::move(entry));
        } catch (const std::exception& e) {
            std::cerr << "skip " << ent.path().string() << ": " << e.what() << "\n";
        }
    }
    return roster;
}

Score score_one(const arbiter::DecisionResult& result, const Row& row) {
    Score score;
    score.ok = result.ok;
    score.choice = result.choice;
    score.probability = result.probability;
    score.confidence = result.confidence;
    score.correct = result.ok && result.choice == row.agent;
    bool saw_gold = false;
    for (const auto& [id, maybe] : result.distribution) {
        const double p = (maybe && std::isfinite(*maybe)) ? *maybe : 0;
        const double y = id == row.agent ? 1.0 : 0.0;
        if (id == row.agent) saw_gold = true;
        score.brier += (p - y) * (p - y);
    }
    if (!saw_gold) score.brier += 1.0;
    return score;
}

struct Summary {
    int n = 0;
    int correct = 0;
    int accepted = 0;
    int accepted_correct = 0;
    double brier = 0;
    double ece = 0;
};

Summary summarize(const std::vector<Score>& scores, double cutoff) {
    Summary out;
    out.n = static_cast<int>(scores.size());
    struct Bin {
        int n = 0;
        int correct = 0;
        double conf = 0;
    };
    Bin bins[10];
    for (const auto& score : scores) {
        if (score.correct) ++out.correct;
        out.brier += score.brier;
        if (score.confidence >= cutoff) {
            ++out.accepted;
            if (score.correct) ++out.accepted_correct;
        }
        int bin = static_cast<int>(score.probability * 10.0);
        if (bin < 0) bin = 0;
        if (bin > 9) bin = 9;
        bins[bin].n += 1;
        bins[bin].conf += score.probability;
        if (score.correct) bins[bin].correct += 1;
    }
    if (out.n > 0) out.brier /= out.n;
    for (const auto& bin : bins) {
        if (bin.n == 0 || out.n == 0) continue;
        const double acc = static_cast<double>(bin.correct) / bin.n;
        const double avg = bin.conf / bin.n;
        out.ece += (static_cast<double>(bin.n) / out.n) * std::fabs(acc - avg);
    }
    return out;
}

void print_summary(const char* label, const Summary& s, double cutoff) {
    const double acc = s.n ? static_cast<double>(s.correct) / s.n : 0;
    const double abstain = s.n ? 1.0 - static_cast<double>(s.accepted) / s.n : 0;
    const double acc_acc = s.accepted ? static_cast<double>(s.accepted_correct) / s.accepted : 0;
    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);
    std::cout << label << " n=" << s.n
              << " accuracy=" << acc
              << " brier=" << s.brier
              << " ece=" << s.ece
              << " cutoff=" << cutoff
              << " abstain=" << abstain
              << " accepted_accuracy=" << acc_acc
              << "\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string root = ARBITER_SOURCE_DIR;
    std::string eval_path = root + "/tests/data/intent_eval.jsonl";
    std::string agents_dir = root + "/agents";
    double temperature = arbiter::kEmbedDefaultTemperature;
    double cutoff = arbiter::kEmbedDefaultMinConfidence;
    bool sweep = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) {
            if (arg != flag || i + 1 >= argc) return std::string();
            return std::string(argv[++i]);
        };
        if (arg == "--sweep") sweep = true;
        else if (arg == "--eval") eval_path = need("--eval");
        else if (arg == "--agents") agents_dir = need("--agents");
        else if (arg == "--temperature") temperature = std::strtod(need("--temperature").c_str(), nullptr);
        else if (arg == "--cutoff") cutoff = std::strtod(need("--cutoff").c_str(), nullptr);
    }

    const auto rows = load_eval(eval_path);
    if (rows.empty()) {
        std::cerr << "eval set missing or empty: " << eval_path << "\n";
        return 1;
    }
    const auto roster = load_roster(agents_dir);
    auto options = arbiter::embed_options_from_roster(roster);
    if (options.size() < 2) {
        std::cerr << "need at least two agents in " << agents_dir << "\n";
        return 1;
    }

    auto& router = arbiter::EmbeddingRouter::instance();
    if (!router.prepare()) {
        std::cout << "embedding model unavailable: " << router.last_error() << "\n";
        std::cout << "bench skipped\n";
        return 0;
    }

    const auto built = std::chrono::steady_clock::now();
    router.precompute(options);
    const double centroid_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - built)
                                   .count();

    arbiter::DecisionRequest warm;
    warm.state = rows.front().text;
    warm.question = "Which specialist should handle this request?";
    warm.options = options;
    for (int i = 0; i < 8; ++i) router.decide(warm, temperature);

    auto run = [&](double temp) {
        std::vector<Score> scores;
        std::vector<double> latency;
        scores.reserve(rows.size());
        latency.reserve(rows.size());
        for (const auto& row : rows) {
            arbiter::DecisionRequest req;
            req.state = row.text;
            req.question = warm.question;
            req.options = options;
            const auto t0 = std::chrono::steady_clock::now();
            arbiter::DecisionResult result = router.decide(req, temp);
            latency.push_back(std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count());
            scores.push_back(score_one(result, row));
        }
        return std::make_pair(summarize(scores, cutoff), latency);
    };

    auto [summary, latency] = run(temperature);
    std::cout << "model BAAI/bge-small-en-v1.5 quantized int8\n";
    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);
    std::cout << "temperature=" << temperature
              << " centroid_ms=" << centroid_ms
              << " warm_p50_ms=" << nearest_rank(latency, 0.50)
              << " warm_p95_ms=" << nearest_rank(latency, 0.95)
              << "\n";
    print_summary("embed", summary, cutoff);

    arbiter::IntentInput hint_in;
    hint_in.requested_agent = "index";
    hint_in.roster = roster;
    int heuristic_correct = 0;
    int heuristic_abstain = 0;
    for (const auto& row : rows) {
        hint_in.text = row.text;
        arbiter::Intent hint = arbiter::heuristic_classify(hint_in);
        if (hint.target_agent == row.agent) ++heuristic_correct;
        if (hint.target_agent.empty() || hint.confidence < 0.8) ++heuristic_abstain;
    }
    std::cout << "heuristic n=" << rows.size()
              << " accuracy=" << (static_cast<double>(heuristic_correct) / rows.size())
              << " abstain=" << (static_cast<double>(heuristic_abstain) / rows.size())
              << "\n";

    if (sweep) {
        for (double temp : {0.02, 0.05, 0.07, 0.10, 0.15, 0.20, 0.30}) {
            auto [s, ignored] = run(temp);
            (void)ignored;
            std::ostringstream label;
            label.setf(std::ios::fixed);
            label.precision(2);
            label << "sweep T=" << temp;
            print_summary(label.str().c_str(), s, cutoff);
        }
    }
    return 0;
}
