#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "code_highlighter.h"
#include "styled_text.h"

using namespace arbiter;

namespace {

bool line_has_style(const StyledLine& line, StyleId id) {
    for (const StyleSpan& span : line.spans) {
        if (span.id == id) return true;
    }
    return false;
}

} // namespace

TEST_CASE("normalize_code_lang maps common aliases") {
    CHECK(normalize_code_lang("C++") == "cpp");
    CHECK(normalize_code_lang("python") == "python");
    CHECK(normalize_code_lang("  ts ") == "typescript");
    CHECK(normalize_code_lang("bash") == "shell");
}

TEST_CASE("highlight_code_line colors cpp keywords and comments") {
    const StyledLine line = highlight_code_line("cpp", "int main() { // entry");
    CHECK(line.text == "int main() { // entry");
    CHECK(line_has_style(line, StyleId::CodeKeyword));
    CHECK(line_has_style(line, StyleId::CodeComment));
}

TEST_CASE("highlight_code_line colors python strings") {
    const StyledLine line = highlight_code_line("python", "msg = \"hello\"");
    CHECK(line_has_style(line, StyleId::CodeString));
}

TEST_CASE("highlight_code_block preserves line count") {
    const std::vector<std::string> src = {"one", "two", "three"};
    const auto out = highlight_code_block("text", src);
    CHECK(out.size() == 3);
    CHECK(out[0].text == "one");
}

TEST_CASE("empty fence lang still highlights strings and comments") {
    const StyledLine line = highlight_code_line("", "x = \"hi\" # note");
    CHECK(line_has_style(line, StyleId::CodeString));
    CHECK(line_has_style(line, StyleId::CodeComment));
}
