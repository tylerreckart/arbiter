// arbiter/src/repl/layout_snapshot.cpp

#include "repl/layout_snapshot.h"

#include "atomic_file.h"
#include "json.h"

#include <fstream>
#include <sstream>

namespace arbiter {

namespace {

std::string orient_to_string(LayoutTree::Orient o) {
    return o == LayoutTree::Orient::Horizontal ? "horizontal" : "vertical";
}

std::optional<LayoutTree::Orient> orient_from_string(std::string_view s) {
    if (s == "horizontal" || s == "h") return LayoutTree::Orient::Horizontal;
    if (s == "vertical" || s == "v") return LayoutTree::Orient::Vertical;
    return std::nullopt;
}

size_t leaf_count_(const LayoutSnapshot::Node& node) {
    if (node.kind == LayoutSnapshot::Node::Kind::Leaf) return 1;
    size_t n = 0;
    for (const auto& c : node.children) n += leaf_count_(c);
    return n;
}

bool validate_node_(const LayoutSnapshot::Node& node) {
    if (node.weight <= 0.0) return false;
    if (node.kind == LayoutSnapshot::Node::Kind::Leaf) {
        return node.children.empty();
    }
    if (node.children.size() < 2) return false;
    for (const auto& c : node.children) {
        if (!validate_node_(c)) return false;
    }
    return true;
}

std::shared_ptr<JsonValue> node_to_json_(const LayoutSnapshot::Node& node) {
    JsonObject o;
    if (node.kind == LayoutSnapshot::Node::Kind::Leaf) {
        o["type"] = jstr("leaf");
        o["weight"] = jnum(node.weight);
        o["conversation_id"] = jstr(node.conversation_id);
        o["agent"] = jstr(node.agent.empty() ? "index" : node.agent);
        return jobj(std::move(o));
    }
    o["type"] = jstr("split");
    o["orient"] = jstr(orient_to_string(node.orient));
    o["weight"] = jnum(node.weight);
    JsonArray kids;
    kids.reserve(node.children.size());
    for (const auto& c : node.children) {
        kids.push_back(node_to_json_(c));
    }
    o["children"] = jarr(std::move(kids));
    return jobj(std::move(o));
}

std::optional<LayoutSnapshot::Node>
node_from_json_(const std::shared_ptr<JsonValue>& v) {
    if (!v || !v->is_object()) return std::nullopt;
    const std::string type = v->get_string("type");
    LayoutSnapshot::Node node;
    node.weight = v->get_number("weight", 1.0);
    if (node.weight <= 0.0) node.weight = 1.0;

    if (type == "leaf") {
        node.kind = LayoutSnapshot::Node::Kind::Leaf;
        node.conversation_id = v->get_string("conversation_id");
        node.agent = v->get_string("agent", "index");
        if (node.agent.empty()) node.agent = "index";
        return node;
    }
    if (type == "split") {
        node.kind = LayoutSnapshot::Node::Kind::Split;
        auto orient = orient_from_string(v->get_string("orient", "vertical"));
        if (!orient) return std::nullopt;
        node.orient = *orient;
        auto kids_v = v->get("children");
        if (!kids_v || !kids_v->is_array()) return std::nullopt;
        for (const auto& child_v : kids_v->as_array()) {
            auto child = node_from_json_(child_v);
            if (!child) return std::nullopt;
            node.children.push_back(std::move(*child));
        }
        if (node.children.size() < 2) return std::nullopt;
        return node;
    }
    return std::nullopt;
}

}  // namespace

size_t layout_snapshot_leaf_count(const LayoutSnapshot::Node& node) {
    return leaf_count_(node);
}

bool validate_layout_snapshot(const LayoutSnapshot& snap, size_t max_leaves) {
    if (snap.version != LayoutSnapshot::kVersion) return false;
    if (!validate_node_(snap.root)) return false;
    const size_t leaves = leaf_count_(snap.root);
    if (leaves == 0 || leaves > max_leaves) return false;
    if (snap.focused_leaf >= leaves) return false;
    return true;
}

std::string layout_snapshot_to_json(const LayoutSnapshot& snap) {
    JsonObject root;
    root["version"] = jnum(static_cast<double>(snap.version));
    root["focused_leaf"] = jnum(static_cast<double>(snap.focused_leaf));
    root["root"] = node_to_json_(snap.root);
    return json_serialize(*jobj(std::move(root)));
}

std::optional<LayoutSnapshot>
layout_snapshot_from_json(std::string_view json) {
    // Fail open on corrupt / empty input — callers keep a single pane.
    std::shared_ptr<JsonValue> parsed;
    try {
        parsed = json_parse(json);
    } catch (...) {
        return std::nullopt;
    }
    if (!parsed || !parsed->is_object()) return std::nullopt;
    LayoutSnapshot snap;
    snap.version = parsed->get_int("version", 0);
    if (snap.version != LayoutSnapshot::kVersion) return std::nullopt;
    const int focused = parsed->get_int("focused_leaf", 0);
    if (focused < 0 ||
        focused > static_cast<int>(kMaxLayoutSnapshotLeaves)) {
        return std::nullopt;
    }
    snap.focused_leaf = static_cast<size_t>(focused);
    auto root_v = parsed->get("root");
    auto root = node_from_json_(root_v);
    if (!root) return std::nullopt;
    snap.root = std::move(*root);
    if (!validate_layout_snapshot(snap)) return std::nullopt;
    return snap;
}

std::string layout_snapshot_path(const std::string& config_dir) {
    return config_dir + "/conversations/layout.json";
}

bool save_layout_snapshot(const std::string& path, const LayoutSnapshot& snap) {
    if (!validate_layout_snapshot(snap)) return false;
    return atomic_write_file(path, layout_snapshot_to_json(snap));
}

std::optional<LayoutSnapshot> load_layout_snapshot(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return layout_snapshot_from_json(ss.str());
}

void for_each_layout_leaf(LayoutSnapshot::Node& node,
                          const std::function<void(LayoutSnapshot::Node&)>& fn) {
    if (node.kind == LayoutSnapshot::Node::Kind::Leaf) {
        fn(node);
        return;
    }
    for (auto& c : node.children) for_each_layout_leaf(c, fn);
}

}  // namespace arbiter
