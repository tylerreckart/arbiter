#include "tui/tui_design.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace arbiter {

namespace {

TuiDesign g_design;
bool g_design_ready = false;
std::uint32_t g_design_generation = 0;

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

using ContentFillFn = void (*)(TuiDesign::Content&);

void fill_content_onedark(TuiDesign::Content& c) {
    c.heading = {
        tui_rgba(97, 175, 239),
        tui_rgba(198, 120, 221),
        tui_rgba(86, 182, 194),
        tui_rgba(209, 154, 102),
    };
    c.code        = tui_rgba(209, 154, 102);
    c.link        = tui_rgba(97, 175, 239);
    c.bullet      = tui_rgba(92, 99, 112);
    c.blockquote  = tui_rgba(92, 99, 112);
    c.rule        = tui_rgba(92, 99, 112);
    c.writ_line   = tui_rgba(209, 154, 102);
    c.diff_add    = tui_rgba(152, 195, 121);
    c.diff_remove = tui_rgba(224, 108, 117);
    c.diff_hunk   = tui_rgba(92, 99, 112);
    c.diff_file   = tui_rgba(97, 175, 239);
    c.success     = tui_rgba(152, 195, 121);
    c.error       = tui_rgba(224, 108, 117);
    c.warning     = tui_rgba(229, 192, 123);
    c.info        = tui_rgba(97, 175, 239);
    c.text_dim    = tui_rgba(92, 99, 112);
    c.text_dimmer = tui_rgba(62, 68, 82);
    c.accent_focused   = tui_rgba(97, 175, 239);
    c.accent_prompt    = tui_rgba(229, 192, 123);
    c.prompt_color     = tui_rgba(92, 99, 112);
    c.user_echo_arrow  = tui_rgba(92, 99, 112);
    c.user_echo_text   = tui_rgba(171, 178, 191);
    c.border_inactive  = tui_rgba(62, 68, 82);
    c.agent_master     = tui_rgba(209, 154, 102);
    c.agent_palette = {
        tui_rgba(224, 108, 117), tui_rgba(229, 192, 123), tui_rgba(209, 154, 102),
        tui_rgba(152, 195, 121), tui_rgba(97, 175, 239), tui_rgba(198, 120, 221),
        tui_rgba(86, 182, 194),  tui_rgba(171, 178, 191), tui_rgba(190, 80, 70),
        tui_rgba(181, 141, 206), tui_rgba(68, 136, 199),  tui_rgba(184, 228, 151),
    };
}

void fill_content_modern(TuiDesign::Content& c) {
    c.heading = {
        tui_rgba(245, 165, 36),
        tui_rgba(200, 200, 200),
        tui_rgba(182, 215, 182),
        tui_rgba(196, 196, 196),
    };
    c.code        = tui_rgba(245, 165, 36);
    c.link        = tui_rgba(245, 165, 36);
    c.bullet      = tui_rgba(154, 154, 154);
    c.blockquote  = tui_rgba(154, 154, 154);
    c.rule        = tui_rgba(99, 99, 99);
    c.writ_line   = tui_rgba(245, 165, 36);
    c.diff_add    = tui_rgba(182, 215, 182);
    c.diff_remove = tui_rgba(215, 168, 168);
    c.diff_hunk   = tui_rgba(154, 154, 154);
    c.diff_file   = tui_rgba(196, 196, 196);
    c.success     = tui_rgba(182, 215, 182);
    c.error       = tui_rgba(215, 168, 168);
    c.warning     = tui_rgba(215, 200, 160);
    c.info        = tui_rgba(196, 196, 196);
    c.text_dim    = tui_rgba(154, 154, 154);
    c.text_dimmer = tui_rgba(99, 99, 99);
    c.accent_focused   = tui_rgba(245, 165, 36);
    c.accent_prompt    = tui_rgba(245, 165, 36);
    c.prompt_color     = tui_rgba(154, 154, 154);
    c.user_echo_arrow  = tui_rgba(154, 154, 154);
    c.user_echo_text   = tui_rgba(232, 232, 232);
    c.border_inactive  = tui_rgba(99, 99, 99);
    c.agent_master     = tui_rgba(245, 165, 36);
    c.agent_palette = {
        tui_rgba(215, 168, 168), tui_rgba(245, 165, 36),  tui_rgba(215, 200, 160),
        tui_rgba(182, 215, 182), tui_rgba(196, 196, 196), tui_rgba(184, 184, 184),
        tui_rgba(160, 160, 160), tui_rgba(232, 232, 232), tui_rgba(200, 140, 140),
        tui_rgba(210, 210, 210), tui_rgba(170, 170, 170), tui_rgba(200, 220, 200),
    };
}

void fill_content_nord(TuiDesign::Content& c) {
    c.heading = {
        tui_rgba(136, 192, 208),
        tui_rgba(180, 142, 173),
        tui_rgba(143, 188, 187),
        tui_rgba(208, 135, 112),
    };
    c.code        = tui_rgba(235, 203, 139);
    c.link        = tui_rgba(136, 192, 208);
    c.bullet      = tui_rgba(76, 86, 106);
    c.blockquote  = tui_rgba(76, 86, 106);
    c.rule        = tui_rgba(76, 86, 106);
    c.writ_line   = tui_rgba(235, 203, 139);
    c.diff_add    = tui_rgba(163, 190, 140);
    c.diff_remove = tui_rgba(191, 97, 106);
    c.diff_hunk   = tui_rgba(76, 86, 106);
    c.diff_file   = tui_rgba(136, 192, 208);
    c.success     = tui_rgba(163, 190, 140);
    c.error       = tui_rgba(191, 97, 106);
    c.warning     = tui_rgba(235, 203, 139);
    c.info        = tui_rgba(136, 192, 208);
    c.text_dim    = tui_rgba(97, 110, 136);
    c.text_dimmer = tui_rgba(76, 86, 106);
    c.accent_focused   = tui_rgba(136, 192, 208);
    c.accent_prompt    = tui_rgba(235, 203, 139);
    c.prompt_color     = tui_rgba(97, 110, 136);
    c.user_echo_arrow  = tui_rgba(97, 110, 136);
    c.user_echo_text   = tui_rgba(236, 239, 244);
    c.border_inactive  = tui_rgba(67, 76, 94);
    c.agent_master     = tui_rgba(208, 135, 112);
    c.agent_palette = {
        tui_rgba(191, 97, 106),  tui_rgba(235, 203, 139), tui_rgba(208, 135, 112),
        tui_rgba(163, 190, 140), tui_rgba(136, 192, 208), tui_rgba(180, 142, 173),
        tui_rgba(143, 188, 187), tui_rgba(236, 239, 244), tui_rgba(180, 100, 110),
        tui_rgba(160, 130, 180), tui_rgba(110, 150, 190), tui_rgba(150, 190, 150),
    };
}

void fill_content_dracula(TuiDesign::Content& c) {
    c.heading = {
        tui_rgba(189, 147, 249),
        tui_rgba(255, 121, 198),
        tui_rgba(139, 233, 253),
        tui_rgba(255, 184, 108),
    };
    c.code        = tui_rgba(255, 184, 108);
    c.link        = tui_rgba(139, 233, 253);
    c.bullet      = tui_rgba(98, 114, 164);
    c.blockquote  = tui_rgba(98, 114, 164);
    c.rule        = tui_rgba(98, 114, 164);
    c.writ_line   = tui_rgba(255, 184, 108);
    c.diff_add    = tui_rgba(80, 250, 123);
    c.diff_remove = tui_rgba(255, 85, 85);
    c.diff_hunk   = tui_rgba(98, 114, 164);
    c.diff_file   = tui_rgba(139, 233, 253);
    c.success     = tui_rgba(80, 250, 123);
    c.error       = tui_rgba(255, 85, 85);
    c.warning     = tui_rgba(241, 250, 140);
    c.info        = tui_rgba(139, 233, 253);
    c.text_dim    = tui_rgba(98, 114, 164);
    c.text_dimmer = tui_rgba(68, 71, 90);
    c.accent_focused   = tui_rgba(189, 147, 249);
    c.accent_prompt    = tui_rgba(241, 250, 140);
    c.prompt_color     = tui_rgba(98, 114, 164);
    c.user_echo_arrow  = tui_rgba(98, 114, 164);
    c.user_echo_text   = tui_rgba(248, 248, 242);
    c.border_inactive  = tui_rgba(68, 71, 90);
    c.agent_master     = tui_rgba(255, 184, 108);
    c.agent_palette = {
        tui_rgba(255, 85, 85),   tui_rgba(241, 250, 140), tui_rgba(255, 184, 108),
        tui_rgba(80, 250, 123),  tui_rgba(139, 233, 253), tui_rgba(189, 147, 249),
        tui_rgba(255, 121, 198), tui_rgba(248, 248, 242), tui_rgba(200, 70, 70),
        tui_rgba(200, 150, 240), tui_rgba(100, 180, 230), tui_rgba(120, 230, 150),
    };
}

void fill_content_solarized(TuiDesign::Content& c) {
    c.heading = {
        tui_rgba(38, 139, 210),
        tui_rgba(211, 54, 130),
        tui_rgba(42, 161, 152),
        tui_rgba(203, 75, 22),
    };
    c.code        = tui_rgba(203, 75, 22);
    c.link        = tui_rgba(38, 139, 210);
    c.bullet      = tui_rgba(88, 110, 117);
    c.blockquote  = tui_rgba(88, 110, 117);
    c.rule        = tui_rgba(88, 110, 117);
    c.writ_line   = tui_rgba(203, 75, 22);
    c.diff_add    = tui_rgba(133, 153, 0);
    c.diff_remove = tui_rgba(220, 50, 47);
    c.diff_hunk   = tui_rgba(88, 110, 117);
    c.diff_file   = tui_rgba(38, 139, 210);
    c.success     = tui_rgba(133, 153, 0);
    c.error       = tui_rgba(220, 50, 47);
    c.warning     = tui_rgba(181, 137, 0);
    c.info        = tui_rgba(38, 139, 210);
    c.text_dim    = tui_rgba(88, 110, 117);
    c.text_dimmer = tui_rgba(7, 54, 66);
    c.accent_focused   = tui_rgba(38, 139, 210);
    c.accent_prompt    = tui_rgba(181, 137, 0);
    c.prompt_color     = tui_rgba(88, 110, 117);
    c.user_echo_arrow  = tui_rgba(88, 110, 117);
    c.user_echo_text   = tui_rgba(147, 161, 161);
    c.border_inactive  = tui_rgba(7, 54, 66);
    c.agent_master     = tui_rgba(203, 75, 22);
    c.agent_palette = {
        tui_rgba(220, 50, 47),  tui_rgba(181, 137, 0),  tui_rgba(203, 75, 22),
        tui_rgba(133, 153, 0), tui_rgba(38, 139, 210), tui_rgba(211, 54, 130),
        tui_rgba(42, 161, 152), tui_rgba(147, 161, 161), tui_rgba(190, 60, 50),
        tui_rgba(160, 90, 150), tui_rgba(50, 120, 170), tui_rgba(120, 160, 80),
    };
}

void fill_content_light(TuiDesign::Content& c) {
    c.heading = {
        tui_rgba(32, 99, 165),
        tui_rgba(142, 68, 173),
        tui_rgba(22, 128, 120),
        tui_rgba(180, 90, 20),
    };
    c.code        = tui_rgba(180, 90, 20);
    c.link        = tui_rgba(32, 99, 165);
    c.bullet      = tui_rgba(120, 130, 140);
    c.blockquote  = tui_rgba(120, 130, 140);
    c.rule        = tui_rgba(180, 185, 190);
    c.writ_line   = tui_rgba(180, 90, 20);
    c.diff_add    = tui_rgba(40, 130, 70);
    c.diff_remove = tui_rgba(190, 50, 50);
    c.diff_hunk   = tui_rgba(120, 130, 140);
    c.diff_file   = tui_rgba(32, 99, 165);
    c.success     = tui_rgba(40, 130, 70);
    c.error       = tui_rgba(190, 50, 50);
    c.warning     = tui_rgba(170, 120, 20);
    c.info        = tui_rgba(32, 99, 165);
    c.text_dim    = tui_rgba(120, 130, 140);
    c.text_dimmer = tui_rgba(160, 165, 170);
    c.accent_focused   = tui_rgba(32, 99, 165);
    c.accent_prompt    = tui_rgba(170, 120, 20);
    c.prompt_color     = tui_rgba(120, 130, 140);
    c.user_echo_arrow  = tui_rgba(120, 130, 140);
    c.user_echo_text   = tui_rgba(30, 35, 40);
    c.border_inactive  = tui_rgba(200, 205, 210);
    c.agent_master     = tui_rgba(180, 90, 20);
    c.agent_palette = {
        tui_rgba(190, 50, 50),  tui_rgba(170, 120, 20), tui_rgba(180, 90, 20),
        tui_rgba(40, 130, 70),  tui_rgba(32, 99, 165), tui_rgba(142, 68, 173),
        tui_rgba(22, 128, 120), tui_rgba(30, 35, 40),   tui_rgba(160, 60, 60),
        tui_rgba(130, 90, 160), tui_rgba(50, 100, 150), tui_rgba(60, 140, 90),
    };
}

TuiDesign base_design(ContentFillFn fill_content) {
    TuiDesign d;
    d.component.prompt.clear();
    d.component.continuation_prompt = "\u2026 ";
    d.component.inactive_prompt.clear();
    d.component.agent_prefix.clear();
    d.component.agent_suffix.clear();
    d.component.status_prefix = " ";
    d.component.status_suffix = " ";
    if (fill_content) fill_content(d.content);
    return d;
}

TuiDesign onedark_design() {
    TuiDesign d = base_design(fill_content_onedark);
    d.bg.base   = tui_rgba(0x28, 0x2c, 0x34);
    d.bg.panel  = tui_rgba(0x21, 0x25, 0x2b);
    d.bg.header = tui_rgba(0x2c, 0x32, 0x3c);
    d.bg.scroll = tui_rgba(0x28, 0x2c, 0x34);
    d.bg.status = tui_rgba(0x21, 0x25, 0x2b);
    d.bg.input  = tui_rgba(0x1e, 0x21, 0x27);
    d.bg.footer = tui_rgba(0x21, 0x25, 0x2b);
    d.bg.gutter = d.bg.scroll;

    d.text.primary = tui_rgba(0xab, 0xb2, 0xbf);
    d.text.muted   = tui_rgba(0x5c, 0x63, 0x70);
    d.text.subtle  = tui_rgba(0x3e, 0x44, 0x52);
    d.text.inverse = tui_rgba(0x28, 0x2c, 0x34);

    d.accent.primary   = tui_rgba(0x61, 0xaf, 0xef);
    d.accent.secondary = tui_rgba(0x98, 0xc3, 0x79);
    d.accent.success   = tui_rgba(0x98, 0xc3, 0x79);
    d.accent.warning   = tui_rgba(0xe5, 0xc0, 0x7b);
    d.accent.error     = tui_rgba(0xe0, 0x6c, 0x75);
    d.accent.info      = tui_rgba(0x61, 0xaf, 0xef);

    d.border.subtle = tui_rgba(0x3e, 0x44, 0x52);
    d.border.focus  = tui_rgba(0x61, 0xaf, 0xef);
    d.border.gutter = d.bg.gutter;
    return d;
}

TuiDesign modern_design() {
    TuiDesign d = base_design(fill_content_modern);
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
    d.border.focus  = d.accent.primary;
    d.border.gutter = d.bg.gutter;
    d.layout.input_padding_x = 2;
    d.layout.footer_gap = 2;
    return d;
}

TuiDesign nord_design() {
    TuiDesign d = base_design(fill_content_nord);
    d.bg.base   = tui_rgba(0x2e, 0x34, 0x40);
    d.bg.panel  = tui_rgba(0x3b, 0x42, 0x52);
    d.bg.header = tui_rgba(0x43, 0x4c, 0x5e);
    d.bg.scroll = tui_rgba(0x2e, 0x34, 0x40);
    d.bg.status = tui_rgba(0x3b, 0x42, 0x52);
    d.bg.input  = tui_rgba(0x24, 0x29, 0x34);
    d.bg.footer = tui_rgba(0x3b, 0x42, 0x52);
    d.bg.gutter = d.bg.scroll;

    d.text.primary = tui_rgba(0xec, 0xef, 0xf4);
    d.text.muted   = tui_rgba(0x81, 0x8a, 0x9a);
    d.text.subtle  = tui_rgba(0x4c, 0x56, 0x6a);
    d.text.inverse = tui_rgba(0x2e, 0x34, 0x40);

    d.accent.primary   = tui_rgba(0x88, 0xc0, 0xd0);
    d.accent.secondary = tui_rgba(0xa3, 0xbe, 0x8c);
    d.accent.success   = tui_rgba(0xa3, 0xbe, 0x8c);
    d.accent.warning   = tui_rgba(0xeb, 0xcb, 0x8b);
    d.accent.error     = tui_rgba(0xbf, 0x61, 0x6a);
    d.accent.info      = tui_rgba(0x88, 0xc0, 0xd0);

    d.border.subtle = tui_rgba(0x4c, 0x56, 0x6a);
    d.border.focus  = tui_rgba(0x88, 0xc0, 0xd0);
    d.border.gutter = d.bg.gutter;
    return d;
}

TuiDesign dracula_design() {
    TuiDesign d = base_design(fill_content_dracula);
    d.bg.base   = tui_rgba(0x28, 0x2a, 0x36);
    d.bg.panel  = tui_rgba(0x21, 0x22, 0x2c);
    d.bg.header = tui_rgba(0x44, 0x47, 0x5a);
    d.bg.scroll = tui_rgba(0x28, 0x2a, 0x36);
    d.bg.status = tui_rgba(0x21, 0x22, 0x2c);
    d.bg.input  = tui_rgba(0x1a, 0x1b, 0x24);
    d.bg.footer = tui_rgba(0x21, 0x22, 0x2c);
    d.bg.gutter = d.bg.scroll;

    d.text.primary = tui_rgba(0xf8, 0xf8, 0xf2);
    d.text.muted   = tui_rgba(0x62, 0x72, 0xa4);
    d.text.subtle  = tui_rgba(0x7a, 0x7d, 0x96);
    d.text.inverse = tui_rgba(0x28, 0x2a, 0x36);

    d.accent.primary   = tui_rgba(0xbd, 0x93, 0xf9);
    d.accent.secondary = tui_rgba(0xff, 0x79, 0xc6);
    d.accent.success   = tui_rgba(0x50, 0xfa, 0x7b);
    d.accent.warning   = tui_rgba(0xf1, 0xfa, 0x8c);
    d.accent.error     = tui_rgba(0xff, 0x55, 0x55);
    d.accent.info      = tui_rgba(0x8b, 0xe9, 0xfd);

    d.border.subtle = tui_rgba(0x44, 0x47, 0x5a);
    d.border.focus  = tui_rgba(0xbd, 0x93, 0xf9);
    d.border.gutter = d.bg.gutter;
    return d;
}

TuiDesign solarized_design() {
    TuiDesign d = base_design(fill_content_solarized);
    d.bg.base   = tui_rgba(0x00, 0x2b, 0x36);
    d.bg.panel  = tui_rgba(0x07, 0x36, 0x42);
    d.bg.header = tui_rgba(0x07, 0x36, 0x42);
    d.bg.scroll = tui_rgba(0x00, 0x2b, 0x36);
    d.bg.status = tui_rgba(0x07, 0x36, 0x42);
    d.bg.input  = tui_rgba(0x00, 0x1f, 0x27);
    d.bg.footer = tui_rgba(0x07, 0x36, 0x42);
    d.bg.gutter = d.bg.scroll;

    d.text.primary = tui_rgba(0x93, 0xa1, 0xa1);
    d.text.muted   = tui_rgba(0x65, 0x7b, 0x83);
    d.text.subtle  = tui_rgba(0x58, 0x6e, 0x75);
    d.text.inverse = tui_rgba(0x00, 0x2b, 0x36);

    d.accent.primary   = tui_rgba(0x26, 0x8b, 0xd2);
    d.accent.secondary = tui_rgba(0x2a, 0xa1, 0x98);
    d.accent.success   = tui_rgba(0x85, 0x99, 0x00);
    d.accent.warning   = tui_rgba(0xb5, 0x89, 0x00);
    d.accent.error     = tui_rgba(0xdc, 0x32, 0x2f);
    d.accent.info      = tui_rgba(0x26, 0x8b, 0xd2);

    d.border.subtle = tui_rgba(0x07, 0x36, 0x42);
    d.border.focus  = tui_rgba(0x26, 0x8b, 0xd2);
    d.border.gutter = d.bg.gutter;
    return d;
}

TuiDesign light_design() {
    TuiDesign d = base_design(fill_content_light);
    d.bg.base   = tui_rgba(0xf8, 0xf9, 0xfa);
    d.bg.panel  = tui_rgba(0xff, 0xff, 0xff);
    d.bg.header = tui_rgba(0xef, 0xf1, 0xf3);
    d.bg.scroll = tui_rgba(0xf8, 0xf9, 0xfa);
    d.bg.status = tui_rgba(0xef, 0xf1, 0xf3);
    d.bg.input  = tui_rgba(0xff, 0xff, 0xff);
    d.bg.footer = tui_rgba(0xef, 0xf1, 0xf3);
    d.bg.gutter = tui_rgba(0xef, 0xf1, 0xf3);

    d.text.primary = tui_rgba(0x1e, 0x23, 0x28);
    d.text.muted   = tui_rgba(0x78, 0x82, 0x8c);
    d.text.subtle  = tui_rgba(0xad, 0xb5, 0xbd);
    d.text.inverse = tui_rgba(0xf8, 0xf9, 0xfa);

    d.accent.primary   = tui_rgba(0x20, 0x63, 0xa5);
    d.accent.secondary = tui_rgba(0x28, 0x82, 0x80);
    d.accent.success   = tui_rgba(0x28, 0x82, 0x80);
    d.accent.warning   = tui_rgba(0xaa, 0x78, 0x14);
    d.accent.error     = tui_rgba(0xbe, 0x32, 0x32);
    d.accent.info      = tui_rgba(0x20, 0x63, 0xa5);

    d.border.subtle = tui_rgba(0xde, 0xe2, 0xe6);
    d.border.focus  = tui_rgba(0x20, 0x63, 0xa5);
    d.border.gutter = d.bg.gutter;
    return d;
}

TuiDesign apply_dense_layout(TuiDesign d) {
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

TuiDesign dense_design() {
    return apply_dense_layout(onedark_design());
}

using PresetFn = TuiDesign (*)();

const std::unordered_map<std::string, PresetFn>& preset_table() {
    static const std::unordered_map<std::string, PresetFn> kTable = {
        {"onedark", onedark_design},
        {"modern", modern_design},
        {"nord", nord_design},
        {"dracula", dracula_design},
        {"solarized", solarized_design},
        {"light", light_design},
        {"dense", dense_design},
    };
    return kTable;
}

void apply_overrides(TuiDesign& d, const JsonValue& root, bool apply_preset = true) {
    if (apply_preset) {
        const std::string preset = root.get_string("preset", "");
        if (!preset.empty()) {
            const auto& table = preset_table();
            if (const auto it = table.find(preset); it != table.end()) {
                d = it->second();
            }
        }
    }

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
    boolean(root, "layout", "show_history_sidebar", d.layout.show_history_sidebar);

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

SidebarColors tui_sidebar_colors(const TuiDesign& d) {
    const bool light_bg = relative_luminance(d.bg.header) > 0.5;
    const TuiRgba soft_target = contrast_target_on(d.bg.header, light_bg);
    const TuiRgba strong_target = contrast_target_emphasis_on(d.bg.header, light_bg);

    SidebarColors c;
    const TuiRgba label_seed = blend_rgb(d.text.muted, d.text.primary, 0.45);
    c.label = ensure_contrast(label_seed, d.bg.header, 4.5, soft_target);
    c.value = ensure_contrast(d.text.primary, d.bg.header, 4.5, strong_target);
    c.body = ensure_contrast(d.text.primary, d.bg.header, 4.5, strong_target);
    return c;
}

std::vector<std::string> tui_builtin_presets() {
    return {"onedark", "modern", "nord", "dracula", "solarized", "light", "dense"};
}

bool tui_preset_is_valid(std::string_view name) {
    return preset_table().count(std::string(name)) != 0;
}

TuiDesign tui_design_for_preset(std::string_view preset) {
    const auto& table = preset_table();
    if (const auto it = table.find(std::string(preset)); it != table.end()) {
        return it->second();
    }
    return onedark_design();
}

const TuiDesign& tui_design() {
    if (!g_design_ready) {
        g_design = onedark_design();
        g_design_ready = true;
    }
    return g_design;
}

void load_tui_design(const std::string& config_dir, std::string_view cli_preset) {
    g_design = onedark_design();
    g_design_ready = true;

    std::shared_ptr<JsonValue> file_root;
    const std::string path = config_dir + "/tui.json";
    std::ifstream f(path);
    if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        try {
            file_root = json_parse(ss.str());
            if (file_root && !file_root->is_object()) file_root.reset();
        } catch (...) {
            g_design = onedark_design();
        }
    }

    const bool cli_override = !cli_preset.empty();
    std::string preset = cli_override ? std::string(cli_preset)
                                      : (file_root ? file_root->get_string("preset", "") : "");
    if (!preset.empty() && tui_preset_is_valid(preset)) {
        g_design = tui_design_for_preset(preset);
    }

    if (file_root) {
        try {
            apply_overrides(g_design, *file_root, /*apply_preset=*/!cli_override);
        } catch (...) {
            g_design = onedark_design();
        }
    }
    ++g_design_generation;
}

std::uint32_t tui_design_generation() {
    return g_design_generation;
}

void set_show_history_sidebar(const std::string& config_dir, bool show) {
    g_design.layout.show_history_sidebar = show;
    if (!g_design_ready) g_design_ready = true;

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
