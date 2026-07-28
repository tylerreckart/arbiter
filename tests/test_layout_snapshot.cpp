// Unit tests for layout snapshot JSON + LayoutTree capture/restore (#42).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/layout.h"
#include "repl/layout_snapshot.h"
#include "repl/pane.h"
#include "repl/transcript_replay.h"
#include "styled_text.h"
#include "tui/opentui/pane_scroll_view.h"
#include "tui/tui.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

using arbiter::LayoutSnapshot;
using arbiter::LayoutTree;
using arbiter::Pane;
using arbiter::Rect;
using arbiter::StyleId;
using arbiter::StyledLine;
using arbiter::TUI;
using arbiter::claim_pane_transcript_replay;
using arbiter::for_each_layout_leaf;
using arbiter::layout_snapshot_from_json;
using arbiter::layout_snapshot_leaf_count;
using arbiter::layout_snapshot_path;
using arbiter::layout_snapshot_to_json;
using arbiter::load_layout_snapshot;
using arbiter::save_layout_snapshot;
using arbiter::styled_append;
using arbiter::validate_layout_snapshot;

namespace {

LayoutSnapshot make_vsplit_two(const std::string& a, const std::string& b) {
    LayoutSnapshot snap;
    snap.root.kind = LayoutSnapshot::Node::Kind::Split;
    snap.root.orient = LayoutTree::Orient::Vertical;
    snap.root.weight = 1.0;

    LayoutSnapshot::Node left;
    left.kind = LayoutSnapshot::Node::Kind::Leaf;
    left.conversation_id = a;
    left.agent = "index";
    left.weight = 1.0;

    LayoutSnapshot::Node right;
    right.kind = LayoutSnapshot::Node::Kind::Leaf;
    right.conversation_id = b;
    right.agent = "backend";
    right.weight = 2.0;

    snap.root.children.push_back(std::move(left));
    snap.root.children.push_back(std::move(right));
    snap.focused_leaf = 1;
    return snap;
}

std::unique_ptr<Pane> make_test_pane() {
    return std::make_unique<Pane>();
}

}  // namespace

TEST_CASE("layout_snapshot_path nests under conversations/") {
    CHECK(layout_snapshot_path("/tmp/arbiter-cfg") ==
          "/tmp/arbiter-cfg/conversations/layout.json");
}

TEST_CASE("json round-trip preserves tree, weights, focus, agents") {
    const auto snap = make_vsplit_two("conv-a", "conv-b");
    REQUIRE(validate_layout_snapshot(snap));

    const std::string json = layout_snapshot_to_json(snap);
    auto parsed = layout_snapshot_from_json(json);
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == LayoutSnapshot::kVersion);
    CHECK(parsed->focused_leaf == 1);
    CHECK(parsed->root.kind == LayoutSnapshot::Node::Kind::Split);
    CHECK(parsed->root.orient == LayoutTree::Orient::Vertical);
    REQUIRE(parsed->root.children.size() == 2);
    CHECK(parsed->root.children[0].conversation_id == "conv-a");
    CHECK(parsed->root.children[0].agent == "index");
    CHECK(parsed->root.children[1].conversation_id == "conv-b");
    CHECK(parsed->root.children[1].agent == "backend");
    CHECK(parsed->root.children[1].weight == doctest::Approx(2.0));
}

TEST_CASE("validate rejects oversized, empty-split, and bad focus") {
    auto snap = make_vsplit_two("a", "b");
    snap.focused_leaf = 99;
    CHECK_FALSE(validate_layout_snapshot(snap));

    snap = make_vsplit_two("a", "b");
    snap.root.children.clear();
    CHECK_FALSE(validate_layout_snapshot(snap));

    snap = make_vsplit_two("a", "b");
    // Build 9 leaves under a flat split.
    snap.root.children.clear();
    for (int i = 0; i < 9; ++i) {
        LayoutSnapshot::Node leaf;
        leaf.kind = LayoutSnapshot::Node::Kind::Leaf;
        leaf.conversation_id = "c" + std::to_string(i);
        snap.root.children.push_back(std::move(leaf));
    }
    CHECK(layout_snapshot_leaf_count(snap.root) == 9);
    CHECK_FALSE(validate_layout_snapshot(snap));
}

TEST_CASE("from_json rejects corrupt and unknown-version payloads") {
    CHECK_FALSE(layout_snapshot_from_json("").has_value());
    CHECK_FALSE(layout_snapshot_from_json("{}").has_value());
    CHECK_FALSE(layout_snapshot_from_json(
                    R"({"version":99,"focused_leaf":0,"root":{"type":"leaf","conversation_id":"x"}})")
                    .has_value());
    CHECK_FALSE(layout_snapshot_from_json(
                    R"({"version":1,"focused_leaf":0,"root":{"type":"split","orient":"diagonal","children":[]}})")
                    .has_value());
    CHECK_FALSE(layout_snapshot_from_json(
                    R"({"version":1,"focused_leaf":99,"root":{"type":"leaf","conversation_id":"x"}})")
                    .has_value());
}

