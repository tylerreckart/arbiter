#include "tui/tui_design.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace arbiter {

namespace {

TuiDesign g_design;
bool g_design_ready = false;

TuiDesign modern_design();

int clamp_byte(int v) {
    return std::max(0, std::min(255, v));
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool parse_hex_color(const std::string& s, TuiRgba& out) {
    if (s.size() != 7 || s[0] != '#') return false;
    int vals[6];
    for (int i = 0; i < 6; ++i) {
        vals[i] = hex_digit(s[static_cast<size_t>(i + 1)]);
        if (vals[i] < 0) return false;
    }
    out = tui_rgba(static_cast<std::uint8_t>(vals[0] * 16 + vals[1]),
                   static_cast<std::uint8_t>(vals[2] * 16 + vals[3]),
                   static_cast<std::uint8_t>(vals[4] * 16 + vals[5]));
    return true;
}

std::shared_ptr<JsonValue> child(const JsonValue& root,
                                 const char* group,
                                 const char* key) {
    auto g = root.get(group);
    if (!g || !g->is_object()) return nullptr;
    return g->get(key);
}

void color(const JsonValue& root, const char* group, const char* key, TuiRgba& out) {
    auto v = child(root, group, key);
    if (v && v->is_string()) {
        TuiRgba parsed = out;
        if (parse_hex_color(v->as_string(), parsed)) out = parsed;
    }
}

void number(const JsonValue& root, const char* group, const char* key, int& out) {
    auto v = child(root, group, key);
    if (v && v->is_number()) out = static_cast<int>(v->as_number());
}

void boolean(const JsonValue& root, const char* group, const char* key, bool& out) {
    auto v = child(root, group, key);
    if (v && v->is_bool()) out = v->as_bool();
}

void string_value(const JsonValue& root, const char* group, const char* key, std::string& out) {
    auto v = child(root, group, key);
    if (v && v->is_string()) out = v->as_string();
}

TuiDesign modern_design() {
    TuiDesign d;

    d.bg.base   = tui_rgba(0x08, 0x08, 0x08);
    d.bg.panel  = tui_rgba(0x10, 0x10, 0x10);
    d.bg.header = tui_rgba(0x1a, 0x1a, 0x1a);
    d.bg.scroll = tui_rgba(0x0c, 0x0c, 0x0c);
    d.bg.status = tui_rgba(0x15, 0x15, 0x15);
    d.bg.input  = tui_rgba(0x1f, 0x1f, 0x1f);
    d.bg.footer = tui_rgba(0x12, 0x12, 0x12);
    d.bg.gutter = d.bg.scroll;

    d.text.primary = tui_rgba(0xe8, 0xe8, 0xe8);
    d.text.muted   = tui_rgba(0x9a, 0x9a, 0x9a);
    d.text.subtle  = tui_rgba(0x63, 0x63, 0x63);
    d.text.inverse = tui_rgba(0x0a, 0x0a, 0x0a);

    d.accent.primary   = tui_rgba(0xf5, 0xa5, 0x24);
    d.accent.secondary = tui_rgba(0xb8, 0xb8, 0xb8);
    d.accent.success   = tui_rgba(0xb6, 0xd7, 0xb6);
    d.accent.warning   = tui_rgba(0xd7, 0xc8, 0xa0);
    d.accent.error     = tui_rgba(0xd7, 0xa8, 0xa8);
    d.accent.info      = tui_rgba(0xc4, 0xc4, 0xc4);

    d.border.subtle = d.bg.scroll;
    d.border.focus  = d.bg.scroll;
    d.border.gutter = d.bg.scroll;

    d.layout.pane_padding_x = 1;
    d.layout.header_padding_x = 1;
    d.layout.status_inset_x = 2;
    d.layout.input_padding_x = 2;
    d.layout.footer_gap = 2;
    d.layout.compact_cols = 72;
    d.layout.dense_cols = 88;
    d.layout.show_footer = true;
    d.layout.status_pill = true;

    d.component.agent_prefix.clear();
    d.component.agent_suffix.clear();
    d.component.status_prefix = " ";
    d.component.status_suffix = " ";
    return d;
}

TuiDesign dense_design() {
    TuiDesign d = modern_design();
    d.layout.pane_padding_x = 0;
    d.layout.header_padding_x = 1;
    d.layout.status_inset_x = 1;
    d.layout.input_padding_x = 0;
    d.layout.footer_gap = 1;
    d.layout.compact_cols = 80;
    d.layout.dense_cols = 96;
    d.layout.status_pill = false;
    d.component.agent_prefix.clear();
    d.component.agent_suffix.clear();
    d.component.status_prefix.clear();
    d.component.status_suffix.clear();
    return d;
}

void apply_overrides(TuiDesign& d, const JsonValue& root) {
    const std::string preset = root.get_string("preset", "");
    if (preset == "dense") d = dense_design();
    if (preset == "modern") d = modern_design();

    color(root, "bg", "base", d.bg.base);
    color(root, "bg", "panel", d.bg.panel);
    color(root, "bg", "header", d.bg.header);
    color(root, "bg", "scroll", d.bg.scroll);
    color(root, "bg", "status", d.bg.status);
    color(root, "bg", "input", d.bg.input);
    color(root, "bg", "footer", d.bg.footer);
    color(root, "bg", "gutter", d.bg.gutter);

    color(root, "text", "primary", d.text.primary);
    color(root, "text", "muted", d.text.muted);
    color(root, "text", "subtle", d.text.subtle);
    color(root, "text", "inverse", d.text.inverse);

    color(root, "accent", "primary", d.accent.primary);
    color(root, "accent", "secondary", d.accent.secondary);
    color(root, "accent", "success", d.accent.success);
    color(root, "accent", "warning", d.accent.warning);
    color(root, "accent", "error", d.accent.error);
    color(root, "accent", "info", d.accent.info);

    color(root, "border", "subtle", d.border.subtle);
    color(root, "border", "focus", d.border.focus);
    color(root, "border", "gutter", d.border.gutter);
    string_value(root, "border", "vertical", d.border.vertical);
    string_value(root, "border", "horizontal", d.border.horizontal);

    number(root, "layout", "pane_padding_x", d.layout.pane_padding_x);
    number(root, "layout", "header_padding_x", d.layout.header_padding_x);
    number(root, "layout", "status_inset_x", d.layout.status_inset_x);
    number(root, "layout", "input_padding_x", d.layout.input_padding_x);
    number(root, "layout", "footer_gap", d.layout.footer_gap);
    number(root, "layout", "compact_cols", d.layout.compact_cols);
    number(root, "layout", "dense_cols", d.layout.dense_cols);
    boolean(root, "layout", "show_footer", d.layout.show_footer);
    boolean(root, "layout", "status_pill", d.layout.status_pill);

    string_value(root, "component", "prompt", d.component.prompt);
    string_value(root, "component", "continuation_prompt", d.component.continuation_prompt);
    string_value(root, "component", "inactive_prompt", d.component.inactive_prompt);
    string_value(root, "component", "agent_prefix", d.component.agent_prefix);
    string_value(root, "component", "agent_suffix", d.component.agent_suffix);
    string_value(root, "component", "status_prefix", d.component.status_prefix);
    string_value(root, "component", "status_suffix", d.component.status_suffix);
    string_value(root, "component", "footer_left", d.component.footer_left);
    string_value(root, "component", "footer_right", d.component.footer_right);
    string_value(root, "component", "footer_left_compact", d.component.footer_left_compact);
    string_value(root, "component", "footer_right_compact", d.component.footer_right_compact);

    d.layout.pane_padding_x = clamp_byte(d.layout.pane_padding_x);
    d.layout.header_padding_x = clamp_byte(d.layout.header_padding_x);
    d.layout.status_inset_x = clamp_byte(d.layout.status_inset_x);
    d.layout.input_padding_x = clamp_byte(d.layout.input_padding_x);
    d.layout.footer_gap = std::max(0, d.layout.footer_gap);
    d.layout.compact_cols = std::max(20, d.layout.compact_cols);
    d.layout.dense_cols = std::max(20, d.layout.dense_cols);
}

} // namespace

TuiRgba tui_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return {r, g, b, a};
}

const TuiDesign& tui_design() {
    if (!g_design_ready) {
        g_design = modern_design();
        g_design_ready = true;
    }
    return g_design;
}

void load_tui_design(const std::string& config_dir) {
    g_design = modern_design();
    g_design_ready = true;

    const std::string path = config_dir + "/tui.json";
    std::ifstream f(path);
    if (!f) return;

    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        auto root = json_parse(ss.str());
        if (root && root->is_object()) apply_overrides(g_design, *root);
    } catch (...) {
        // Bad design config should never prevent the TUI from starting.
        g_design = modern_design();
    }
}

} // namespace arbiter
