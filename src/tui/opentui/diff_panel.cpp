#include "tui/opentui/diff_panel.h"

#include "tui/tui_design.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace arbiter::opentui {

namespace {

struct HunkLine {
    enum class Tag : std::uint8_t { context, add, remove };
    Tag tag = Tag::context;
    std::string content;
};

struct Hunk {
    std::uint32_t old_start = 0;
    std::uint32_t new_start = 0;
    std::vector<HunkLine> lines;
};

void draw_text(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::string_view text,
               const TuiRgba& fg,
               const TuiRgba& bg,
               std::uint32_t attrs = 0) {
    if (text.empty()) return;
    bufferDrawText(frame,
                   text.data(),
                   static_cast<std::uint32_t>(text.size()),
                   x,
                   y,
                   fg.data(),
                   bg.data(),
                   attrs);
}

void fill_rect(OpenTuiHandle frame,
               std::uint32_t x,
               std::uint32_t y,
               std::uint32_t w,
               std::uint32_t h,
               const TuiRgba& bg) {
    if (w == 0 || h == 0) return;
    bufferFillRect(frame, x, y, w, h, bg.data());
}

std::uint32_t parse_range_start(std::string_view part) {
    if (part.empty() || (part[0] != '-' && part[0] != '+')) return 0;
    auto digits = part.substr(1);
    const auto comma = digits.find(',');
    if (comma != std::string_view::npos) digits = digits.substr(0, comma);
    try {
        return static_cast<std::uint32_t>(std::stoul(std::string(digits)));
    } catch (...) {
        return 0;
    }
}

bool parse_patch(std::string_view patch,
                 std::string& header_old,
                 std::string& header_new,
                 std::vector<Hunk>& hunks) {
    header_old.clear();
    header_new.clear();
    hunks.clear();

    Hunk current{};
    bool in_hunk = false;

    std::size_t pos = 0;
    while (pos <= patch.size()) {
        const std::size_t end = patch.find('\n', pos);
        std::string_view raw(patch.data() + pos,
                             (end == std::string_view::npos) ? patch.size() - pos
                                                             : end - pos);
        while (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);

        if (raw.size() >= 4 && raw.substr(0, 4) == "--- ") {
            header_old = std::string(raw.substr(4));
        } else if (raw.size() >= 4 && raw.substr(0, 4) == "+++ ") {
            header_new = std::string(raw.substr(4));
        } else if (raw.size() >= 2 && raw.substr(0, 2) == "@@") {
            if (in_hunk && !current.lines.empty()) hunks.push_back(std::move(current));
            current = Hunk{};
            in_hunk = true;
            const auto plus = raw.find('+');
            const auto space = raw.find(' ', 2);
            if (plus != std::string_view::npos) {
                const auto old_part = raw.substr(2, plus - 2);
                const auto new_part = (space != std::string_view::npos)
                    ? raw.substr(plus, space - plus)
                    : raw.substr(plus);
                current.old_start = parse_range_start(old_part);
                current.new_start = parse_range_start(new_part);
            }
        } else if (in_hunk && !raw.empty()) {
            const char marker = raw[0];
            if (marker == '+' || marker == '-' || marker == ' ') {
                HunkLine hl;
                hl.tag = (marker == '+') ? HunkLine::Tag::add
                         : (marker == '-') ? HunkLine::Tag::remove
                                           : HunkLine::Tag::context;
                hl.content = (raw.size() > 1) ? std::string(raw.substr(1)) : std::string{};
                current.lines.push_back(std::move(hl));
            }
        }

        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    if (in_hunk && !current.lines.empty()) hunks.push_back(std::move(current));
    return !hunks.empty();
}

std::string trim_filename(std::string_view path) {
    if (path.size() >= 2 && path.substr(0, 2) == "a/") path.remove_prefix(2);
    if (path.size() >= 2 && path.substr(0, 2) == "b/") path.remove_prefix(2);
    return std::string(path);
}

enum class BgLine : std::uint8_t { context, add, remove, empty };

TuiRgba bg_for_line(BgLine line, const TuiDesign& d) {
    switch (line) {
    case BgLine::add:
        return tui_rgba(0x0d, 0x33, 0x16);
    case BgLine::remove:
        return tui_rgba(0x4a, 0x12, 0x12);
    case BgLine::empty:
        return tui_rgba(0x10, 0x10, 0x10);
    case BgLine::context:
    default:
        return tui_rgba(0x18, 0x18, 0x18);
    }
}

std::string truncate_cells(std::string_view text, int max_cells) {
    if (max_cells <= 0) return {};
    int w = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const int cw = (c < 0x80) ? 1 : 2;
        if (w + cw > max_cells) break;
        w += cw;
        i += (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4));
    }
    return std::string(text.substr(0, i));
}

} // namespace