TEST_CASE("save/load round-trip via atomic file") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("arbiter-layout-snap-" + std::to_string(::getpid()));
    fs::create_directories(dir / "conversations");
    const auto path = layout_snapshot_path(dir.string());

    const auto snap = make_vsplit_two("one", "two");
    REQUIRE(save_layout_snapshot(path, snap));
    auto loaded = load_layout_snapshot(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->focused_leaf == 1);
    CHECK(loaded->root.children[1].conversation_id == "two");

    fs::remove_all(dir);
}

TEST_CASE("for_each_layout_leaf walks pre-order") {
    auto snap = make_vsplit_two("a", "b");
    std::vector<std::string> ids;
    for_each_layout_leaf(snap.root, [&](LayoutSnapshot::Node& leaf) {
        ids.push_back(leaf.conversation_id);
    });
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "a");
    CHECK(ids[1] == "b");
}

TEST_CASE("LayoutTree capture/restore round-trips conversation bindings") {
    Rect bounds{0, 0, 120, 40};
    LayoutTree tree(make_test_pane(), bounds);
    tree.focused().conversation_id = "first";
    tree.focused().current_agent = "index";

    Pane* second = tree.split_focused(LayoutTree::Orient::Vertical, make_test_pane);
    REQUIRE(second != nullptr);
    second->conversation_id = "second";
    second->current_agent = "backend";
    REQUIRE(tree.focus_pane(second));

    const auto snap = tree.capture_snapshot();
    REQUIRE(validate_layout_snapshot(snap));
    CHECK(snap.focused_leaf == 1);
    CHECK(layout_snapshot_leaf_count(snap.root) == 2);

    LayoutTree restored(make_test_pane(), bounds);
    REQUIRE(restored.restore_snapshot(snap, make_test_pane, bounds));
    CHECK(restored.pane_count() == 2);

    std::vector<std::string> ids;
    std::vector<std::string> agents;
    restored.for_each_pane([&](Pane& p) {
        ids.push_back(p.conversation_id);
        agents.push_back(p.current_agent);
    });
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "first");
    CHECK(ids[1] == "second");
    CHECK(agents[1] == "backend");
    CHECK(restored.focused().conversation_id == "second");
}

TEST_CASE("LayoutTree restore preserves nested split orientation and weights") {
    Rect bounds{0, 0, 160, 48};
    LayoutTree tree(make_test_pane(), bounds);
    tree.focused().conversation_id = "a";

    Pane* right = tree.split_focused(LayoutTree::Orient::Vertical, make_test_pane);
    REQUIRE(right != nullptr);
    right->conversation_id = "b";
    right->current_agent = "reviewer";
    REQUIRE(tree.focus_pane(right));

    Pane* bottom = tree.split_focused(LayoutTree::Orient::Horizontal, make_test_pane);
    REQUIRE(bottom != nullptr);
    bottom->conversation_id = "c";
    bottom->current_agent = "frontend";

    // Skew the root vertical split via separator drag (index 0).
    auto sep = tree.hit_separator(bounds.w / 2, 1);
    REQUIRE(sep.has_value());
    REQUIRE(tree.drag_separator(*sep, bounds.w / 3, 1));

    const auto snap = tree.capture_snapshot();
    REQUIRE(validate_layout_snapshot(snap));
    CHECK(snap.root.kind == LayoutSnapshot::Node::Kind::Split);
    CHECK(snap.root.orient == LayoutTree::Orient::Vertical);
    REQUIRE(snap.root.children.size() == 2);
    CHECK(snap.root.children[1].kind == LayoutSnapshot::Node::Kind::Split);
    CHECK(snap.root.children[1].orient == LayoutTree::Orient::Horizontal);

    LayoutTree restored(make_test_pane(), bounds);
    REQUIRE(restored.restore_snapshot(snap, make_test_pane, bounds));
    CHECK(restored.pane_count() == 3);

    auto again = restored.capture_snapshot();
    CHECK(again.root.orient == LayoutTree::Orient::Vertical);
    REQUIRE(again.root.children.size() == 2);
    CHECK(again.root.children[0].weight == doctest::Approx(snap.root.children[0].weight));
    CHECK(again.root.children[1].weight == doctest::Approx(snap.root.children[1].weight));
    CHECK(again.root.children[1].orient == LayoutTree::Orient::Horizontal);

    std::vector<std::string> agents;
    restored.for_each_pane([&](Pane& p) { agents.push_back(p.current_agent); });
    REQUIRE(agents.size() == 3);
    CHECK(agents[1] == "reviewer");
    CHECK(agents[2] == "frontend");
}

