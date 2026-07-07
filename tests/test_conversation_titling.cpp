#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/conversation_titling.h"
#include "styled_text.h"

using namespace arbiter;

TEST_CASE("deterministic title collapses whitespace and returns short messages unchanged") {
    CHECK(deterministic_conversation_title("fix the bug") == "fix the bug");
    CHECK(deterministic_conversation_title("  fix   the\n  bug  ") == "fix the bug");
}

TEST_CASE("deterministic title strips writ lines and markdown markers") {
    CHECK(deterministic_conversation_title("/write foo.txt\nplease refactor this") == "please refactor this");
    CHECK(deterministic_conversation_title("# Heading\nplease refactor this") == "Heading please refactor this");
    CHECK(deterministic_conversation_title("- do the thing") == "do the thing");
    CHECK(deterministic_conversation_title("> quoted text here") == "quoted text here");
    CHECK(deterministic_conversation_title("**bold** and `code`") == "bold and code");
}

TEST_CASE("deterministic title cuts at a word boundary under 40 display columns") {
    const std::string long_msg =
        "refactor the TUI output renderer so that it handles very long streaming responses gracefully";
    const std::string title = deterministic_conversation_title(long_msg);
    CHECK(display_width(title) <= 40);
    CHECK(title.substr(title.size() - 3) == "…");
    // Cut at a word boundary: no trailing partial word before the ellipsis.
    CHECK(long_msg.rfind(title.substr(0, title.size() - 3), 0) == 0);
}

TEST_CASE("deterministic title hard-cuts a single unbroken word that alone exceeds the budget") {
    const std::string one_word(60, 'a');
    const std::string title = deterministic_conversation_title(one_word);
    CHECK(display_width(title) <= 40);
    CHECK(title.substr(title.size() - 3) == "…");
}

TEST_CASE("deterministic title returns empty when nothing survives stripping") {
    CHECK(deterministic_conversation_title("/write foo.txt\n```\n").empty());
    CHECK(deterministic_conversation_title("   \n  ").empty());
}

TEST_CASE("sanitize_model_title strips quotes, backticks, and trailing punctuation") {
    CHECK(sanitize_model_title("\"Refactor the renderer\"") == "Refactor the renderer");
    CHECK(sanitize_model_title("`Refactor the renderer`") == "Refactor the renderer");
    CHECK(sanitize_model_title("Refactor the renderer.") == "Refactor the renderer");
    CHECK(sanitize_model_title("  Refactor the renderer  \n extra line") == "Refactor the renderer");
}

TEST_CASE("sanitize_model_title caps at 60 display columns") {
    const std::string long_reply(100, 'x');
    CHECK(display_width(sanitize_model_title(long_reply)) <= 60);
}

TEST_CASE("sanitize_model_title rejects empty/whitespace-only replies") {
    CHECK(sanitize_model_title("").empty());
    CHECK(sanitize_model_title("   \n  ").empty());
    CHECK(sanitize_model_title("\"\"").empty());
}
