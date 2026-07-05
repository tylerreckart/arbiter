#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "markdown.h"

using namespace arbiter;

TEST_CASE("MarkdownRenderer routes ```diff fences to diff sink") {
    bool called = false;
    std::string captured;
    MarkdownRenderer md;
    md.set_diff_sink([&](const std::string& patch) {
        called = true;
        captured = patch;
    });

    const char* input =
        "intro\n"
        "```diff\n"
        "--- a/foo\n"
        "+++ b/foo\n"
        "@@ -1 +1 @@\n"
        "-old\n"
        "+new\n"
        "```\n"
        "outro\n";

    std::string out = md.feed(input);
    out += md.flush();

    CHECK(called);
    CHECK(captured.find("--- a/foo") != std::string::npos);
    CHECK(captured.find("+new") != std::string::npos);
    CHECK(out.find("intro") != std::string::npos);
    CHECK(out.find("outro") != std::string::npos);
    CHECK(out.find("```diff") == std::string::npos);
    CHECK(out.find("+new") == std::string::npos);
    CHECK(out == "intro\noutro\n");
}

TEST_CASE("render_diff_ansi colors add and remove lines") {
    const std::string patch =
        "--- a\n"
        "+++ b\n"
        "@@ -1 +1 @@\n"
        "-rm\n"
        "+add\n";
    const std::string styled = render_diff_ansi(patch);
    CHECK(styled.find("-rm") != std::string::npos);
    CHECK(styled.find("+add") != std::string::npos);
    CHECK(styled.find("\033[") != std::string::npos);
}
