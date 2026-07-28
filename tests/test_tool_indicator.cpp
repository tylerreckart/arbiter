#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/tui.h"
#include "tui/tui_design.h"

using namespace arbiter;

TEST_CASE("outer-bottom panes reserve footer pad; stacked panes stay compact") {
    TUI bottom;
    bottom.set_rect(Rect{0, 20, 80, 20});
    bottom.set_outer_bottom(true);
    bottom.set_footer_hint_mode(FooterHintMode::Hidden);
    CHECK(bottom.bottom_pad_rows() == TUI::kBottomPadRows);
    CHECK(bottom.chrome_snapshot().outer_bottom);
    CHECK(bottom.chrome_snapshot().bottom_pad_rows == TUI::kBottomPadRows);

    TUI stacked;
    stacked.set_rect(Rect{0, 0, 80, 20});
    stacked.set_outer_bottom(false);
    stacked.set_outer_top(false);
    stacked.set_footer_hint_mode(FooterHintMode::Compact);
    CHECK(stacked.bottom_pad_rows() == TUI::kCompactBottomPadRows);
    CHECK(stacked.bottom_pad_rows() == 0);
    CHECK_FALSE(stacked.chrome_snapshot().outer_bottom);
    CHECK_FALSE(stacked.chrome_snapshot().outer_top);
    CHECK(stacked.chrome_snapshot().bottom_pad_rows == TUI::kCompactBottomPadRows);

    CHECK(tui_outer_bottom_pad_rows(tui_design()) == TUI::kBottomPadRows);
}

TEST_CASE("inactive panes are content-only; focused panes reserve readline") {
    TUI fresh;
    fresh.set_rect(Rect{0, 0, 80, 24});
    CHECK(fresh.input_rows() == 0);

    fresh.begin_input();
    CHECK(fresh.input_rows() == TUI::kDefaultInputRows);

    fresh.set_input_rows(0);
    CHECK(fresh.input_rows() == 0);
    // No input band → scroll extends to the trailing pad.
    const int bottom_pad = fresh.bottom_pad_rows();
    CHECK(fresh.chrome_snapshot().input_rows == 0);
    CHECK(bottom_pad >= TUI::kCompactBottomPadRows);
}

TEST_CASE("ToolCallIndicator begin arms and bump counts Finished tools") {
    ToolCallIndicator ind(nullptr);
    CHECK(ind.total() == 0);
    CHECK(ind.failed() == 0);

    // bump before begin is a no-op (turn not armed).
    ind.bump("fetch", true);
    CHECK(ind.total() == 0);

    ind.begin();
    ind.bump("fetch", true);
    ind.bump("exec", false);
    ind.bump("help", true);
    CHECK(ind.total() == 3);
    CHECK(ind.failed() == 1);

    // begin() resets for the next turn.
    ind.begin();
    CHECK(ind.total() == 0);
    CHECK(ind.failed() == 0);
    ind.bump("write", true);
    CHECK(ind.total() == 1);
    CHECK(ind.failed() == 0);

    const std::string summary = ind.finalize();
    CHECK(summary.find("1 tool") != std::string::npos);
    // finalize disarms — further bumps ignored until begin.
    ind.bump("fetch", true);
    CHECK(ind.total() == 1);
}
