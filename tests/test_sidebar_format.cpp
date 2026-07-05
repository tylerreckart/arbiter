#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/sidebar_format.h"

using namespace arbiter;

TEST_CASE("format_token_count scales at k and M boundaries") {
    CHECK(format_token_count(500) == "500");
    CHECK(format_token_count(1500) == "1.5k");
    CHECK(format_token_count(12000) == "12.0k");
    CHECK(format_token_count(2'500'000) == "2.5M");
}

TEST_CASE("estimate_cost_usd is model-aware") {
    CHECK(estimate_cost_usd("claude-haiku-4-5", 1'000'000, 0) == doctest::Approx(0.8));
    CHECK(estimate_cost_usd("claude-sonnet-4-6", 1'000'000, 0) == doctest::Approx(3.0));
    CHECK(estimate_cost_usd("claude-opus-4-7", 0, 1'000'000) == doctest::Approx(75.0));
    CHECK(estimate_cost_usd("ollama/llama3", 1'000'000, 1'000'000) == doctest::Approx(0.0));
}

TEST_CASE("cost_basis_label reflects mixed and local models") {
    CHECK(cost_basis_label("claude-sonnet-4-6", false) == "claude-sonnet-4-6");
    CHECK(cost_basis_label("claude-sonnet-4-6", true) == "mixed");
    CHECK(cost_basis_label("ollama/llama3", false) == "local");
}

TEST_CASE("format_context_pct uses model window and last-turn prompt tokens") {
    CHECK(context_pct_value(40'000, "claude-sonnet-4-6") == 20);
    CHECK(context_pct_value(64'000, "gpt-4o") == 50);
    CHECK(format_context_pct(40'000, "claude-sonnet-4-6") == "20%");
    CHECK(context_pct_value(10'000, "ollama/llama3") == -1);
    CHECK(context_pct_value(0, "claude-sonnet-4-6") == -1);
}