TEST_CASE("LayoutTree restore reserves readline only on the focused leaf") {
    Rect bounds{0, 0, 120, 48};
    LayoutTree tree(make_test_pane(), bounds);
    tree.focused().conversation_id = "shared";
    tree.focused().current_agent = "index";

    Pane* right = tree.split_focused(LayoutTree::Orient::Vertical, make_test_pane);
    REQUIRE(right != nullptr);
    right->conversation_id = "shared";
    right->current_agent = "index";

    Pane* bottom = tree.split_focused(LayoutTree::Orient::Horizontal, make_test_pane);
    REQUIRE(bottom != nullptr);
    bottom->conversation_id = "shared";
    bottom->current_agent = "index";

    const auto snap = tree.capture_snapshot();
    LayoutTree restored(make_test_pane(), bounds);
    REQUIRE(restored.restore_snapshot(snap, make_test_pane, bounds));
    CHECK(restored.pane_count() == 3);

    int n = 0;
    restored.for_each_pane([&](Pane& p) {
        ++n;
        const bool focused = (&p == &restored.focused());
        if (focused) {
            CHECK(p.tui.input_rows() == arbiter::TUI::kDefaultInputRows);
        } else {
            CHECK(p.tui.input_rows() == 0);
        }
    });
    CHECK(n == 3);
}

TEST_CASE("focus toggle preserves scrolled viewport when readline appears") {
    Rect bounds{0, 0, 100, 40};
    LayoutTree tree(make_test_pane(), bounds);
    Pane* right = tree.split_focused(LayoutTree::Orient::Vertical, make_test_pane);
    REQUIRE(right != nullptr);

    Pane* left_ptr = nullptr;
    tree.for_each_pane([&](Pane& p) {
        if (!left_ptr) left_ptr = &p;
    });
    REQUIRE(left_ptr != nullptr);
    REQUIRE(left_ptr != right);
    REQUIRE(tree.focus_pane(right)); // right focused → left input_rows=0

    left_ptr->scroll = std::make_unique<arbiter::opentui::PaneScrollView>();
    left_ptr->scroll->bind(left_ptr->tui);
    for (int i = 0; i < 80; ++i) {
        StyledLine line;
        styled_append(line, StyleId::Default, "line-" + std::to_string(i));
        left_ptr->scroll->append_prose({line}, /*new_block=*/true);
    }
    left_ptr->scroll->bind(left_ptr->tui);
    const int max_off = left_ptr->scroll->max_scroll_offset();
    REQUIRE(max_off > TUI::kDefaultInputRows);
    left_ptr->scroll_offset = max_off / 2;
    const int offset_while_inactive = left_ptr->scroll_offset;
    CHECK(left_ptr->tui.input_rows() == 0);

    // Focus left → readline appears, viewport shrinks; offset should rise so
    // first_visible stays put.
    REQUIRE(tree.focus_pane(left_ptr));
    CHECK(left_ptr->tui.input_rows() == TUI::kDefaultInputRows);
    CHECK(left_ptr->scroll_offset == offset_while_inactive + TUI::kDefaultInputRows);

    const int offset_while_focused = left_ptr->scroll_offset;

    // Unfocus → readline disappears, viewport grows; offset should fall back.
    REQUIRE(tree.focus_pane(right));
    CHECK(left_ptr->tui.input_rows() == 0);
    CHECK(left_ptr->scroll_offset == offset_while_focused - TUI::kDefaultInputRows);
    CHECK(left_ptr->scroll_offset == offset_while_inactive);
}

TEST_CASE("live-tail scroll_offset stays zero across focus readline toggle") {
    Rect bounds{0, 0, 100, 40};
    LayoutTree tree(make_test_pane(), bounds);
    Pane* right = tree.split_focused(LayoutTree::Orient::Vertical, make_test_pane);
    REQUIRE(right != nullptr);

    Pane* left_ptr = nullptr;
    tree.for_each_pane([&](Pane& p) {
        if (!left_ptr) left_ptr = &p;
    });
    REQUIRE(tree.focus_pane(right));
    left_ptr->scroll = std::make_unique<arbiter::opentui::PaneScrollView>();
    left_ptr->scroll->bind(left_ptr->tui);
    for (int i = 0; i < 40; ++i) {
        StyledLine line;
        styled_append(line, StyleId::Default, "tail-" + std::to_string(i));
        left_ptr->scroll->append_prose({line}, /*new_block=*/true);
    }
    left_ptr->scroll_offset = 0;

    REQUIRE(tree.focus_pane(left_ptr));
    CHECK(left_ptr->scroll_offset == 0);
    REQUIRE(tree.focus_pane(right));
    CHECK(left_ptr->scroll_offset == 0);
}

