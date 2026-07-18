#include "tui/tui_design.h"

#include "json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace arbiter {

namespace {

// The active design is published as an immutable snapshot: writers build a
// new TuiDesign and swap the pointer; readers (`tui_design()`) do a lock-free
// atomic load.  Every snapshot ever published is retained in g_design_history
// for the life of the process, so a `const TuiDesign&` handed out before a
// theme switch can never dangle.  Theme switches are rare and user-driven, so
// the retained copies are a few KB total in practice.
//
// Writers (load_tui_design, set_show_history_sidebar) and the string state
// below are serialized by g_design_mu.  Loop threads, pane exec threads, and
// the output pump all read concurrently via tui_design()/theme().
std::mutex g_design_mu;
std::vector<std::shared_ptr<const TuiDesign>> g_design_history;  // under g_design_mu
std::atomic<const TuiDesign*> g_design_current{nullptr};
std::atomic<std::uint32_t> g_design_generation{0};
std::string g_active_preset = kDefaultTuiPreset;   // under g_design_mu
std::string g_active_theme_file;                   // under g_design_mu

// Publish `d` as the new active design.  Caller must hold g_design_mu.
void publish_design_locked(TuiDesign d) {
    auto snap = std::make_shared<const TuiDesign>(std::move(d));
    g_design_current.store(snap.get(), std::memory_order_release);
    g_design_history.push_back(std::move(snap));
}

int clamp_byte(int v) {
    return std::max(0, std::min(255, v));
}

double channel_linear(std::uint16_t v) {
    const double s = static_cast<double>(v) / 255.0;
    return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

double relative_luminance(const TuiRgba& c) {
    return 0.2126 * channel_linear(c[0])
         + 0.7152 * channel_linear(c[1])
         + 0.0722 * channel_linear(c[2]);
}

double contrast_ratio(const TuiRgba& fg, const TuiRgba& bg) {
    const double l_fg = relative_luminance(fg);
    const double l_bg = relative_luminance(bg);
    const double lighter = std::max(l_fg, l_bg);
    const double darker = std::min(l_fg, l_bg);
    return (lighter + 0.05) / (darker + 0.05);
}

TuiRgba darken(const TuiRgba& c, double factor) {
    TuiRgba out = c;
    for (int i = 0; i < 3; ++i) {
        out[static_cast<size_t>(i)] = static_cast<std::uint16_t>(
            std::clamp(static_cast<int>(c[static_cast<size_t>(i)] * factor), 0, 255));
    }
    out[3] = 255;
    return out;
}

TuiRgba blend_rgb(const TuiRgba& a, const TuiRgba& b, double t) {
    TuiRgba out = a;
    for (int i = 0; i < 3; ++i) {
        out[static_cast<size_t>(i)] = static_cast<std::uint16_t>(std::clamp(
            static_cast<int>(a[static_cast<size_t>(i)]
                             + t * (b[static_cast<size_t>(i)] - a[static_cast<size_t>(i)])),
            0,
            255));
    }
    out[3] = 255;
    return out;
}

TuiRgba ensure_contrast(TuiRgba fg, const TuiRgba& bg, double min_ratio, TuiRgba target) {
    if (contrast_ratio(fg, bg) >= min_ratio) return fg;
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 12; ++i) {
        const double mid = (lo + hi) * 0.5;
        if (contrast_ratio(blend_rgb(fg, target, mid), bg) >= min_ratio) hi = mid;
        else lo = mid;
    }
    return blend_rgb(fg, target, hi);
}

TuiRgba contrast_target_on(const TuiRgba& bg, bool light) {
    return light ? tui_rgba(0x1e, 0x23, 0x28) : tui_rgba(0xec, 0xef, 0xf4);
}

TuiRgba contrast_target_emphasis_on(const TuiRgba& bg, bool light) {
    return light ? tui_rgba(0x0d, 0x11, 0x17) : tui_rgba(0xff, 0xff, 0xff);
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

void derive_code_syntax_colors(TuiDesign::Content& c) {
    c.code_string   = c.diff_add;
    c.code_comment  = c.text_dim;
    c.code_number   = c.warning;
    c.code_keyword  = c.heading[1];
    c.code_type     = c.heading[2];
    c.code_function = c.heading[0];
}

bool rgba_unset(const TuiRgba& c) {
    return c[3] == 0;
}

void fill_diff_bg_from_base(TuiDesign& d) {
    auto& c = d.content;
    const bool light = relative_luminance(d.bg.base) > 0.45;
    c.diff_bg_context = light ? tui_rgba(0xf0, 0xf2, 0xf4) : tui_rgba(0x18, 0x18, 0x18);
    c.diff_bg_add = light ? tui_rgba(0xd8, 0xf0, 0xdc) : tui_rgba(0x0d, 0x33, 0x16);
    c.diff_bg_remove = light ? tui_rgba(0xf8, 0xd8, 0xd8) : tui_rgba(0x4a, 0x12, 0x12);
    c.diff_bg_empty = light ? tui_rgba(0xe4, 0xe6, 0xe8) : tui_rgba(0x10, 0x10, 0x10);
}

// Fill only still-unset surface slots from chrome.  Never overwrites values
// already supplied by a nested preset / earlier document in the chain.
void derive_unset_panel_surfaces(TuiDesign& d) {
    auto& c = d.content;
    if (rgba_unset(c.code_bg)) c.code_bg = d.bg.panel;
    if (rgba_unset(c.code_header_bg)) c.code_header_bg = d.bg.header;
    if (rgba_unset(c.code_gutter)) c.code_gutter = d.text.muted;
    if (rgba_unset(c.system_fg)) c.system_fg = c.text_dim;

    const bool light = relative_luminance(d.bg.base) > 0.45;
    if (rgba_unset(c.diff_bg_context)) {
        c.diff_bg_context = light ? tui_rgba(0xf0, 0xf2, 0xf4) : tui_rgba(0x18, 0x18, 0x18);
    }
    if (rgba_unset(c.diff_bg_add)) {
        c.diff_bg_add = light ? tui_rgba(0xd8, 0xf0, 0xdc) : tui_rgba(0x0d, 0x33, 0x16);
    }
    if (rgba_unset(c.diff_bg_remove)) {
        c.diff_bg_remove = light ? tui_rgba(0xf8, 0xd8, 0xd8) : tui_rgba(0x4a, 0x12, 0x12);
    }
    if (rgba_unset(c.diff_bg_empty)) {
        c.diff_bg_empty = light ? tui_rgba(0xe4, 0xe6, 0xe8) : tui_rgba(0x10, 0x10, 0x10);
    }
}

// When the current document changes chrome colors without restating the
// matching surface keys, keep panels in sync with that chrome.  Explicit
// content.* keys in this document always win (already applied).
void sync_surfaces_for_chrome_overrides(TuiDesign& d, const JsonValue& doc) {
    auto& c = d.content;
    if (child(doc, "bg", "panel") && !child(doc, "content", "code_bg")) {
        c.code_bg = d.bg.panel;
    }
    if (child(doc, "bg", "header") && !child(doc, "content", "code_header_bg")) {
        c.code_header_bg = d.bg.header;
    }
    if (child(doc, "text", "muted") && !child(doc, "content", "code_gutter")) {
        c.code_gutter = d.text.muted;
    }
    if (child(doc, "content", "text_dim") && !child(doc, "content", "system_fg")) {
        c.system_fg = c.text_dim;
    }
    const bool any_diff_bg = child(doc, "content", "diff_bg_context")
        || child(doc, "content", "diff_bg_add")
        || child(doc, "content", "diff_bg_remove")
        || child(doc, "content", "diff_bg_empty");
    if (child(doc, "bg", "base") && !any_diff_bg) {
        fill_diff_bg_from_base(d);
    }
}

void finalize_panel_surfaces(TuiDesign& d, const JsonValue* doc) {
    if (doc) sync_surfaces_for_chrome_overrides(d, *doc);
    derive_unset_panel_surfaces(d);
}

std::string executable_dir() {
#if defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    return std::filesystem::path(path).parent_path().string();
#elif defined(__linux__)
    char path[4096];
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return {};
    path[len] = '\0';
    return std::filesystem::path(path).parent_path().string();
#else
    return {};
#endif
}

std::string bundled_themes_dir() {
    static std::string cached;
    static bool tried = false;
    if (tried) return cached;
    tried = true;

    if (const char* env = std::getenv("ARBITER_THEMES_DIR")) {
        if (std::filesystem::is_directory(env)) {
            cached = env;
            return cached;
        }
    }
#ifdef ARBITER_THEMES_DIR
    if (std::filesystem::is_directory(ARBITER_THEMES_DIR)) {
        cached = ARBITER_THEMES_DIR;
        return cached;
    }
#endif
    const std::string exe = executable_dir();
    if (!exe.empty()) {
        const std::filesystem::path base(exe);
        const std::filesystem::path candidates[] = {
            base / "../share/arbiter/themes",
            base / "../../share/arbiter/themes",
            base / "../../../share/arbiter/themes",
            base / "themes",
            base / "../themes",
        };
        for (const auto& candidate : candidates) {
            std::error_code ec;
            const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
            if (!ec && std::filesystem::is_directory(resolved)) {
                cached = resolved.string();
                return cached;
            }
        }
    }
    return cached;
}

std::string resolve_theme_path(const std::string& config_dir, std::string_view name) {
    if (name.empty()) return {};
    const std::string stem(name);
    if (!config_dir.empty()) {
        const std::string user = config_dir + "/themes/" + stem + ".json";
        if (std::filesystem::exists(user)) return user;
    }
    const std::string bundled = bundled_themes_dir() + "/" + stem + ".json";
    if (std::filesystem::exists(bundled)) return bundled;
    return {};
}

std::vector<std::string> list_bundled_theme_names() {
    std::vector<std::string> out;
    const std::string dir = bundled_themes_dir();
    if (dir.empty() || !std::filesystem::is_directory(dir)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        out.push_back(entry.path().stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<JsonValue> parse_json_file(const std::string& path);
bool apply_theme_from_path(TuiDesign& d,
                           const std::string& path,
                           const std::string& config_dir);
bool resolve_and_apply_theme(TuiDesign& d,
                             const std::string& config_dir,
                             std::string_view name);

void apply_color_overrides(TuiDesign& d, const JsonValue& root) {
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

    color(root, "content", "code", d.content.code);
    color(root, "content", "link", d.content.link);
    color(root, "content", "bullet", d.content.bullet);
    color(root, "content", "blockquote", d.content.blockquote);
    color(root, "content", "rule", d.content.rule);
    color(root, "content", "writ_line", d.content.writ_line);
    color(root, "content", "diff_add", d.content.diff_add);
    color(root, "content", "diff_remove", d.content.diff_remove);
    color(root, "content", "diff_hunk", d.content.diff_hunk);
    color(root, "content", "diff_file", d.content.diff_file);
    color(root, "content", "success", d.content.success);
    color(root, "content", "error", d.content.error);
    color(root, "content", "warning", d.content.warning);
    color(root, "content", "info", d.content.info);
    color(root, "content", "code_keyword", d.content.code_keyword);
    color(root, "content", "code_string", d.content.code_string);
    color(root, "content", "code_comment", d.content.code_comment);
    color(root, "content", "code_number", d.content.code_number);
    color(root, "content", "code_type", d.content.code_type);
    color(root, "content", "code_function", d.content.code_function);
    color(root, "content", "code_bg", d.content.code_bg);
    color(root, "content", "code_header_bg", d.content.code_header_bg);
    color(root, "content", "code_gutter", d.content.code_gutter);
    color(root, "content", "diff_bg_context", d.content.diff_bg_context);
    color(root, "content", "diff_bg_add", d.content.diff_bg_add);
    color(root, "content", "diff_bg_remove", d.content.diff_bg_remove);
    color(root, "content", "diff_bg_empty", d.content.diff_bg_empty);
    color(root, "content", "system_fg", d.content.system_fg);
    color(root, "content", "text_dim", d.content.text_dim);
    color(root, "content", "text_dimmer", d.content.text_dimmer);
    color(root, "content", "accent_focused", d.content.accent_focused);
    color(root, "content", "accent_prompt", d.content.accent_prompt);
    color(root, "content", "prompt_color", d.content.prompt_color);
    color(root, "content", "user_echo_arrow", d.content.user_echo_arrow);
    color(root, "content", "user_echo_text", d.content.user_echo_text);
    color(root, "content", "border_inactive", d.content.border_inactive);
    color(root, "content", "agent_master", d.content.agent_master);
    if (auto h = child(root, "content", "heading"); h && h->is_array()) {
        const auto& arr = h->as_array();
        for (size_t i = 0; i < d.content.heading.size() && i < arr.size(); ++i) {
            if (arr[i] && arr[i]->is_string()) {
                TuiRgba parsed = d.content.heading[i];
                if (parse_hex_color(arr[i]->as_string(), parsed)) {
                    d.content.heading[i] = parsed;
                }
            }
        }
    }
    if (auto p = child(root, "content", "agent_palette"); p && p->is_array()) {
        const auto& arr = p->as_array();
        for (size_t i = 0; i < d.content.agent_palette.size() && i < arr.size(); ++i) {
            if (arr[i] && arr[i]->is_string()) {
                TuiRgba parsed = d.content.agent_palette[i];
                if (parse_hex_color(arr[i]->as_string(), parsed)) {
                    d.content.agent_palette[i] = parsed;
                }
            }
        }
    }
}

void apply_layout_component_overrides(TuiDesign& d, const JsonValue& root) {
    string_value(root, "border", "vertical", d.border.vertical);
    string_value(root, "border", "horizontal", d.border.horizontal);

    number(root, "layout", "pane_padding_x", d.layout.pane_padding_x);
    number(root, "layout", "header_padding_x", d.layout.header_padding_x);
    number(root, "layout", "status_inset_x", d.layout.status_inset_x);
    number(root, "layout", "input_padding_x", d.layout.input_padding_x);
    number(root, "layout", "footer_gap", d.layout.footer_gap);
    number(root, "layout", "block_gap", d.layout.block_gap);
    number(root, "layout", "panel_gap", d.layout.panel_gap);
    number(root, "layout", "prose_paragraph_gap", d.layout.prose_paragraph_gap);
    number(root, "layout", "scroll_pad_y", d.layout.scroll_pad_y);
    number(root, "layout", "scroll_gutter_cols", d.layout.scroll_gutter_cols);
    number(root, "layout", "compact_cols", d.layout.compact_cols);
    number(root, "layout", "dense_cols", d.layout.dense_cols);
    boolean(root, "layout", "show_footer", d.layout.show_footer);
    boolean(root, "layout", "status_pill", d.layout.status_pill);
    boolean(root, "layout", "show_history_sidebar", d.layout.show_history_sidebar);
    boolean(root, "layout", "chrome_compact_rows", d.layout.chrome_compact_rows);

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
    d.layout.block_gap = std::max(0, std::min(8, d.layout.block_gap));
    d.layout.panel_gap = std::max(0, std::min(8, d.layout.panel_gap));
    d.layout.prose_paragraph_gap = std::max(0, std::min(8, d.layout.prose_paragraph_gap));
    d.layout.scroll_pad_y = std::max(0, std::min(4, d.layout.scroll_pad_y));
    d.layout.scroll_gutter_cols = std::max(0, std::min(4, d.layout.scroll_gutter_cols));
    d.layout.compact_cols = std::max(20, d.layout.compact_cols);
    d.layout.dense_cols = std::max(20, d.layout.dense_cols);
}

void apply_overrides(TuiDesign& d,
                     const JsonValue& root,
                     const std::string& config_dir,
                     bool apply_preset = true,
                     bool apply_colors = true) {
    if (apply_preset) {
        const std::string preset = root.get_string("preset", "");
        if (!preset.empty()) {
            TuiDesign base;
            if (resolve_and_apply_theme(base, config_dir, preset)) {
                d = base;
            }
        }
    }
    if (apply_colors) apply_color_overrides(d, root);
    apply_layout_component_overrides(d, root);
}

void strip_color_overrides(JsonObject& root) {
    root.erase("bg");
    root.erase("text");
    root.erase("accent");
    root.erase("border");
    root.erase("content");
}

std::string rgba_to_hex(const TuiRgba& c) {
    char buf[8];
    std::snprintf(buf,
                  sizeof(buf),
                  "#%02x%02x%02x",
                  static_cast<unsigned>(c[0]),
                  static_cast<unsigned>(c[1]),
                  static_cast<unsigned>(c[2]));
    return buf;
}

std::shared_ptr<JsonValue> parse_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return nullptr;
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        auto root = json_parse(ss.str());
        if (root && root->is_object()) return root;
    } catch (...) {
    }
    return nullptr;
}

std::string resolve_config_relative_path(const std::string& config_dir,
                                         std::string_view rel) {
    if (rel.empty()) return {};
    if (rel.front() == '/') return std::string(rel);
    if (rel.size() >= 2 && rel[0] == '~' && rel[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + std::string(rel.substr(1));
    }
    return config_dir + "/" + std::string(rel);
}

TuiDesign default_design() {
    TuiDesign d;
    const std::string path = resolve_theme_path("", kDefaultTuiPreset);
    if (!path.empty()) {
        if (auto root = parse_json_file(path)) {
            apply_color_overrides(d, *root);
            apply_layout_component_overrides(d, *root);
            if (!child(*root, "content", "code_keyword")) {
                derive_code_syntax_colors(d.content);
            }
            finalize_panel_surfaces(d, root.get());
        }
    } else {
        finalize_panel_surfaces(d, nullptr);
    }
    return d;
}

void apply_theme_document(TuiDesign& d,
                          const JsonValue& doc,
                          const std::string& config_dir,
                          int depth = 0) {
    if (depth > 8) return;
    const std::string preset = doc.get_string("preset", "");
    if (!preset.empty()) {
        const std::string base_path = resolve_theme_path(config_dir, preset);
        if (!base_path.empty()) {
            if (auto nested = parse_json_file(base_path)) {
                apply_theme_document(d, *nested, config_dir, depth + 1);
            }
        }
    }
    apply_color_overrides(d, doc);
    apply_layout_component_overrides(d, doc);
    if (!child(doc, "content", "code_keyword")) {
        derive_code_syntax_colors(d.content);
    }
    finalize_panel_surfaces(d, &doc);
}

bool apply_theme_from_path(TuiDesign& d,
                           const std::string& path,
                           const std::string& config_dir) {
    if (auto root = parse_json_file(path)) {
        apply_theme_document(d, *root, config_dir);
        return true;
    }
    return false;
}

bool resolve_and_apply_theme(TuiDesign& d,
                             const std::string& config_dir,
                             std::string_view name) {
    if (name.empty()) return false;

    const std::string as_path = resolve_config_relative_path(config_dir, name);
    if (as_path.size() > 5 && as_path.substr(as_path.size() - 5) == ".json") {
        return apply_theme_from_path(d, as_path, config_dir);
    }

    const std::string theme_path = resolve_theme_path(config_dir, name);
    if (!theme_path.empty()) {
        return apply_theme_from_path(d, theme_path, config_dir);
    }
    return false;
}

std::shared_ptr<JsonValue> rgba_json(const TuiRgba& c) {
    return jstr(rgba_to_hex(c));
}

std::shared_ptr<JsonValue> make_color_group(
    const std::vector<std::pair<const char*, TuiRgba>>& fields) {
    JsonObject obj;
    for (const auto& [key, color] : fields) {
        obj[key] = rgba_json(color);
    }
    return jobj(std::move(obj));
}

std::shared_ptr<JsonValue> design_to_json_value(const TuiDesign& d,
                                                std::string_view preset) {
    JsonObject root;
    if (!preset.empty()) root["preset"] = jstr(std::string(preset));

    root["bg"] = make_color_group({
        {"base", d.bg.base},
        {"panel", d.bg.panel},
        {"header", d.bg.header},
        {"scroll", d.bg.scroll},
        {"status", d.bg.status},
        {"input", d.bg.input},
        {"footer", d.bg.footer},
        {"gutter", d.bg.gutter},
    });
    root["text"] = make_color_group({
        {"primary", d.text.primary},
        {"muted", d.text.muted},
        {"subtle", d.text.subtle},
        {"inverse", d.text.inverse},
    });
    root["accent"] = make_color_group({
        {"primary", d.accent.primary},
        {"secondary", d.accent.secondary},
        {"success", d.accent.success},
        {"warning", d.accent.warning},
        {"error", d.accent.error},
        {"info", d.accent.info},
    });
    root["border"] = make_color_group({
        {"subtle", d.border.subtle},
        {"focus", d.border.focus},
        {"gutter", d.border.gutter},
    });
    root["border"]->as_object_mut()["vertical"] = jstr(d.border.vertical);
    root["border"]->as_object_mut()["horizontal"] = jstr(d.border.horizontal);

    JsonObject content;
    JsonArray heading;
    for (const TuiRgba& h : d.content.heading) heading.push_back(rgba_json(h));
    content["heading"] = jarr(std::move(heading));
    const std::vector<std::pair<const char*, TuiRgba>> content_fields = {
        {"code", d.content.code},
        {"link", d.content.link},
        {"bullet", d.content.bullet},
        {"blockquote", d.content.blockquote},
        {"rule", d.content.rule},
        {"writ_line", d.content.writ_line},
        {"diff_add", d.content.diff_add},
        {"diff_remove", d.content.diff_remove},
        {"diff_hunk", d.content.diff_hunk},
        {"diff_file", d.content.diff_file},
        {"success", d.content.success},
        {"error", d.content.error},
        {"warning", d.content.warning},
        {"info", d.content.info},
        {"code_keyword", d.content.code_keyword},
        {"code_string", d.content.code_string},
        {"code_comment", d.content.code_comment},
        {"code_number", d.content.code_number},
        {"code_type", d.content.code_type},
        {"code_function", d.content.code_function},
        {"code_bg", d.content.code_bg},
        {"code_header_bg", d.content.code_header_bg},
        {"code_gutter", d.content.code_gutter},
        {"diff_bg_context", d.content.diff_bg_context},
        {"diff_bg_add", d.content.diff_bg_add},
        {"diff_bg_remove", d.content.diff_bg_remove},
        {"diff_bg_empty", d.content.diff_bg_empty},
        {"system_fg", d.content.system_fg},
        {"text_dim", d.content.text_dim},
        {"text_dimmer", d.content.text_dimmer},
        {"accent_focused", d.content.accent_focused},
        {"accent_prompt", d.content.accent_prompt},
        {"prompt_color", d.content.prompt_color},
        {"user_echo_arrow", d.content.user_echo_arrow},
        {"user_echo_text", d.content.user_echo_text},
        {"border_inactive", d.content.border_inactive},
        {"agent_master", d.content.agent_master},
    };
    for (const auto& [key, color] : content_fields) {
        content[key] = rgba_json(color);
    }
    JsonArray palette;
    for (const TuiRgba& c : d.content.agent_palette) palette.push_back(rgba_json(c));
    content["agent_palette"] = jarr(std::move(palette));
    root["content"] = jobj(std::move(content));

    JsonObject layout;
    layout["pane_padding_x"] = jnum(d.layout.pane_padding_x);
    layout["header_padding_x"] = jnum(d.layout.header_padding_x);
    layout["status_inset_x"] = jnum(d.layout.status_inset_x);
    layout["input_padding_x"] = jnum(d.layout.input_padding_x);
    layout["footer_gap"] = jnum(d.layout.footer_gap);
    layout["block_gap"] = jnum(d.layout.block_gap);
    layout["panel_gap"] = jnum(d.layout.panel_gap);
    layout["prose_paragraph_gap"] = jnum(d.layout.prose_paragraph_gap);
    layout["scroll_pad_y"] = jnum(d.layout.scroll_pad_y);
    layout["scroll_gutter_cols"] = jnum(d.layout.scroll_gutter_cols);
    layout["compact_cols"] = jnum(d.layout.compact_cols);
    layout["dense_cols"] = jnum(d.layout.dense_cols);
    layout["show_footer"] = jbool(d.layout.show_footer);
    layout["status_pill"] = jbool(d.layout.status_pill);
    layout["show_history_sidebar"] = jbool(d.layout.show_history_sidebar);
    layout["chrome_compact_rows"] = jbool(d.layout.chrome_compact_rows);
    root["layout"] = jobj(std::move(layout));

    JsonObject component;
    component["prompt"] = jstr(d.component.prompt);
    component["continuation_prompt"] = jstr(d.component.continuation_prompt);
    component["inactive_prompt"] = jstr(d.component.inactive_prompt);
    component["agent_prefix"] = jstr(d.component.agent_prefix);
    component["agent_suffix"] = jstr(d.component.agent_suffix);
    component["status_prefix"] = jstr(d.component.status_prefix);
    component["status_suffix"] = jstr(d.component.status_suffix);
    component["footer_left"] = jstr(d.component.footer_left);
    component["footer_right"] = jstr(d.component.footer_right);
    component["footer_left_compact"] = jstr(d.component.footer_left_compact);
    component["footer_right_compact"] = jstr(d.component.footer_right_compact);
    root["component"] = jobj(std::move(component));

    return jobj(std::move(root));
}

} // namespace

TuiRgba tui_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return {r, g, b, a};
}

int tui_pane_pad_x(int cols, const TuiDesign& d) {
    const int max_pad = std::max(0, d.layout.pane_padding_x);
    if (max_pad == 0 || cols <= d.layout.compact_cols) return 0;
    if (cols >= d.layout.dense_cols) return max_pad;
    const int span = d.layout.dense_cols - d.layout.compact_cols;
    if (span <= 0) return max_pad;
    const int scaled = (max_pad * (cols - d.layout.compact_cols) + span / 2) / span;
    return std::max(1, std::min(max_pad, scaled));
}

int tui_pane_edge_pad(int cols, const TuiDesign& d) {
    const int raw = tui_pane_pad_x(cols, d);
    return std::min(raw, std::max(0, (cols - 1) / 2));
}

int tui_input_pad_x(int cols, const TuiDesign& d) {
    const int max_pad = std::max(0, d.layout.input_padding_x);
    if (max_pad == 0 || cols <= d.layout.compact_cols) return 0;
    if (cols >= d.layout.dense_cols) return max_pad;
    // Track the pane pad ramp so input inset doesn't outrun outer pad.
    return std::max(0, std::min(max_pad, tui_pane_pad_x(cols, d)));
}

int tui_bottom_pad_rows(bool footer_hint_visible, const TuiDesign& d) {
    // Keep in sync with TUI::kBottomPadRows / kCompactBottomPadRows.
    constexpr int kFull = 3;
    constexpr int kCompact = 1;
    const bool footer_on = footer_hint_visible && d.layout.show_footer;
    if (!footer_on && d.layout.chrome_compact_rows) return kCompact;
    return kFull;
}

TuiRgba tui_sidebar_bg(const TuiDesign& d) {
    return darken(d.bg.base, 0.85);
}

SidebarColors tui_sidebar_colors(const TuiDesign& d) {
    const TuiRgba sbg = tui_sidebar_bg(d);
    const bool light_bg = relative_luminance(sbg) > 0.5;
    const TuiRgba soft_target = contrast_target_on(sbg, light_bg);
    const TuiRgba strong_target = contrast_target_emphasis_on(sbg, light_bg);

    SidebarColors c;
    const TuiRgba label_seed = blend_rgb(d.text.muted, d.text.primary, 0.45);
    c.label = ensure_contrast(label_seed, sbg, 4.5, soft_target);
    c.value = ensure_contrast(d.text.primary, sbg, 4.5, strong_target);
    c.body = ensure_contrast(d.text.primary, sbg, 4.5, strong_target);
    return c;
}

std::vector<std::string> tui_builtin_presets() {
    return list_bundled_theme_names();
}

bool tui_preset_is_valid(std::string_view name) {
    const std::string bundled = bundled_themes_dir() + "/" + std::string(name) + ".json";
    return std::filesystem::exists(bundled);
}

TuiDesign tui_design_for_preset(std::string_view preset) {
    TuiDesign d;
    if (resolve_and_apply_theme(d, "", preset)) return d;
    return default_design();
}

const TuiDesign& tui_design() {
    const TuiDesign* cur = g_design_current.load(std::memory_order_acquire);
    if (cur) return *cur;
    // First use before load_tui_design() — publish the default lazily.
    std::lock_guard<std::mutex> lk(g_design_mu);
    cur = g_design_current.load(std::memory_order_acquire);
    if (!cur) {
        publish_design_locked(default_design());
        cur = g_design_current.load(std::memory_order_acquire);
    }
    return *cur;
}

void load_tui_design(const std::string& config_dir, std::string_view cli_preset) {
    std::lock_guard<std::mutex> lk(g_design_mu);
    TuiDesign design = default_design();
    g_active_preset = kDefaultTuiPreset;
    g_active_theme_file.clear();

    std::shared_ptr<JsonValue> file_root;
    const std::string path = config_dir + "/tui.json";
    if (auto root = parse_json_file(path)) {
        file_root = std::move(root);
    }

    const bool cli_override = !cli_preset.empty();
    const std::string theme_file = file_root ? file_root->get_string("theme_file", "") : "";

    if (cli_override) {
        if (resolve_and_apply_theme(design, config_dir, cli_preset)) {
            g_active_preset = std::string(cli_preset);
            g_active_theme_file.clear();
        }
    } else if (!theme_file.empty()) {
        const std::string resolved = resolve_config_relative_path(config_dir, theme_file);
        if (apply_theme_from_path(design, resolved, config_dir)) {
            g_active_theme_file = resolved;
            g_active_preset.clear();
        }
    } else if (file_root) {
        const std::string preset = file_root->get_string("preset", "");
        if (!preset.empty() && resolve_and_apply_theme(design, config_dir, preset)) {
            if (tui_preset_is_valid(preset)) {
                g_active_preset = preset;
                g_active_theme_file.clear();
            } else {
                g_active_preset.clear();
                g_active_theme_file = config_dir + "/themes/" + preset + ".json";
            }
        }
    }

    if (file_root) {
        try {
            apply_color_overrides(design, *file_root);
            apply_layout_component_overrides(design, *file_root);
            // Re-sync panel/diff surfaces when tui.json tweaks chrome without
            // restating every content.* surface key.
            finalize_panel_surfaces(design, file_root.get());
        } catch (...) {
            design = default_design();
            g_active_preset = kDefaultTuiPreset;
            g_active_theme_file.clear();
        }
    }
    publish_design_locked(std::move(design));
    g_design_generation.fetch_add(1, std::memory_order_release);
}

std::uint32_t tui_design_generation() {
    return g_design_generation.load(std::memory_order_acquire);
}

std::string tui_active_preset() {
    std::lock_guard<std::mutex> lk(g_design_mu);
    return g_active_preset;
}

std::string tui_active_theme_file() {
    std::lock_guard<std::mutex> lk(g_design_mu);
    return g_active_theme_file;
}

std::string tui_themes_dir(const std::string& config_dir) {
    return config_dir + "/themes";
}

std::string tui_bundled_themes_dir() {
    return bundled_themes_dir();
}

void tui_install_bundled_themes(const std::string& config_dir, bool force) {
    const std::string src = bundled_themes_dir();
    const std::string dest = tui_themes_dir(config_dir);
    if (src.empty() || !std::filesystem::is_directory(src)) return;
    std::filesystem::create_directories(dest);
    for (const auto& entry : std::filesystem::directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        const auto out = std::filesystem::path(dest) / entry.path().filename();
        std::error_code ec;
        if (force) {
            std::filesystem::copy_file(
                entry.path(), out, std::filesystem::copy_options::overwrite_existing, ec);
        } else if (!std::filesystem::exists(out)) {
            std::filesystem::copy_file(
                entry.path(), out, std::filesystem::copy_options::skip_existing, ec);
        }
    }
}

std::vector<std::string> tui_user_theme_names(const std::string& config_dir) {
    std::vector<std::string> out;
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(config_dir) / "themes";
    if (!fs::is_directory(dir)) return out;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        out.push_back(entry.path().stem().string());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> tui_list_available_themes(const std::string& config_dir) {
    std::vector<std::string> out = tui_builtin_presets();
    for (const auto& name : tui_user_theme_names(config_dir)) {
        if (!tui_preset_is_valid(name)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool tui_theme_name_is_valid(const std::string& config_dir, std::string_view name) {
    if (tui_preset_is_valid(name)) return true;
    if (name.find('/') != std::string_view::npos || name.ends_with(".json")) {
        return parse_json_file(resolve_config_relative_path(config_dir, name)) != nullptr;
    }
    return !resolve_theme_path(config_dir, name).empty();
}

std::string tui_design_to_json(const TuiDesign& design, std::string_view preset) {
    return json_serialize(*design_to_json_value(design, preset));
}

bool tui_write_theme_file(const std::string& path,
                          const TuiDesign& design,
                          std::string_view preset) {
    std::ofstream out(path);
    if (!out) return false;
    out << tui_design_to_json(design, preset) << '\n';
    return static_cast<bool>(out);
}

static void write_tui_json_root(const std::string& path, JsonObject root_obj) {
    auto root = jobj(std::move(root_obj));
    std::ofstream out(path);
    if (out) out << json_serialize(*root);
}

void set_tui_preset(const std::string& config_dir, std::string_view preset) {
    if (!tui_theme_name_is_valid(config_dir, preset)) return;

    const std::string path = config_dir + "/tui.json";
    JsonObject root_obj;
    if (auto parsed = parse_json_file(path)) {
        root_obj = parsed->as_object();
    }

    if (tui_preset_is_valid(preset)) {
        root_obj["preset"] = jstr(std::string(preset));
        root_obj.erase("theme_file");
    } else {
        root_obj.erase("preset");
        root_obj["theme_file"] = jstr("themes/" + std::string(preset) + ".json");
    }

    write_tui_json_root(path, std::move(root_obj));
    load_tui_design(config_dir);
}

bool set_tui_theme_file(const std::string& config_dir, std::string_view theme_file) {
    const std::string resolved = resolve_config_relative_path(config_dir, theme_file);
    if (!parse_json_file(resolved)) return false;

    const std::string path = config_dir + "/tui.json";
    JsonObject root_obj;
    if (auto parsed = parse_json_file(path)) {
        root_obj = parsed->as_object();
    }
    root_obj["theme_file"] = jstr(std::string(theme_file));
    root_obj.erase("preset");
    write_tui_json_root(path, std::move(root_obj));
    load_tui_design(config_dir);
    return true;
}

void set_show_history_sidebar(const std::string& config_dir, bool show) {
    {
        // Copy-modify-publish so concurrent readers never observe a
        // half-written design.
        std::lock_guard<std::mutex> lk(g_design_mu);
        const TuiDesign* cur = g_design_current.load(std::memory_order_acquire);
        TuiDesign design = cur ? *cur : default_design();
        design.layout.show_history_sidebar = show;
        publish_design_locked(std::move(design));
    }

    const std::string path = config_dir + "/tui.json";
    JsonObject root_obj;
    {
        std::ifstream f(path);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            try {
                auto parsed = json_parse(ss.str());
                if (parsed && parsed->is_object()) root_obj = parsed->as_object();
            } catch (...) {
                root_obj.clear();
            }
        }
    }

    auto layout_it = root_obj.find("layout");
    std::shared_ptr<JsonValue> layout_val;
    if (layout_it != root_obj.end() && layout_it->second && layout_it->second->is_object()) {
        layout_val = layout_it->second;
    } else {
        layout_val = jobj();
        root_obj["layout"] = layout_val;
    }
    layout_val->as_object_mut()["show_history_sidebar"] = jbool(show);

    auto root = jobj(std::move(root_obj));
    std::ofstream out(path);
    if (out) out << json_serialize(*root);
}

} // namespace arbiter
