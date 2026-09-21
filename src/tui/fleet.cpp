#include "tui/fleet.h"

#include "styled_text.h"
#include "tui/sidebar_format.h"

#include <algorithm>

namespace arbiter {

namespace {

std::string node_key(const FleetNode& n) {
    if (n.stream_id >= 0) return "s:" + std::to_string(n.stream_id);
    if (!n.clone_id.empty()) return "c:" + n.clone_id;
    return "u:" + n.agent;
}

int clamp_depth(int depth) {
    if (depth < 0) return 0;
    if (depth > 2) return 2;  // cap is 2; paint honestly
    return depth;
}

std::vector<std::string> string_array(const JsonValue& payload,
                                      const std::string& key) {
    std::vector<std::string> out;
    auto v = payload.get(key);
    if (!v || !v->is_array()) return out;
    for (const auto& item : v->as_array()) {
        if (!item) continue;
        if (item->is_string()) out.push_back(item->as_string());
        else if (item->is_object()) {
            const std::string id = item->get_string("id");
            if (!id.empty()) out.push_back(id);
        }
    }
    return out;
}

int array_len(const JsonValue& payload, const std::string& key) {
    auto v = payload.get(key);
    if (!v || !v->is_array()) return 0;
    return static_cast<int>(v->as_array().size());
}

std::string status_glyph(const FleetNode& n) {
    if (n.running) return "\u25CF";          // ●
    if (n.ended && n.ok) return "\u2713";    // ✓
    if (n.ended && !n.ok) return "\u2717";   // ✗
    return "\u25CB";                         // ○ pending
}

std::string tree_prefix(int depth, bool last_sibling, bool parent_last) {
    if (depth <= 0) return {};
    std::string p;
    if (depth >= 2) {
        p += parent_last ? "  " : "\u2502 ";  // │
    }
    p += last_sibling ? "\u2514 " : "\u251C ";  // └  ├
    return p;
}

}  // namespace

std::string format_fleet_row(const FleetNode& node,
                             bool last_sibling,
                             bool parent_last,
                             int max_cells) {
    if (max_cells < 4) max_cells = 4;
    std::string line = tree_prefix(node.depth, last_sibling, parent_last);
    line += status_glyph(node);
    line += " ";
    std::string name = node.agent.empty() ? "?" : node.agent;
    if (node.ephemeral) name += " \u25C7";  // ◇ JIT marker
    line += name;

    std::string tail;
    if (!node.active_tool.empty() && node.running) {
        tail = node.active_tool;
    } else {
        const int tok = node.input_tokens + node.output_tokens;
        if (tok > 0) tail = format_token_count(tok);
    }
    if (!tail.empty()) {
        const int used = static_cast<int>(display_width(line));
        const int gap = 1;
        const int tail_w = static_cast<int>(display_width(tail));
        int pad = max_cells - used - gap - tail_w;
        if (pad < 1) {
            // Trim the name so the tail still fits.
            const int name_budget = std::max(1, max_cells - tail_w - gap
                - static_cast<int>(display_width(tree_prefix(node.depth,
                                                             last_sibling,
                                                             parent_last)
                                                 + status_glyph(node) + " ")));
            name = trim_to_display_cols(name, name_budget);
            line = tree_prefix(node.depth, last_sibling, parent_last)
                 + status_glyph(node) + " " + name;
            pad = max_cells - static_cast<int>(display_width(line))
                - gap - tail_w;
        }
        if (pad < 1) pad = 1;
        line.append(static_cast<size_t>(pad), ' ');
        line += tail;
    }
    return trim_to_display_cols(line, max_cells);
}

std::vector<std::string> format_fleet_overlay(const FleetReconcileOverlay& ov,
                                              int max_cells) {
    std::vector<std::string> lines;
    if (!ov.active) return lines;
    if (max_cells < 4) max_cells = 4;

    std::string head;
    if (!ov.mode.empty()) head = ov.mode;
    else head = "reconcile";
    if (ov.wave > 0) {
        if (!head.empty()) head += " \u00b7 ";
        head += "wave " + std::to_string(ov.wave);
    } else if (!ov.phase.empty()) {
        if (!head.empty()) head += " \u00b7 ";
        head += ov.phase;
    }
    lines.push_back(trim_to_display_cols(head, max_cells));

    std::string delta = "\u0394 " + std::to_string(ov.residual) + " residual";
    delta += " \u00b7 " + std::to_string(ov.held) + " held";
    lines.push_back(trim_to_display_cols(delta, max_cells));
    return lines;
}

void FleetTree::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    nodes_.clear();
    overlay_ = {};
    request_id_.clear();
}

