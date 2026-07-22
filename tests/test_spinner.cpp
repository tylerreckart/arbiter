#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/spinner.h"

#include <string>

using namespace arbiter;

TEST_CASE("spinner_frame returns a non-empty Braille glyph") {
    const std::string_view frame = spinner_frame();
    REQUIRE(!frame.empty());
    // Braille patterns are in U+2800..U+28FF (UTF-8: E2 A0 .. / E2 A3 ..).
    CHECK(frame.size() == 3);
    CHECK(static_cast<unsigned char>(frame[0]) == 0xE2);
}

TEST_CASE("wait_phrase returns a rotating status phrase") {
    const std::string_view phrase = wait_phrase();
    REQUIRE(!phrase.empty());
    CHECK(std::string(phrase).find("\u2026") != std::string::npos);
}

TEST_CASE("wait_status_label combines phrase and Braille") {
    const std::string label = wait_status_label();
    REQUIRE(label.size() > 4);
    CHECK(label.find(' ') != std::string::npos);
}

TEST_CASE("spinner_status_label prefixes Braille to a fixed label") {
    const std::string label = spinner_status_label("2 tool calls\u2026");
    REQUIRE(label.size() > 12);
    CHECK(label.find("tool calls") != std::string::npos);
    CHECK(label.find(' ') != std::string::npos);
}