TEST_CASE("stacked pane placement geometry after restore") {
    const Rect bounds{0, 0, 120, 40};
    LayoutTree live(make_test_pane(), bounds);
    live.focused().conversation_id = "shared-conv";
    live.focused().current_agent = "index";

    Pane* right = live.split_focused(LayoutTree::Orient::Vertical, make_test_pane);
    REQUIRE(right != nullptr);
    right->conversation_id = "shared-conv";
    right->current_agent = "index";
    REQUIRE(live.focus_pane(right));

    Pane* mid = live.split_focused(LayoutTree::Orient::Horizontal, make_test_pane);
    REQUIRE(mid != nullptr);
    mid->conversation_id = "shared-conv";
    mid->current_agent = "index";
    REQUIRE(live.focus_pane(mid));

    Pane* bottom = live.split_focused(LayoutTree::Orient::Horizontal, make_test_pane);
    REQUIRE(bottom != nullptr);
    bottom->conversation_id = "shared-conv";
    bottom->current_agent = "index";
    REQUIRE(live.focus_pane(bottom));

    const auto snap = live.capture_snapshot();
    REQUIRE(validate_layout_snapshot(snap));
    CHECK(layout_snapshot_leaf_count(snap.root) == 4);

    LayoutTree restored(make_test_pane(), bounds);
    REQUIRE(restored.restore_snapshot(snap, make_test_pane, bounds));

    struct PaneGeom {
        int index = 0;
        Rect rect{};
        int input_rows = 0;
        int bottom_pad = 0;
        bool outer_bottom = false;
        bool focused = false;
        bool has_content = false;
        bool polluted = false;
        std::string label;
    };

    std::unordered_set<std::string> claimed;
    std::vector<PaneGeom> panes;
    int i = 0;
    restored.for_each_pane([&](Pane& p) {
        const auto chrome = p.tui.chrome_snapshot();
        PaneGeom g;
        g.index = i++;
        g.rect = chrome.rect;
        g.input_rows = chrome.input_rows;
        g.bottom_pad = chrome.bottom_pad_rows;
        g.outer_bottom = chrome.outer_bottom;
        g.focused = (&p == &restored.focused());
        const std::string agent = p.current_agent.empty() ? "index" : p.current_agent;
        g.has_content = claim_pane_transcript_replay(
            claimed, p.conversation_id, agent);
        g.polluted = false;
        if (g.index == 0) g.label = "left / parent";
        else if (g.outer_bottom) g.label = "bottom-right (focused)";
        else if (g.index == 1) g.label = "top-right";
        else g.label = "mid-right";
        panes.push_back(g);
    });
    REQUIRE(panes.size() == 4);
    CHECK(claimed.size() == 1);

    for (const auto& p : panes) {
        CHECK(p.rect.h >= 8);
        CHECK(p.rect.w >= 24);
        if (p.focused) {
            CHECK(p.input_rows == TUI::kDefaultInputRows);
        } else {
            CHECK(p.input_rows == 0);
        }
    }

    CHECK(panes[0].rect.y == 0);
    CHECK(panes[0].rect.h == bounds.h);
    CHECK(panes[1].rect.x == panes[2].rect.x);
    CHECK(panes[2].rect.x == panes[3].rect.x);

    CHECK_FALSE(panes[1].outer_bottom);
    CHECK_FALSE(panes[2].outer_bottom);
    CHECK(panes[3].outer_bottom);
    CHECK(panes[1].bottom_pad == TUI::kCompactBottomPadRows);
    CHECK(panes[2].bottom_pad == TUI::kCompactBottomPadRows);
    CHECK(panes[3].bottom_pad == TUI::kBottomPadRows);
    CHECK(panes[1].bottom_pad == 0);
    CHECK(panes[2].bottom_pad == 0);

    CHECK(std::abs(panes[1].rect.h - panes[2].rect.h) <= 1);
    CHECK(std::abs(panes[2].rect.h - panes[3].rect.h) <= 1);
    CHECK(panes[2].rect.h >= 10);

    // Even stacked gutters: one separator cell between sibling rects.
    CHECK(panes[2].rect.y == panes[1].rect.y + panes[1].rect.h + 1);
    CHECK(panes[3].rect.y == panes[2].rect.y + panes[2].rect.h + 1);

    CHECK(panes[0].has_content);
    CHECK_FALSE(panes[1].has_content);
    CHECK_FALSE(panes[2].has_content);
    CHECK_FALSE(panes[3].has_content);
}
