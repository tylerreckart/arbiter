#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/glyphs.h"
#include "tui/spinner.h"

#include <cstdlib>
#include <string>

using namespace arbiter;

namespace {
struct GlyphModeGuard {
    std::string prev;
    bool had = false;
    explicit GlyphModeGuard(const char* mode) {
        if (const char* p = std::getenv("ARBITER_GLYPHS")) {
            had = true;
            prev = p;
        }
        setenv("ARBITER_GLYPHS", mode, 1);
    }
    ~GlyphModeGuard() {
        if (had) setenv("ARBITER_GLYPHS", prev.c_str(), 1);
        else unsetenv("ARBITER_GLYPHS");
    }
};
}  // namespace

TEST_CASE("spinner_frame returns a non-empty Braille glyph") {
    GlyphModeGuard mode("unicode");
    const std::string_view frame = spinner_frame();
    REQUIRE(!frame.empty());
    // Braille patterns are in U+2800..U+28FF (UTF-8: E2 A0 .. / E2 A3 ..).
    CHECK(frame.size() == 3);
    CHECK(static_cast<unsigned char>(frame[0]) == 0xE2);
}

TEST_CASE("wait_phrase returns a rotating status phrase") {
    GlyphModeGuard mode("unicode");
    const std::string_view phrase = wait_phrase();
    REQUIRE(!phrase.empty());
    CHECK(std::string(phrase).find("\u2026") != std::string::npos);
}

TEST_CASE("wait_status_label combines phrase and Braille") {
    GlyphModeGuard mode("unicode");
    const std::string label = wait_status_label();
    REQUIRE(label.size() > 4);
    CHECK(label.find(' ') != std::string::npos);
}

TEST_CASE("ascii glyph mode uses a one-byte spinner and three-dot phrases") {
    GlyphModeGuard mode("ascii");
    const std::string_view frame = spinner_frame();
    REQUIRE(frame.size() == 1);
    const char c = frame[0];
    CHECK((c == '|' || c == '/' || c == '-' || c == '\\'));
    const std::string phrase(wait_phrase());
    CHECK(phrase.find("...") != std::string::npos);
    CHECK(phrase.find("\u2026") == std::string::npos);
    const std::string label = spinner_status_label("fetch");
    CHECK(label.size() >= 2);
    CHECK(label[1] == ' ');
    CHECK(label.find("fetch") != std::string::npos);
}

TEST_CASE("spinner_status_label prefixes Braille to a fixed label") {
    GlyphModeGuard mode("unicode");
    const std::string label = spinner_status_label("2 tool calls\u2026");
    REQUIRE(label.size() > 12);
    CHECK(label.find("tool calls") != std::string::npos);
    CHECK(label.find(' ') != std::string::npos);
}
