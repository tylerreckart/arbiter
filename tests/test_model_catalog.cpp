#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "model_catalog.h"
#include "model_context.h"

#include <set>
#include <string>

using namespace arbiter;

TEST_CASE("model catalogue is non-empty with unique ids") {
    std::size_t n = 0;
    const auto* models = model_catalog(n);
    REQUIRE(n >= 20);
    REQUIRE(models != nullptr);

    std::set<std::string> ids;
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(models[i].id != nullptr);
        CHECK(models[i].id[0] != '\0');
        CHECK(models[i].provider != nullptr);
        CHECK(std::string(models[i].provider) == "openrouter");
        CHECK(models[i].context_window > 0);
        CHECK(ids.insert(models[i].id).second);
    }
}

TEST_CASE("find_model_catalog_entry exact match") {
    const auto* sonnet = find_model_catalog_entry("anthropic/claude-sonnet-5");
    REQUIRE(sonnet != nullptr);
    CHECK(sonnet->context_window == 1'000'000);
    CHECK(std::string(sonnet->provider) == "openrouter");

    const auto* haiku = find_model_catalog_entry("claude-haiku-4-5");
    REQUIRE(haiku != nullptr);
    CHECK(haiku->context_window == 200'000);

    const auto* grok = find_model_catalog_entry("x-ai/grok-4.5");
    REQUIRE(grok != nullptr);
    CHECK(grok->context_window == 500'000);

    CHECK(find_model_catalog_entry("not-a-real-model") == nullptr);
    CHECK(find_model_catalog_entry("") == nullptr);
}

TEST_CASE("context_window_for_model prefers catalogue entries") {
    std::size_t n = 0;
    const auto* models = model_catalog(n);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(context_window_for_model(models[i].id) == models[i].context_window);
    }
}

TEST_CASE("context_window_for_model keeps heuristic fallback") {
    // Unlisted ids still get a useful estimate for compaction / sidebar.
    CHECK(context_window_for_model("anthropic/claude-sonnet-4.6") == 1'000'000);
    CHECK(context_window_for_model("some-vendor/gpt-5-experimental") == 1'000'000);
    CHECK(context_window_for_model("ollama/llama3") == 0);
    CHECK(context_window_for_model("") == 0);
}

TEST_CASE("format_context_window") {
    CHECK(format_context_window(1'000'000) == "1M");
    CHECK(format_context_window(200'000) == "200k");
    CHECK(format_context_window(128'000) == "128k");
    CHECK(format_context_window(0) == "unknown");
    CHECK(format_context_window(12345) == "12345");
}

TEST_CASE("format_model_catalog_list mentions usage") {
    const std::string list = format_model_catalog_list();
    CHECK(list.find("anthropic/claude-sonnet-5") != std::string::npos);
    CHECK(list.find("ctx=1M") != std::string::npos);
    CHECK(list.find("/model <agent-id> <model-id>") != std::string::npos);
}

TEST_CASE("wizard recommended ids resolve in catalogue") {
    static constexpr const char* kRecommended[] = {
        "~openai/gpt-latest",
        "anthropic/claude-sonnet-5",
        "openai/gpt-5.5",
        "google/gemini-3.6-flash",
        "google/gemini-3.1-flash-lite",
    };
    for (const char* id : kRecommended) {
        const auto* e = find_model_catalog_entry(id);
        REQUIRE(e != nullptr);
        CHECK(e->recommended);
        CHECK(e->context_window > 0);
    }
}