void FleetTree::on_request_received(const std::string& request_id,
                                    const std::string& agent) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!request_id.empty() && request_id != request_id_) {
        nodes_.clear();
        overlay_ = {};
    }
    if (!request_id.empty()) request_id_ = request_id;
    if (agent == "reconcile") {
        overlay_.active = true;
        overlay_.request_id = request_id_;
        if (overlay_.mode.empty()) overlay_.mode = "ensure";
    }
}

FleetNode* FleetTree::find_stream_locked(int stream_id) {
    if (stream_id < 0) return nullptr;
    for (auto& n : nodes_) {
        if (n.stream_id == stream_id) return &n;
    }
    return nullptr;
}

FleetNode* FleetTree::find_clone_locked(const std::string& clone_id) {
    if (clone_id.empty()) return nullptr;
    for (auto& n : nodes_) {
        if (n.clone_id == clone_id) return &n;
    }
    return nullptr;
}

int FleetTree::infer_parent_locked(int depth) const {
    if (depth <= 0) return -1;
    const int want = depth - 1;
    int fallback = -1;
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        if (it->depth != want) continue;
        if (it->stream_id < 0) continue;
        if (it->running || !it->ended) return it->stream_id;
        if (fallback < 0) fallback = it->stream_id;
    }
    return fallback;
}

FleetNode* FleetTree::ensure_stream_locked(int stream_id) {
    if (auto* n = find_stream_locked(stream_id)) return n;
    FleetNode n;
    n.stream_id = stream_id;
    n.request_id = request_id_;
    nodes_.push_back(std::move(n));
    return &nodes_.back();
}

void FleetTree::attach_stream_to_clone_locked(int stream_id,
                                              const std::string& agent) {
    if (stream_id < 0 || agent.empty()) return;
    if (find_stream_locked(stream_id)) return;
    // Prefer an open JIT clone with this agent that has no stream yet.
    for (auto& n : nodes_) {
        if (!n.ephemeral) continue;
        if (n.stream_id >= 0) continue;
        if (n.agent != agent) continue;
        if (n.ended) continue;
        n.stream_id = stream_id;
        return;
    }
}

void FleetTree::on_stream_start(const std::string& agent,
                                int stream_id,
                                int depth,
                                const std::string& request_id,
                                const std::string& conversation_id,
                                const std::string& pane_agent) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!request_id.empty() && request_id != request_id_) {
        nodes_.clear();
        overlay_ = {};
        request_id_ = request_id;
    } else if (!request_id.empty()) {
        request_id_ = request_id;
    }
    depth = clamp_depth(depth);
    attach_stream_to_clone_locked(stream_id, agent);
    auto* n = ensure_stream_locked(stream_id);
    n->agent = agent;
    n->depth = depth;
    n->running = true;
    n->ended = false;
    n->ok = true;
    n->active_tool.clear();
    if (n->parent_stream_id < 0) n->parent_stream_id = infer_parent_locked(depth);
    if (!request_id_.empty()) n->request_id = request_id_;
    if (!conversation_id.empty()) n->conversation_id = conversation_id;
    if (!pane_agent.empty()) n->pane_agent = pane_agent;
}

void FleetTree::on_agent_start(const std::string& agent,
                               int stream_id,
                               int depth) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* n = find_stream_locked(stream_id);
    if (!n) {
        FleetNode fresh;
        fresh.stream_id = stream_id;
        fresh.agent = agent;
        fresh.depth = clamp_depth(depth);
        fresh.running = true;
        fresh.parent_stream_id = infer_parent_locked(fresh.depth);
        fresh.request_id = request_id_;
        nodes_.push_back(std::move(fresh));
        return;
    }
    if (!agent.empty()) n->agent = agent;
    n->running = true;
    n->ended = false;
}

void FleetTree::on_text(int stream_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto* n = find_stream_locked(stream_id)) {
        n->running = true;
        n->ended = false;
    }
}

