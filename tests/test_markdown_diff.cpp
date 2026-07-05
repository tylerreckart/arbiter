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

TEST_CASE("MarkdownRenderer defers non-diff fenced blocks until close") {
    MarkdownRenderer md;
    const char* input =
        "before\n"
        "```python\n"
        "line1\n"
        "line2\n"
        "line3\n"
        "```\n"
        "after\n";

    std::string out = md.feed(input);
    out += md.flush();

    CHECK(out.find("before") != std::string::npos);
    CHECK(out.find("after") != std::string::npos);
    CHECK(out.find("line1") != std::string::npos);
    CHECK(out.find("line2") != std::string::npos);
    CHECK(out.find("line3") != std::string::npos);
    CHECK(out.find("```python") != std::string::npos);
}

TEST_CASE("MarkdownRenderer emits preview for long deferred code blocks") {
    MarkdownRenderer md;
    std::string input = "lead\n```txt\n";
    for (int i = 0; i < 10; ++i) input += "body" + std::to_string(i) + "\n";
    input += "```\n";

    std::string mid = md.feed(input.substr(0, input.find("body9")));
    CHECK(mid.find("… (code block, 8+ lines) …") != std::string::npos);
    CHECK(mid.find("body0") == std::string::npos);

    std::string out = mid + md.feed(input.substr(input.find("body9")));
    out += md.flush();
    CHECK(out.find("body0") != std::string::npos);
    CHECK(out.find("body9") != std::string::npos);
}

TEST_CASE("truncate_interim_output collapses fenced blocks and caps length") {
    const std::string input =
        "summary line\n"
        "```diff\n"
        "--- a\n"
        "+++ b\n"
        "+lots\n"
        "```\n"
        "tail\n"
        "extra1\n"
        "extra2\n"
        "extra3\n"
        "extra4\n"
        "extra5\n"
        "extra6\n"
        "extra7\n";

    const std::string out = truncate_interim_output(input, 4, 200);
    CHECK(out.find("summary line") != std::string::npos);
    CHECK(out.find("… (fenced block) …") != std::string::npos);
    CHECK(out.find("--- a") == std::string::npos);
    CHECK(out.find("[truncated") != std::string::npos);
}