void DiffPanel::set_patch(std::string_view patch) {
    header_old_.clear();
    header_new_.clear();
    lines_.clear();
    rows_ = 0;

    std::vector<Hunk> hunks;
    if (!parse_patch(patch, header_old_, header_new_, hunks)) return;

    split_ = true;
    for (const auto& hunk : hunks) {
        std::uint32_t old_line = hunk.old_start;
        std::uint32_t new_line = hunk.new_start;
        std::size_t i = 0;
        while (i < hunk.lines.size()) {
            const auto& hl = hunk.lines[i];
            if (hl.tag == HunkLine::Tag::context) {
                Row row;
                row.left = {Kind::context, hl.content, old_line, true, ' '};
                row.right = {Kind::context, hl.content, new_line, true, ' '};
                lines_.push_back(std::move(row));
                ++old_line;
                ++new_line;
                ++i;
                continue;
            }

            std::vector<SideLine> removes;
            std::vector<SideLine> adds;
            while (i < hunk.lines.size() && hunk.lines[i].tag != HunkLine::Tag::context) {
                const auto& cur = hunk.lines[i];
                if (cur.tag == HunkLine::Tag::remove) {
                    removes.push_back({Kind::remove, cur.content, old_line++, true, '-'});
                } else if (cur.tag == HunkLine::Tag::add) {
                    adds.push_back({Kind::add, cur.content, new_line++, true, '+'});
                }
                ++i;
            }

            const std::size_t max_len = std::max(removes.size(), adds.size());
            for (std::size_t j = 0; j < max_len; ++j) {
                Row row;
                row.left = (j < removes.size())
                    ? removes[j]
                    : SideLine{Kind::empty, "", 0, false, '\0'};
                row.right = (j < adds.size())
                    ? adds[j]
                    : SideLine{Kind::empty, "", 0, false, '\0'};
                lines_.push_back(std::move(row));
            }
        }
    }
    rows_ = lines_.size() + 1;
}

int DiffPanel::gutter_width() const {
    std::uint32_t max_num = 0;
    for (const auto& row : lines_) {
        if (row.left.show_line_number) max_num = std::max(max_num, row.left.line_num);
        if (row.right.show_line_number) max_num = std::max(max_num, row.right.line_num);
    }
    std::uint32_t digits = 1;
    std::uint32_t n = max_num;
    while (n >= 10) { n /= 10; ++digits; }
    return static_cast<int>(digits) + 3;
}

