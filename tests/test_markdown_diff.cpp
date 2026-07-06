#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "markdown.h"
#include "render_policy.h"
#include "styled_text.h"

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

TEST_CASE("MarkdownRenderer code_sink receives fence lang tag") {
    std::string captured_lang;
    MarkdownRenderer md;
    md.set_code_sink(
        [&](std::string /*open*/, std::string lang) { captured_lang = std::move(lang); },
        [](const std::string& /*line*/) {},
        [](std::string /*close*/) {});

    md.feed("```go\nx\n```\n");
    md.flush();
    CHECK(captured_lang == "go");

    captured_lang.clear();
    md.feed("```rust\nx\n```\n");
    md.flush();
    CHECK(captured_lang == "rust");
}

TEST_CASE("MarkdownRenderer streams code body through code_sink") {
    std::vector<std::string> captured;
    MarkdownRenderer md;
    md.set_code_sink(
        [](std::string /*open*/, std::string /*lang*/) {},
        [&](const std::string& line) { captured.push_back(line); },
        [](std::string /*close*/) {});

    std::string input = "lead\n```txt\n";
    for (int i = 0; i < 10; ++i) input += "body" + std::to_string(i) + "\n";
    input += "```\n";

    std::string mid = md.feed(input.substr(0, input.find("body9")));
    CHECK(mid.find("body0") == std::string::npos);
    CHECK(mid.find("… (code block") == std::string::npos);
    CHECK(captured.size() >= 8);

    md.feed(input.substr(input.find("body9")));
    md.flush();
    CHECK(captured.size() == 10u);
}

TEST_CASE("kInterim policy collapses fences and caps length") {
    std::vector<StyledLine> lines = {
        styled_plain_line("summary line", StyleId::Default),
        styled_plain_line("```diff", StyleId::Default),
        styled_plain_line("--- a", StyleId::Default),
        styled_plain_line("```", StyleId::Default),
        styled_plain_line("tail", StyleId::Default),
        styled_plain_line("extra1", StyleId::Default),
        styled_plain_line("extra2", StyleId::Default),
        styled_plain_line("extra3", StyleId::Default),
        styled_plain_line("extra4", StyleId::Default),
        styled_plain_line("extra5", StyleId::Default),
        styled_plain_line("extra6", StyleId::Default),
        styled_plain_line("extra7", StyleId::Default),
    };

    const auto out = apply_prose_policy(std::move(lines), kInterim);
    CHECK(out.size() >= 2);
    CHECK(out.front().text.find("summary line") != std::string::npos);
    bool saw_fence_placeholder = false;
    bool saw_truncated = false;
    for (const auto& line : out) {
        if (line.text.find("… (fenced block)") != std::string::npos) saw_fence_placeholder = true;
        if (line.text.find("[truncated") != std::string::npos) saw_truncated = true;
        CHECK(line.text.find("--- a") == std::string::npos);
    }
    CHECK(saw_fence_placeholder);
    CHECK(saw_truncated);
}

TEST_CASE("feed_styled and feed produce equivalent ANSI") {
    const char* input =
        "# Title\n"
        "plain **bold** and `code`\n"
        "- bullet\n";

    MarkdownRenderer md1;
    MarkdownRenderer md2;
    std::string ansi_from_feed = md1.feed(input) + md1.flush();

    std::string ansi_from_styled;
    for (const StyledLine& line : md2.feed_styled(input)) {
        ansi_from_styled += to_ansi(line);
        if (ansi_from_styled.empty() || ansi_from_styled.back() != '\n') {
            ansi_from_styled += '\n';
        }
    }
    for (const StyledLine& line : md2.flush_styled()) {
        ansi_from_styled += to_ansi(line);
        if (ansi_from_styled.empty() || ansi_from_styled.back() != '\n') {
            ansi_from_styled += '\n';
        }
    }

    CHECK(ansi_from_feed == ansi_from_styled);
}

TEST_CASE("display_width counts wide characters") {
    CHECK(display_width("abc") == 3);
    CHECK(display_width("\xe2\x80\xa2") == 1);  // bullet
    CHECK(display_width("hello") == 5);
}
