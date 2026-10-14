// tests/test_embed_route.cpp — centroid math, WordPiece, eval-file shape.
// The ONNX session is not required. Accuracy and latency are arbiter-embed-bench.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "embed_route.h"
#include "json.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

using namespace arbiter;

#ifndef ARBITER_SOURCE_DIR
#define ARBITER_SOURCE_DIR "."
#endif

namespace {

struct EnvGuard {
    std::string name;
    bool had = false;
    std::string prev;
    EnvGuard(const char* n, const char* value) : name(n) {
        if (const char* cur = std::getenv(n)) {
            had = true;
            prev = cur;
        }
        setenv(n, value, 1);
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), prev.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

} // namespace

TEST_CASE("wordpiece splits words, punctuation, and continuations") {
    WordPieceVocab vocab;
    CHECK(wordpiece_load_lines(vocab, {
        "[PAD]", "[UNK]", "[CLS]", "[SEP]", "hello", "world", "##ing", ","
    }));
    CHECK(vocab.cls_id == 2);
    CHECK(vocab.unk_id == 1);
    auto hello = wordpiece_encode(vocab, "Hello, world", 32);
    CHECK(hello == std::vector<int64_t>{2, 4, 7, 5, 3});
    auto ing = wordpiece_encode(vocab, "helloing", 32);
    CHECK(ing == std::vector<int64_t>{2, 4, 6, 3});
    auto unk = wordpiece_encode(vocab, "zzz", 32);
    CHECK(unk == std::vector<int64_t>{2, 1, 3});
}

TEST_CASE("softmax over cosine keeps a near match and a tie is unconfident") {
    std::vector<Centroid> centroids = {
        {"scout", {1.f, 0.f}},
        {"vera", {0.f, 1.f}},
    };
    auto hit = score_embedding_centroids({1.f, 0.f}, centroids, 0.1);
    CHECK(hit.ok);
    CHECK(hit.choice == "scout");
    CHECK(hit.probability > 0.9);
    CHECK(hit.confidence == doctest::Approx(decision_confidence(hit.probability, 2)));
    CHECK(hit.labels_observed == 2);
    double sum = 0;
    for (const auto& [id, p] : hit.distribution) {
        REQUIRE(p.has_value());
        sum += *p;
        (void)id;
    }
    CHECK(sum == doctest::Approx(1.0));

    std::vector<Centroid> tied = {
        {"scout", {1.f, 0.f}},
        {"vera", {1.f, 0.f}},
    };
    auto even = score_embedding_centroids({1.f, 0.f}, tied, 0.07);
    CHECK(even.ok);
    CHECK(even.probability == doctest::Approx(0.5));
    CHECK(even.confidence == doctest::Approx(0.0));
}

TEST_CASE("centroid backend averages examples and caches the roster") {
    EmbeddingCentroidBackend backend;
    backend.set_temperature(0.05);
    int calls = 0;
    backend.set_embed([&](const std::string& text) -> std::vector<float> {
        ++calls;
        if (text.find("review") != std::string::npos || text.find("diff") != std::string::npos)
            return {0.f, 1.f};
        if (text.find("research") != std::string::npos || text.find("source") != std::string::npos)
            return {1.f, 0.f};
        return {1.f, 1.f};
    });
    DecisionOption scout;
    scout.id = "scout";
    scout.description = "research-analyst. Find primary sources";
    DecisionOption vera;
    vera.id = "vera";
    vera.description = "code-reviewer. Inspect real code";
    vera.examples = {"review the diff"};
    DecisionRequest req;
    req.state = "please review this diff";
    req.options = {scout, vera};
    auto first = backend.decide(req);
    CHECK(first.ok);
    CHECK(first.choice == "vera");
    CHECK(first.latency_ms >= 0);
    const int after_first = calls;
    auto second = backend.decide(req);
    CHECK(second.choice == "vera");
    // Query is embedded again; the two agent texts are not.
    CHECK(calls == after_first + 1);
}

TEST_CASE("a missing embedder is an error, not a guess") {
    EmbeddingCentroidBackend backend;
    DecisionRequest req;
    req.state = "hello";
    DecisionOption a;
    a.id = "scout";
    a.description = "research";
    DecisionOption b;
    b.id = "vera";
    b.description = "review";
    req.options = {a, b};
    auto result = backend.decide(req);
    CHECK_FALSE(result.ok);
    CHECK(result.choice.empty());
}

TEST_CASE("operator embed paths that are missing do not fetch") {
    EnvGuard model("ARBITER_EMBED_MODEL", "/tmp/arbiter-missing-model.onnx");
    EnvGuard vocab("ARBITER_EMBED_VOCAB", "/tmp/arbiter-missing-vocab.txt");
    auto assets = resolve_embed_assets();
    CHECK(assets.model.empty());
    CHECK(assets.vocab.empty());
    CHECK_FALSE(assets.error.empty());
}

TEST_CASE("eval set has 59 held-out requests across the roster") {
    const std::string root = ARBITER_SOURCE_DIR;
    std::ifstream eval(root + "/tests/data/intent_eval.jsonl");
    REQUIRE(eval.good());
    std::set<std::string> texts;
    std::set<std::string> agents;
    int rows = 0;
    std::string line;
    while (std::getline(eval, line)) {
        if (line.empty()) continue;
        auto parsed = json_parse(line);
        REQUIRE(parsed);
        const std::string text = parsed->get_string("text");
        const std::string agent = parsed->get_string("agent");
        CHECK_FALSE(text.empty());
        CHECK_FALSE(agent.empty());
        texts.insert(text);
        agents.insert(agent);
        ++rows;
    }
    CHECK(rows == 59);
    CHECK(texts.size() == 59);
    CHECK(agents.size() == 10);

    const char* ids[] = {"scout", "vera", "quill", "forge", "loom",
                         "nexus", "compass", "beacon", "echo", "jules"};
    for (const char* id : ids) {
        std::ifstream in(root + "/agents/" + id + ".json");
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        auto parsed = json_parse(ss.str());
        REQUIRE(parsed);
        auto examples = parsed->get("routing_examples");
        if (!examples || !examples->is_array()) continue;
        for (const auto& value : examples->as_array()) {
            REQUIRE(value);
            REQUIRE(value->is_string());
            CHECK(texts.count(value->as_string()) == 0);
        }
    }
}