void DiffPanel::draw(OpenTuiHandle frame,
                     int x,
                     int y,
                     int w,
                     int h,
                     int skip_rows) const {
    if (w <= 0 || h <= 0 || lines_.empty()) return;

    const TuiDesign& d = tui_design();
    const int total_rows = static_cast<int>(rows_);

    const int first_row = std::max(0, skip_rows);
    if (first_row >= total_rows) return;

    const int draw_rows = std::min(h, total_rows - first_row);
    const int gutter = gutter_width();
    const int left_w = w / 2;
    const int right_x = x + left_w;
    const int right_w = w - left_w;

    fill_rect(frame,
              static_cast<std::uint32_t>(x),
              static_cast<std::uint32_t>(y),
              static_cast<std::uint32_t>(w),
              static_cast<std::uint32_t>(draw_rows),
              d.bg.panel);

    for (int row = 0; row < draw_rows; ++row) {
        const int global = first_row + row;
        const int py = y + row;
        if (global == 0) {
            fill_rect(frame,
                      static_cast<std::uint32_t>(x),
                      static_cast<std::uint32_t>(py),
                      static_cast<std::uint32_t>(w),
                      1,
                      d.bg.header);

            const std::string old_title = trim_filename(header_old_);
            const std::string new_title = trim_filename(header_new_);
            draw_text(frame,
                      static_cast<std::uint32_t>(x + 1),
                      static_cast<std::uint32_t>(py),
                      truncate_cells(old_title, left_w - 2),
                      d.text.primary,
                      d.bg.header);
            draw_text(frame,
                      static_cast<std::uint32_t>(right_x + 1),
                      static_cast<std::uint32_t>(py),
                      truncate_cells(new_title, right_w - 2),
                      d.text.primary,
                      d.bg.header);
            continue;
        }

        const int line_idx = global - 1;
        if (line_idx < 0 || line_idx >= static_cast<int>(lines_.size())) continue;
        const Row& r = lines_[static_cast<std::size_t>(line_idx)];

        auto to_bg = [](Kind kind) -> BgLine {
            switch (kind) {
            case Kind::add: return BgLine::add;
            case Kind::remove: return BgLine::remove;
            case Kind::empty: return BgLine::empty;
            default: return BgLine::context;
            }
        };
        const TuiRgba left_bg = bg_for_line(to_bg(r.left.kind), d);
        const TuiRgba right_bg = bg_for_line(to_bg(r.right.kind), d);
        fill_rect(frame,
                  static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(py),
                  static_cast<std::uint32_t>(left_w),
                  1,
                  left_bg);
        fill_rect(frame,
                  static_cast<std::uint32_t>(right_x),
                  static_cast<std::uint32_t>(py),
                  static_cast<std::uint32_t>(right_w),
                  1,
                  right_bg);

        const int content_w = std::max(1, left_w - gutter - 1);
        if (r.left.show_line_number) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%*u ",
                          gutter - 2,
                          r.left.line_num);
            const TuiRgba sign_fg = (r.left.sign == '-')
                ? d.accent.error
                : (r.left.sign == '+') ? d.accent.success : d.text.muted;
            draw_text(frame,
                      static_cast<std::uint32_t>(x + 1),
                      static_cast<std::uint32_t>(py),
                      buf,
                      d.text.muted,
                      left_bg);
            if (r.left.sign == '-' || r.left.sign == '+') {
                char sign[2] = {r.left.sign, '\0'};
                draw_text(frame,
                          static_cast<std::uint32_t>(x + 1 + static_cast<int>(std::strlen(buf)) - 1),
                          static_cast<std::uint32_t>(py),
                          sign,
                          sign_fg,
                          left_bg,
                          1);
            }
            draw_text(frame,
                      static_cast<std::uint32_t>(x + 1 + static_cast<std::uint32_t>(gutter)),
                      static_cast<std::uint32_t>(py),
                      truncate_cells(r.left.content, content_w),
                      d.text.primary,
                      left_bg);
        }

        if (r.right.show_line_number) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%*u ",
                          gutter - 2,
                          r.right.line_num);
            const TuiRgba sign_fg = (r.right.sign == '-')
                ? d.accent.error
                : (r.right.sign == '+') ? d.accent.success : d.text.muted;
            draw_text(frame,
                      static_cast<std::uint32_t>(right_x),
                      static_cast<std::uint32_t>(py),
                      buf,
                      d.text.muted,
                      right_bg);
            if (r.right.sign == '-' || r.right.sign == '+') {
                char sign[2] = {r.right.sign, '\0'};
                draw_text(frame,
                          static_cast<std::uint32_t>(right_x + static_cast<int>(std::strlen(buf)) - 1),
                          static_cast<std::uint32_t>(py),
                          sign,
                          sign_fg,
                          right_bg,
                          1);
            }
            draw_text(frame,
                      static_cast<std::uint32_t>(right_x + static_cast<std::uint32_t>(gutter)),
                      static_cast<std::uint32_t>(py),
                      truncate_cells(r.right.content, content_w),
                      d.text.primary,
                      right_bg);
        }
    }
}

} // namespace arbiter::opentui
