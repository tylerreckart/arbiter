// Unit tests for layout snapshot JSON + LayoutTree capture/restore (#42).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/layout.h"
#include "repl/layout_snapshot.h"
#include "repl/pane.h"

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using arbiter::LayoutSnapshot;
using arbiter::LayoutTree;
using arbiter::Pane;
using arbiter::Rect;
using arbiter::for_each_layout_leaf;
using arbiter::layout_snapshot_from_json;
using arbiter::layout_snapshot_leaf_count;
using arbiter::layout_snapshot_path;
using arbiter::layout_snapshot_to_json;
using arbiter::load_layout_snapshot;
using arbiter::save_layout_snapshot;
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