void FleetTree::on_tool_call(int stream_id, const std::string& tool, bool started) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* n = find_stream_locked(stream_id);
    if (!n) return;
    if (started) {
        n->active_tool = tool;
        n->running = true;
        n->ended = false;
    } else if (n->active_tool == tool || n->active_tool.empty()) {
        n->active_tool.clear();
    }
}

void FleetTree::on_token_usage(int stream_id, int input_tokens, int output_tokens) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* n = find_stream_locked(stream_id);
    if (!n) return;
    if (input_tokens > 0) n->input_tokens += input_tokens;
    if (output_tokens > 0) n->output_tokens += output_tokens;
}

void FleetTree::on_stream_end(int stream_id, bool ok) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* n = find_stream_locked(stream_id);
    if (!n) return;
    n->running = false;
    n->ended = true;
    n->ok = ok;
    n->active_tool.clear();
}

void FleetTree::on_agent_spawned(const std::string& agent,
                                 const std::string& clone_id,
                                 const std::vector<std::string>& clauses,
                                 const std::string& request_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!request_id.empty() && request_id != request_id_) {
        nodes_.clear();
        overlay_ = {};
        request_id_ = request_id;
    } else if (!request_id.empty()) {
        request_id_ = request_id;
    }
    overlay_.active = true;
    if (overlay_.mode.empty()) overlay_.mode = "ensure";
    overlay_.request_id = request_id_;

    if (auto* existing = find_clone_locked(clone_id)) {
        existing->agent = agent;
        existing->ephemeral = true;
        existing->running = true;
        existing->ended = false;
        existing->clauses = clauses;
        return;
    }
    FleetNode n;
    n.clone_id = clone_id;
    n.agent = agent;
    n.depth = 1;
    n.ephemeral = true;
    n.running = true;
    n.clauses = clauses;
    n.request_id = request_id_;
    n.parent_stream_id = infer_parent_locked(1);
    nodes_.push_back(std::move(n));
}

void FleetTree::on_agent_teardown(const std::string& clone_id) {
    std::lock_guard<std::mutex> lk(mu_);
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [&](const FleetNode& n) {
                                    return !clone_id.empty()
                                        && n.clone_id == clone_id;
                                }),
                 nodes_.end());
}

void FleetTree::on_reconcile_delta(int residual, int held, bool empty, int wave,
                                   const std::string& request_id) {
    std::lock_guard<std::mutex> lk(mu_);
    overlay_.active = true;
    overlay_.residual = residual;
    overlay_.held = held;
    overlay_.empty = empty;
    overlay_.wave = wave;
    if (!request_id.empty()) {
        request_id_ = request_id;
        overlay_.request_id = request_id;
    }
    if (overlay_.mode.empty()) overlay_.mode = "ensure";
}

void FleetTree::on_reconcile_progress(const std::string& phase,
                                      const std::string& /*detail*/,
                                      const std::string& request_id) {
    std::lock_guard<std::mutex> lk(mu_);
    overlay_.active = true;
    overlay_.phase = phase;
    if (!request_id.empty()) {
        request_id_ = request_id;
        overlay_.request_id = request_id;
    }
    if (phase == "observe") overlay_.mode = "observe";
    else if (phase == "ensure" || phase == "wave" || phase == "implement") {
        overlay_.mode = "ensure";
    }
}

void FleetTree::on_done() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& n : nodes_) {
        if (!n.ended) {
            n.running = false;
            n.ended = true;
        }
        n.active_tool.clear();
    }
}

void FleetTree::bind_host(int stream_id,
                          const std::string& conversation_id,
                          const std::string& pane_agent) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* n = find_stream_locked(stream_id);
    if (!n) return;
    if (!conversation_id.empty()) n->conversation_id = conversation_id;
    if (!pane_agent.empty()) n->pane_agent = pane_agent;
}

void FleetTree::ingest_sse(const std::string& event, const std::string& data) {
    if (data.empty()) {
        ingest_sse(event, JsonValue{});
        return;
    }
    try {
        auto parsed = json_parse(data);
        if (parsed) ingest_sse(event, *parsed);
        else ingest_sse(event, JsonValue{});
    } catch (...) {
        ingest_sse(event, JsonValue{});
    }
}

