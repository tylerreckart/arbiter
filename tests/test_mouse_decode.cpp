// Unit tests for decode_sgr_mouse, hit helpers, and weighted split sizing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "repl/layout_weights.h"
#include "tui/opentui/mouse_decode.h"
#include "tui/opentui/mouse_hit.h"

using arbiter::allocate_weighted_sizes;
using arbiter::opentui::decode_sgr_mouse;
using arbiter::opentui::MouseButton;
using arbiter::opentui::MouseType;
using arbiter::opentui::ScrollDir;
using arbiter::opentui::history_sidebar_row_at;
using arbiter::opentui::rect_contains;
using arbiter::Rect;

TEST_CASE("left-button press/release decode to Down/Up with 0-based coords") {
    auto down = decode_sgr_mouse("<0;10;5", 'M');
    REQUIRE(down.has_value());
    CHECK(down->type == MouseType::Down);
    CHECK(down->button == MouseButton::Left);
    CHECK(down->x == 9);
    CHECK(down->y == 4);

    auto up = decode_sgr_mouse("<0;10;5", 'm');
    REQUIRE(up.has_value());
    CHECK(up->type == MouseType::Up);
    CHECK(up->button == MouseButton::Left);
}

TEST_CASE("wheel reports decode as Scroll with direction") {
    auto up = decode_sgr_mouse("<64;3;3", 'M');
    REQUIRE(up.has_value());
    CHECK(up->type == MouseType::Scroll);
    CHECK(up->scroll == ScrollDir::Up);
    CHECK(up->scroll_delta == 1);

    auto down = decode_sgr_mouse("<65;3;3", 'M');
    REQUIRE(down.has_value());
    CHECK(down->type == MouseType::Scroll);
    CHECK(down->scroll == ScrollDir::Down);
}

TEST_CASE("drag motion (button-event tracking) decodes as Drag") {
    // Bit 5 (32) = motion, low bits = left button.
    auto drag = decode_sgr_mouse("<32;20;10", 'M');
    REQUIRE(drag.has_value());
    CHECK(drag->type == MouseType::Drag);
    CHECK(drag->button == MouseButton::Left);
    CHECK(drag->x == 19);
    CHECK(drag->y == 9);
}

TEST_CASE("bare motion (button==3) decodes as Move") {
    auto move = decode_sgr_mouse("<35;8;8", 'M');
    REQUIRE(move.has_value());
    CHECK(move->type == MouseType::Move);
    CHECK(move->button == MouseButton::None);
}

TEST_CASE("modifier bits are surfaced") {
    // shift(4) + ctrl(16) + left button = 20
    auto ev = decode_sgr_mouse("<20;1;1", 'M');
    REQUIRE(ev.has_value());
    CHECK(ev->shift);
    CHECK(ev->ctrl);
    CHECK_FALSE(ev->alt);
}

TEST_CASE("malformed parameter strings are rejected") {
    CHECK_FALSE(decode_sgr_mouse("0;1;1", 'M').has_value());   // missing '<'
    CHECK_FALSE(decode_sgr_mouse("<0;1", 'M').has_value());     // missing field
    CHECK_FALSE(decode_sgr_mouse("<0;1;1", 'A').has_value());   // wrong final
    CHECK_FALSE(decode_sgr_mouse("<0;0;1", 'M').has_value());   // 1-based x < 1
    CHECK_FALSE(decode_sgr_mouse("", 'M').has_value());
}

TEST_CASE("rect_contains and history_sidebar_row_at match frame geometry") {
    Rect r{0, 0, 26, 40};
    CHECK(rect_contains(r, 0, 0));
    CHECK(rect_contains(r, 25, 39));
    CHECK_FALSE(rect_contains(r, 26, 0));
    CHECK_FALSE(rect_contains(r, 0, 40));

    // Box starts at y+1, with a blank row beneath its title, so list_top is
    // y+3 = 3; row height 2; 3 visible slots; 2 real rows
    // ("+ New" + one conversation) so the third slot is empty.
    CHECK(history_sidebar_row_at(r, 3, 0, 3, 2, false) == 0);
    CHECK(history_sidebar_row_at(r, 4, 0, 3, 2, false) == 0);
    CHECK(history_sidebar_row_at(r, 5, 0, 3, 2, false) == 1);
    CHECK(history_sidebar_row_at(r, 7, 0, 3, 2, false) == -1);  // empty slot past list
    CHECK(history_sidebar_row_at(r, 5, 2, 3, 5, false) == 3);   // scrolled into real rows
    CHECK(history_sidebar_row_at(r, 2, 0, 3, 2, false) == -1);  // title/padding
    CHECK(history_sidebar_row_at(r, 9, 0, 3, 2, false) == -1);  // below visible band
    CHECK(history_sidebar_row_at(r, 3, 0, 0, 2, false) == -1);  // no visible rows

    // Filter line shifts list_top to y+4 = 4. Clicking the filter row (y=3)
    // must not activate "+ New"; first conversation row starts at y=4.
    CHECK(history_sidebar_row_at(r, 3, 0, 3, 2, true) == -1);  // filter line
    CHECK(history_sidebar_row_at(r, 4, 0, 3, 2, true) == 0);   // "+ New"
    CHECK(history_sidebar_row_at(r, 6, 0, 3, 2, true) == 1);   // first entry
}

TEST_CASE("allocate_weighted_sizes sums exactly to available") {
    auto equal = allocate_weighted_sizes(10, {1.0, 1.0});
    REQUIRE(equal.size() == 2);
    CHECK(equal[0] + equal[1] == 10);

    auto skewed = allocate_weighted_sizes(10, {3.0, 1.0});
    REQUIRE(skewed.size() == 2);
    CHECK(skewed[0] + skewed[1] == 10);
    CHECK(skewed[0] >= skewed[1]);
}

TEST_CASE("allocate_weighted_sizes handles available < N without overshoot") {
    auto sizes = allocate_weighted_sizes(2, {1.0, 1.0, 1.0, 1.0});
    REQUIRE(sizes.size() == 4);
    int sum = 0;
    for (int s : sizes) {
        CHECK(s >= 0);
        sum += s;
    }
    CHECK(sum == 2);
}

TEST_CASE("allocate_weighted_sizes zero available yields all zeros") {
    auto sizes = allocate_weighted_sizes(0, {1.0, 2.0, 3.0});
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == 0);
    CHECK(sizes[1] == 0);
    CHECK(sizes[2] == 0);
}
