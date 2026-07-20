#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/confirm_keys.h"

TEST_CASE("is_abandon_key recognizes legacy Esc and Ctrl-C") {
    CHECK(arbiter::is_abandon_key(0x03, 0, ""));
    CHECK(arbiter::is_abandon_key(0x1B, 0, ""));
    CHECK_FALSE(arbiter::is_abandon_key('y', 0, ""));
    CHECK_FALSE(arbiter::is_abandon_key(0x1B, 'A', ""));  // arrow
}

TEST_CASE("is_abandon_key recognizes kitty CSI-u Esc and Ctrl-C") {
    CHECK(arbiter::is_abandon_key(0x1B, 'u', "27"));
    CHECK(arbiter::is_abandon_key(0x1B, 'u', "99;5"));  // Ctrl-C
    CHECK_FALSE(arbiter::is_abandon_key(0x1B, 'u', "112;5"));  // Ctrl-P
    CHECK_FALSE(arbiter::is_abandon_key(0x1B, 'u', "27;5"));   // Ctrl+Esc — no legacy
}

TEST_CASE("is_abandon_key ignores SGR mouse finals") {
    CHECK_FALSE(arbiter::is_abandon_key(0x1B, 'M', "<0;1;1"));
    CHECK_FALSE(arbiter::is_abandon_key(0x1B, 'm', "<0;1;1"));
}