void FleetTree::ingest_sse(const std::string& event, const JsonValue& payload) {
    const std::string rid = payload.is_object()
        ? payload.get_string("request_id") : std::string{};
    const std::string agent = payload.is_object()
        ? payload.get_string("agent") : std::string{};
    const int sid = payload.is_object()
        ? payload.get_int("stream_id", -1) : -1;
    const int depth = payload.is_object()
        ? payload.get_int("depth", 0) : 0;

    if (event == "request_received") {
        on_request_received(rid, agent);
        return;
    }
    if (event == "stream_start") {
        on_stream_start(agent, sid, depth, rid);
        return;
    }
    if (event == "agent_start") {
        on_agent_start(agent, sid, depth);
        return;
    }
    if (event == "text") {
        if (sid >= 0) on_text(sid);
        return;
    }
    if (event == "tool_call") {
        std::string tool = payload.get_string("tool");
        if (tool.empty()) tool = payload.get_string("label");
        // SSE tool_call is Finished; Started is TUI-only.
        if (sid >= 0) on_tool_call(sid, tool, /*started=*/false);
        return;
    }
    if (event == "token_usage") {
        on_token_usage(sid,
                       payload.get_int("input_tokens"),
                       payload.get_int("output_tokens"));
        return;
    }
    if (event == "stream_end") {
        on_stream_end(sid, payload.get_bool("ok", true));
        return;
    }
    if (event == "agent.spawned") {
        on_agent_spawned(agent, payload.get_string("clone_id"),
                         string_array(payload, "clauses"), rid);
        return;
    }
    if (event == "agent.teardown") {
        on_agent_teardown(payload.get_string("clone_id"));
        return;
    }
    if (event == "reconcile.delta") {
        on_reconcile_delta(array_len(payload, "residual"),
                           array_len(payload, "held"),
                           payload.get_bool("empty", false),
                           payload.get_int("wave", 0),
                           rid);
        return;
    }
    if (event == "reconcile.progress") {
        on_reconcile_progress(payload.get_string("phase"),
                              payload.get_string("detail"),
                              rid);
        return;
    }
    if (event == "done" || event == "reconcile.done") {
        on_done();
        return;
    }
}

std::vector<FleetNode> FleetTree::nodes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return nodes_;
}

FleetReconcileOverlay FleetTree::overlay() const {
    std::lock_guard<std::mutex> lk(mu_);
    return overlay_;
}

std::string FleetTree::request_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return request_id_;
}

bool FleetTree::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return nodes_.empty() && !overlay_.active;
}

bool FleetTree::has_live() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& n : nodes_) {
        if (n.running && !n.ended) return true;
    }
    return overlay_.active && !overlay_.empty && overlay_.mode == "ensure";
}

int FleetTree::total_input() const {
    std::lock_guard<std::mutex> lk(mu_);
    int t = 0;
    for (const auto& n : nodes_) t += n.input_tokens;
    return t;
}

int FleetTree::total_output() const {
    std::lock_guard<std::mutex> lk(mu_);
    int t = 0;
    for (const auto& n : nodes_) t += n.output_tokens;
    return t;
}

std::optional<FleetNode> FleetTree::find_stream(int stream_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& n : nodes_) {
        if (n.stream_id == stream_id) return n;
    }
    return std::nullopt;
}

std::optional<FleetNode> FleetTree::find_clone(const std::string& clone_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& n : nodes_) {
        if (n.clone_id == clone_id) return n;
    }
    return std::nullopt;
}

