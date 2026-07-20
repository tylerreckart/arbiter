#include "tui/style_resolver.h"

namespace arbiter {

namespace {

constexpr std::uint32_t kAttrBold          = 1u << 0;
constexpr std::uint32_t kAttrDim           = 1u << 1;
constexpr std::uint32_t kAttrItalic        = 1u << 2;
constexpr std::uint32_t kAttrUnderline     = 1u << 3;
constexpr std::uint32_t kAttrStrikethrough = 1u << 7;

} // namespace

ResolvedStyle resolve_style(StyleId id) {
    const TuiDesign& d = tui_design();
    ResolvedStyle rs;
    switch (id) {
    case StyleId::Default:
        rs.fg = &d.text.primary;
        break;
    case StyleId::Dim:
        rs.fg = &d.content.text_dim;
        rs.attrs = kAttrDim;
        break;
    case StyleId::Bold:
        rs.fg = &d.text.primary;
        rs.attrs = kAttrBold;
        break;
    case StyleId::Italic:
        rs.fg = &d.text.primary;
        rs.attrs = kAttrItalic;
        break;
    case StyleId::Strike:
        rs.fg = &d.content.text_dim;
        rs.attrs = kAttrStrikethrough | kAttrDim;
        break;
    case StyleId::Heading1:
        rs.fg = &d.content.heading[0];
        rs.attrs = kAttrBold;
        break;
    case StyleId::Heading2:
        rs.fg = &d.content.heading[1];
        rs.attrs = kAttrBold;
        break;
    case StyleId::Heading3:
        rs.fg = &d.content.heading[2];
        rs.attrs = kAttrBold;
        break;
    case StyleId::Heading4:
        rs.fg = &d.content.heading[3];
        rs.attrs = kAttrBold;
        break;
    case StyleId::Code:
    case StyleId::CodeFence:
        rs.fg = &d.content.code;
        break;
    case StyleId::Link:
        rs.fg = &d.content.link;
        rs.attrs = kAttrUnderline;
        break;
    case StyleId::Bullet:
        rs.fg = &d.content.bullet;
        break;
    case StyleId::Blockquote:
        rs.fg = &d.content.blockquote;
        rs.attrs = kAttrDim;
        break;
    case StyleId::Rule:
        rs.fg = &d.content.rule;
        rs.attrs = kAttrDim;
        break;
    case StyleId::WritLine:
        rs.fg = &d.content.writ_line;
        rs.attrs = kAttrBold;
        break;
    case StyleId::DiffAdd:
        rs.fg = &d.content.diff_add;
        break;
    case StyleId::DiffRemove:
        rs.fg = &d.content.diff_remove;
        break;
    case StyleId::DiffHunk:
        rs.fg = &d.content.diff_hunk;
        rs.attrs = kAttrDim;
        break;
    case StyleId::DiffFile:
        rs.fg = &d.content.diff_file;
        break;
    case StyleId::Success:
        rs.fg = &d.content.success;
        break;
    case StyleId::Error:
        rs.fg = &d.content.error;
        break;
    case StyleId::Warning:
        rs.fg = &d.content.warning;
        break;
    case StyleId::Info:
        rs.fg = &d.content.info;
        break;
    case StyleId::CodeKeyword:
        rs.fg = &d.content.code_keyword;
        break;
    case StyleId::CodeString:
        rs.fg = &d.content.code_string;
        break;
    case StyleId::CodeComment:
        rs.fg = &d.content.code_comment;
        rs.attrs = kAttrDim;
        break;
    case StyleId::CodeNumber:
        rs.fg = &d.content.code_number;
        break;
    case StyleId::CodeType:
        rs.fg = &d.content.code_type;
        break;
    case StyleId::CodeFunction:
        rs.fg = &d.content.code_function;
        break;
    case StyleId::System:
        rs.fg = &d.content.system_fg;
        rs.attrs = kAttrDim;
        break;
    case StyleId::UserEchoArrow:
        // Left accent cell on echoed turns — mirrors pane_frame input strip.
        rs.fg = &d.accent.primary;
        rs.bg = &d.accent.primary;
        break;
    case StyleId::UserEchoText:
        rs.fg = &d.content.user_echo_text;
        rs.bg = &d.content.user_echo_bg;
        break;
    }
    return rs;
}

} // namespace arbiter
