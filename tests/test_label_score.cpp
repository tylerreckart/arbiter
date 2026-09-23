// tests/test_label_score.cpp — Renormalized label distributions and the
// env gates. No provider client.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "label_score.h"

using namespace arbiter;

static std::vector<LabelSpec> two() {
    return {
        {'S', "silent", "stay quiet"},
        {'C', "context", "a note would help"},
    };
}

static std::vector<LabelSpec> three() {
    return {
        {'R', "research", "sources"},
        {'W', "write", "prose"},
        {'U', "unknown", "none of these"},
    };
}

TEST_CASE("softmax over the supplied letters only") {
    auto d = distribution_from_logprobs(two(), {
        {"S", 0.0},
        {"C", -10.0},
        {"Z", 5.0},  // not a label; ignored
    });
    REQUIRE(d.ok);
    CHECK(d.argmax == "silent");
    CHECK(d.peak > 0.999);
    CHECK(d.margin > 0.99);
    CHECK(d.probs.size() == 2);
    CHECK(d.probs[0].first == "silent");
}

TEST_CASE("a missing letter is not a zero") {
    auto d = distribution_from_logprobs(three(), {
        {"R", 0.0},
        {"W", -0.5},
    });
    CHECK_FALSE(d.ok);
    CHECK(d.argmax.empty());
    CHECK(d.probs.empty());
}

TEST_CASE("whitespace around a letter matches; a word does not") {
    auto spaced = distribution_from_logprobs(two(), {
        {" S", 0.0},
        {"\nC", -1.0},
    });
    CHECK(spaced.ok);
    CHECK(spaced.argmax == "silent");

    auto word = distribution_from_logprobs(two(), {
        {"Silent", 0.0},
        {"C", -0.2},
    });
    CHECK_FALSE(word.ok);
}

TEST_CASE("the higher logprob wins when a letter is repeated") {
    auto d = distribution_from_logprobs(two(), {
        {"s", -3.0},
        {"S", 0.0},
        {"C", -1.0},
    });
    REQUIRE(d.ok);
    CHECK(d.argmax == "silent");
    CHECK(d.peak > d.probs[1].second);
}

TEST_CASE("openai body reads top_logprobs and ignores other shapes") {
    const char* body = R"({
      "choices": [{
        "message": {"content": "R"},
        "logprobs": {
          "content": [{
            "token": "R",
            "logprob": -0.1,
            "top_logprobs": [
              {"token": "R", "logprob": -0.1},
              {"token": "W", "logprob": -2.0},
              {"token": "U", "logprob": -3.0}
            ]
          }]
        }
      }]
    })";
    auto d = distribution_from_openai_body(three(), body);
    REQUIRE(d.ok);
    CHECK(d.argmax == "research");
    CHECK(d.peak > 0.5);

    CHECK_FALSE(distribution_from_openai_body(three(), "{}").ok);
    CHECK_FALSE(distribution_from_openai_body(three(), "not json").ok);
}

TEST_CASE("extreme requires the peak floor and a positive margin") {
    LabelDistribution d;
    d.ok = true;
    d.argmax = "silent";
    d.peak = 0.96;
    d.margin = 0.9;
    CHECK(label_distribution_is_extreme(d, 0.5));
    CHECK_FALSE(label_distribution_is_extreme(d, 0.0));
    d.peak = 0.85;
    CHECK_FALSE(label_distribution_is_extreme(d, 0.5));
    d.peak = 0.96;
    d.margin = 0.2;
    CHECK_FALSE(label_distribution_is_extreme(d, 0.5));
    d.ok = false;
    d.margin = 0.9;
    CHECK_FALSE(label_distribution_is_extreme(d, 0.5));
}

TEST_CASE("margins parse off by default and only ollama ids arm the filter") {
    auto off = decision_filter_from_values(nullptr, nullptr, nullptr);
    CHECK_FALSE(off.enabled_for_presence());
    CHECK_FALSE(off.enabled_for_intent());

    auto local = decision_filter_from_values("ollama/qwen3:0.6b", "0.5", "0.4");
    CHECK(local.enabled_for_presence());
    CHECK(local.enabled_for_intent());
    CHECK(local.presence_silence_margin == doctest::Approx(0.5));

    auto hosted = decision_filter_from_values("anthropic/claude-haiku-4-5", "0.5", "0.5");
    CHECK_FALSE(hosted.enabled_for_presence());
    CHECK_FALSE(hosted.enabled_for_intent());

    auto clamped = decision_filter_from_values("ollama/qwen", "2", "nope");
    CHECK(clamped.presence_silence_margin == doctest::Approx(1.0));
    CHECK(clamped.intent_route_margin == doctest::Approx(0.0));
    CHECK(clamped.enabled_for_presence());
    CHECK_FALSE(clamped.enabled_for_intent());
}

TEST_CASE("prompt lists the letter, the name, and the description") {
    auto prompt = label_score_user_prompt("Task: hello", two());
    CHECK(prompt.find("Task: hello") != std::string::npos);
    CHECK(prompt.find("S. silent — stay quiet") != std::string::npos);
    CHECK(prompt.find("C. context — a note would help") != std::string::npos);
}

TEST_CASE("intent labels include unknown and omit multi") {
    auto specs = intent_decision_labels();
    bool unknown = false;
    bool multi = false;
    for (const auto& s : specs) {
        if (s.name == "unknown") unknown = true;
        if (s.name == "multi") multi = true;
        CHECK(s.letter != 0);
        CHECK_FALSE(s.description.empty());
    }
    CHECK(unknown);
    CHECK_FALSE(multi);
}