std::vector<FleetPaintRow> FleetTree::paint_rows(int max_cells) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<FleetPaintRow> out;
    if (nodes_.empty()) return out;

    // Pre-order: depth 0, then its children, then grandchildren.
    std::vector<int> order;
    order.reserve(nodes_.size());
    auto append_children = [&](auto&& self, int parent_sid, int parent_depth) -> void {
        std::vector<int> kids;
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
            const auto& n = nodes_[static_cast<size_t>(i)];
            if (n.depth != parent_depth + 1) continue;
            if (parent_sid >= 0 && n.parent_stream_id >= 0
                && n.parent_stream_id != parent_sid) {
                continue;
            }
            if (parent_sid < 0 && n.parent_stream_id >= 0) continue;
            kids.push_back(i);
        }
        for (int i = 0; i < static_cast<int>(kids.size()); ++i) {
            order.push_back(kids[static_cast<size_t>(i)]);
            const auto& kid = nodes_[static_cast<size_t>(kids[static_cast<size_t>(i)])];
            if (kid.stream_id >= 0) self(self, kid.stream_id, kid.depth);
            else self(self, -1, kid.depth);
        }
    };

    std::vector<int> roots;
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
        if (nodes_[static_cast<size_t>(i)].depth == 0) roots.push_back(i);
    }
    if (roots.empty()) {
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
            if (nodes_[static_cast<size_t>(i)].parent_stream_id < 0)
                roots.push_back(i);
        }
    }
    if (roots.empty()) {
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
            roots.push_back(i);
    }

    for (int r : roots) {
        order.push_back(r);
        const auto& root = nodes_[static_cast<size_t>(r)];
        append_children(append_children, root.stream_id, root.depth);
    }

    // Dedup in case a node matched both root and child walks.
    std::vector<int> seen(nodes_.size(), 0);
    std::vector<int> unique;
    unique.reserve(order.size());
    for (int i : order) {
        if (i < 0 || i >= static_cast<int>(nodes_.size())) continue;
        if (seen[static_cast<size_t>(i)]) continue;
        seen[static_cast<size_t>(i)] = 1;
        unique.push_back(i);
    }
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
        if (!seen[static_cast<size_t>(i)]) unique.push_back(i);
    }

    auto last_sibling = [&](size_t idx_in_unique) -> bool {
        if (idx_in_unique + 1 >= unique.size()) return true;
        const int cur_i = unique[idx_in_unique];
        const int nxt_i = unique[idx_in_unique + 1];
        return nodes_[static_cast<size_t>(nxt_i)].depth
            <= nodes_[static_cast<size_t>(cur_i)].depth;
    };
    auto parent_last = [&](size_t idx_in_unique) -> bool {
        const int cur_depth = nodes_[static_cast<size_t>(unique[idx_in_unique])].depth;
        if (cur_depth < 2) return true;
        // Walk back to the depth-1 ancestor in the flattened order.
        for (size_t j = idx_in_unique; j > 0; --j) {
            const auto& cand = nodes_[static_cast<size_t>(unique[j - 1])];
            if (cand.depth == 1) {
                // Is there another depth-1 after this ancestor?
                for (size_t k = j; k < unique.size(); ++k) {
                    if (nodes_[static_cast<size_t>(unique[k])].depth == 1
                        && k != j - 1) {
                        // later sibling of the parent exists if it appears after
                        // this subtree — approximate: parent is last if no
                        // other depth-1 follows this node at the same indent.
                    }
                }
                return last_sibling(j - 1);
            }
            if (cand.depth == 0) break;
        }
        return true;
    };

    out.reserve(unique.size());
    for (size_t u = 0; u < unique.size(); ++u) {
        const auto& n = nodes_[static_cast<size_t>(unique[u])];
        FleetPaintRow row;
        row.key = node_key(n);
        row.stream_id = n.stream_id;
        row.clone_id = n.clone_id;
        row.agent = n.agent;
        row.depth = n.depth;
        row.ephemeral = n.ephemeral;
        row.running = n.running;
        row.ended = n.ended;
        row.ok = n.ok;
        row.conversation_id = n.conversation_id;
        row.pane_agent = n.pane_agent.empty() ? n.agent : n.pane_agent;
        row.line = format_fleet_row(n, last_sibling(u), parent_last(u), max_cells);
        out.push_back(std::move(row));
    }
    return out;
}

void FleetSidebarState::toggle_visible() {
    std::lock_guard<std::mutex> lk(mu_);
    user_visible_ = !user_visible_;
    if (!user_visible_) focused_ = false;
}

void FleetSidebarState::set_visible(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    user_visible_ = on;
    if (!user_visible_) focused_ = false;
}

bool FleetSidebarState::user_visible() const {
    std::lock_guard<std::mutex> lk(mu_);
    return user_visible_;
}

void FleetSidebarState::enter_focus() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!user_visible_) user_visible_ = true;
    focused_ = true;
}

void FleetSidebarState::exit_focus() {
    std::lock_guard<std::mutex> lk(mu_);
    focused_ = false;
}

bool FleetSidebarState::focused() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focused_;
}

int FleetSidebarState::effective_width(int cols, int leading_cols,
                                       bool has_content) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!user_visible_ && !focused_) return 0;
    if (!has_content && !focused_ && !user_visible_) return 0;
    // Hidden unless there is a job or the user forced the pane on.
    if (!has_content && !focused_) {
        // user_visible defaults true — still require content so an idle
        // session does not lose conversation columns to an empty Fleet box.
        return 0;
    }
    const int available = cols - std::max(0, leading_cols);
    return breakpoint_width(available);
}

