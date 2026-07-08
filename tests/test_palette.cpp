// tests/test_palette.cpp — Pins the Ctrl-P palette's ranking contract:
// name prefix beats name substring beats description substring beats
// name subsequence; ties keep input order; empty query passes through.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/palette.h"

using namespace arbiter;

static std::vector<PaletteItem> sample() {
    return {
        {"/agents",  "list loaded agents"},
        {"/chat",    "conversation management"},
        {"/find",    "search the focused pane's scrollback"},
        {"/schedule","schedule recurring tasks"},
        {"/search",  "web search"},
    };
}

TEST_CASE("empty query returns everything in input order") {
    const auto out = palette_filter(sample(), "");
    REQUIRE(out.size() == 5);
    CHECK(out.front().name == "/agents");
    CHECK(out.back().name == "/search");
}

TEST_CASE("name prefix outranks substring and description hits") {
    // "sea" prefixes /search (sans slash), substrings nothing else's name,
    // and appears in /find's description ("search the focused ...").
    const auto out = palette_filter(sample(), "sea");
    REQUIRE(out.size() >= 2);
    CHECK(out[0].name == "/search");
    CHECK(out[1].name == "/find");   // description tier
}

TEST_CASE("leading slash in the query is honored too") {
    const auto out = palette_filter(sample(), "/sch");
    REQUIRE(!out.empty());
    CHECK(out[0].name == "/schedule");
}

TEST_CASE("subsequence matches catch scattered letters") {
    // "sdl" is a subsequence of /schedule only.
    const auto out = palette_filter(sample(), "sdl");
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "/schedule");
}

TEST_CASE("case-insensitive matching") {
    const auto out = palette_filter(sample(), "AGENTS");
    REQUIRE(!out.empty());
    CHECK(out[0].name == "/agents");
}

TEST_CASE("no matches yields empty") {
    CHECK(palette_filter(sample(), "zzz-not-here").empty());
}
