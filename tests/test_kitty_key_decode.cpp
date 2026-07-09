// Direct unit tests for decode_kitty_csi_u — no PTY, no rendering, just the
// parameter-string -> legacy-byte decode logic. PTY-level end-to-end wiring
// (proving read_key_event() actually reaches this decoder) is covered
// separately in test_line_editor.cpp; the finer-grained cases (event types,
// malformed input) belong here where they aren't at the mercy of
// diff-rendering or UI text.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/opentui/kitty_key_decode.h"

using arbiter::opentui::decode_kitty_csi_u;

TEST_CASE("plain Ctrl+letter reports decode to the legacy C0 byte") {
    CHECK(decode_kitty_csi_u("112;5") == 0x10);   // Ctrl-P
    CHECK(decode_kitty_csi_u("114;5") == 0x12);   // Ctrl-R
    CHECK(decode_kitty_csi_u("119;5") == 0x17);   // Ctrl-W
    CHECK(decode_kitty_csi_u("97;5") == 0x01);    // Ctrl-A
    CHECK(decode_kitty_csi_u("122;5") == 0x1A);   // Ctrl-Z
}

TEST_CASE("uppercase-letter codepoints under ctrl still map to the same C0 byte") {
    // Some terminals report the base codepoint as uppercase when shift is
    // also physically involved in producing it on certain layouts.
    CHECK(decode_kitty_csi_u("80;5") == 0x10);    // Ctrl+P (as 'P')
}

TEST_CASE("alternate-key colon subfields on the codepoint are ignored for the base decode") {
    // "report alternate keys" (part of the flags OpenTUI pushes) adds a
    // colon-separated shifted/base-layout codepoint after the primary one.
    // Only the primary codepoint should drive the legacy-byte mapping.
    CHECK(decode_kitty_csi_u("112:80;5") == 0x10);      // Ctrl-P, shifted alt 'P'
    CHECK(decode_kitty_csi_u("119:87;5") == 0x17);      // Ctrl-W, shifted alt 'W'
    CHECK(decode_kitty_csi_u("112:80:112;5") == 0x10);  // + base-layout subfield too
}

TEST_CASE("event-type subfield on the modifier selects press/repeat vs release") {
    CHECK(decode_kitty_csi_u("112;5:1") == 0x10);   // explicit press
    CHECK(decode_kitty_csi_u("112;5:2") == 0x10);   // repeat — still acts
    CHECK(decode_kitty_csi_u("112;5:3") == std::nullopt);  // release — no legacy equivalent
    CHECK(decode_kitty_csi_u("112:80;5:3") == std::nullopt);  // release + alt-key colon
}

TEST_CASE("disambiguated Esc/Enter/Tab/Backspace decode with no modifiers") {
    CHECK(decode_kitty_csi_u("27") == 0x1B);
    CHECK(decode_kitty_csi_u("13") == '\r');
    CHECK(decode_kitty_csi_u("9") == '\t');
    CHECK(decode_kitty_csi_u("127") == 0x7F);
    // But Esc/Enter/Tab/Backspace are meaningless with ctrl held — no
    // legacy single-byte form exists for e.g. Ctrl+Esc.
    CHECK(decode_kitty_csi_u("27;5") == std::nullopt);
}

TEST_CASE("Ctrl+bracket/backslash/underscore and Ctrl+Space/? land in the C0 range") {
    CHECK(decode_kitty_csi_u("91;5") == 0x1B);   // Ctrl+[
    CHECK(decode_kitty_csi_u("92;5") == 0x1C);   // Ctrl+\  (backslash)
    CHECK(decode_kitty_csi_u("93;5") == 0x1D);   // Ctrl+]
    CHECK(decode_kitty_csi_u("94;5") == 0x1E);   // Ctrl+^
    CHECK(decode_kitty_csi_u("95;5") == 0x1F);   // Ctrl+_
    CHECK(decode_kitty_csi_u("32;5") == 0x00);   // Ctrl+Space
    CHECK(decode_kitty_csi_u("63;5") == 0x7F);   // Ctrl+?
}

TEST_CASE("modifiers beyond plain ctrl (alt, shift) are not collapsed to a legacy byte") {
    // Ctrl+Alt+P (mods = 1 + ctrl(4) + alt(2) = 7) has no single-byte
    // legacy form; must not be misdecoded as plain Ctrl-P.
    CHECK(decode_kitty_csi_u("112;7") == std::nullopt);
    // Ctrl+Shift+P (mods = 1 + ctrl(4) + shift(1) = 6).
    CHECK(decode_kitty_csi_u("112;6") == std::nullopt);
}

TEST_CASE("no-modifier printable-range codepoints without ctrl are not touched") {
    // Plain 'p' with no modifiers shouldn't normally reach this decoder at
    // all (it stays a legacy byte the terminal never re-encodes), but if it
    // did, it has no ctrl-derived mapping and isn't one of the
    // Esc/Enter/Tab/Backspace special cases, so it must decode to nothing.
    CHECK(decode_kitty_csi_u("112") == std::nullopt);
    CHECK(decode_kitty_csi_u("112;1") == std::nullopt);  // mods=1 == "no modifiers"
}

TEST_CASE("malformed or empty parameter strings decode to nothing") {
    CHECK(decode_kitty_csi_u("") == std::nullopt);
    CHECK(decode_kitty_csi_u(";5") == std::nullopt);
    CHECK(decode_kitty_csi_u("abc;5") == std::nullopt);
    CHECK(decode_kitty_csi_u("112;abc") == std::nullopt);
    CHECK(decode_kitty_csi_u("0;5") == std::nullopt);
}