Rect FleetSidebarState::rect_for_terminal(int cols, int rows, int leading_cols,
                                          bool has_content) const {
    const int w = effective_width(cols, leading_cols, has_content);
    if (w <= 0 || cols <= w + kOuterGutter || rows <= 0) return kEmptyRect;
    return Rect{cols - w - kOuterGutter, 0, w, rows};
}

void FleetSidebarState::clamp_scroll_locked(int idx, int visible_rows,
                                            int row_count) {
    if (visible_rows < 1) visible_rows = 1;
    last_visible_rows_ = visible_rows;
    if (row_count <= 0) {
        selected_ = 0;
        scroll_offset_ = 0;
        return;
    }
    if (idx < 0) idx = 0;
    if (idx >= row_count) idx = row_count - 1;
    selected_ = idx;
    if (selected_ < scroll_offset_) scroll_offset_ = selected_;
    if (selected_ >= scroll_offset_ + visible_rows) {
        scroll_offset_ = selected_ - visible_rows + 1;
    }
    if (scroll_offset_ < 0) scroll_offset_ = 0;
}

void FleetSidebarState::move_selection(int delta, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int count = std::max(last_row_count_, selected_ + 1);
    clamp_scroll_locked(selected_ + delta, visible_rows, count);
}

void FleetSidebarState::page_selection(int direction, int visible_rows) {
    const int step = std::max(1, visible_rows - 1);
    move_selection(direction < 0 ? -step : step, visible_rows);
}

void FleetSidebarState::select_at_index(int index, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int count = std::max({last_row_count_, index + 1, 1});
    clamp_scroll_locked(index, visible_rows, count);
}

int FleetSidebarState::selected_index() const {
    std::lock_guard<std::mutex> lk(mu_);
    return selected_;
}

int FleetSidebarState::scroll_offset() const {
    std::lock_guard<std::mutex> lk(mu_);
    return scroll_offset_;
}

FleetKey FleetSidebarState::handle_key(int key_byte, char csi_final,
                                       const std::string& csi_params) {
    if (key_byte == 'j' || key_byte == 'J') return FleetKey::Down;
    if (key_byte == 'k' || key_byte == 'K') return FleetKey::Up;
    if (key_byte == '\r' || key_byte == '\n') return FleetKey::Enter;
    if (key_byte == 0x1B) {
        if (csi_final == 'A') return FleetKey::Up;
        if (csi_final == 'B') return FleetKey::Down;
        if (csi_final == '~' && csi_params == "5") return FleetKey::PageUp;
        if (csi_final == '~' && csi_params == "6") return FleetKey::PageDown;
        if (csi_final == 0 && csi_params.empty()) return FleetKey::Escape;
        return FleetKey::None;
    }
    return FleetKey::None;
}

FleetSnapshot FleetSidebarState::snapshot(const FleetTree& tree,
                                          int cols, int leading_cols) const {
    FleetSnapshot snap;
    const auto overlay = tree.overlay();
    const bool has_content = !tree.empty();
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap.focused = focused_;
        snap.selected = selected_;
        snap.scroll_offset = scroll_offset_;
        snap.visible = user_visible_ && (has_content || focused_);
        if (!user_visible_ && !focused_) snap.visible = false;
    }
    snap.request_id = tree.request_id();
    snap.overlay = overlay;
    snap.total_input = tree.total_input();
    snap.total_output = tree.total_output();
    snap.has_live = tree.has_live();

    const int w = effective_width(cols, leading_cols, has_content || snap.focused);
    const int cells = std::max(8, w - 4);
    auto ov_lines = format_fleet_overlay(overlay, cells);
    snap.overlay_lines = static_cast<int>(ov_lines.size());
    snap.rows = tree.paint_rows(cells);

    const int row_count = static_cast<int>(snap.rows.size());
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_row_count_ = row_count;
        if (row_count <= 0) {
            selected_ = 0;
            scroll_offset_ = 0;
            snap.selected = 0;
            snap.scroll_offset = 0;
        } else {
            if (selected_ >= row_count) selected_ = row_count - 1;
            if (selected_ < 0) selected_ = 0;
            snap.selected = selected_;
            snap.scroll_offset = scroll_offset_;
        }
    }
    return snap;
}

}  // namespace arbiter
