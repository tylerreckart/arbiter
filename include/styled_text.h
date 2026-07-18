#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

enum class StyleId : std::uint8_t {
    Default,
    Dim,
    Bold,
    Italic,
    Strike,
    Heading1,
    Heading2,
    Heading3,
    Heading4,
    Code,
    CodeFence,
    Link,
    Bullet,
    Blockquote,
    Rule,
    WritLine,
    DiffAdd,
    DiffRemove,
    DiffHunk,
    DiffFile,
    Success,
    Error,
    Warning,
    Info,
    CodeKeyword,
    CodeString,
    CodeComment,
    CodeNumber,
    CodeType,
    CodeFunction,
    System,
    UserEchoArrow,
    UserEchoText,
};

struct StyleSpan {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    StyleId       id = StyleId::Default;
};

struct StyledLine {
    std::string              text;
    std::vector<StyleSpan>   spans;
};

// Display columns for a UTF-8 string (wcwidth mode 0, matches OpenTUI buffers).
[[nodiscard]] std::size_t display_width(std::string_view text);

// Truncate UTF-8 so display_width(result) <= max_cols; never splits a cluster
// and never leaves a trailing continuation byte.
[[nodiscard]] std::string trim_to_display_cols(std::string s, int max_cols);

void styled_append(StyledLine& line, StyleId id, std::string_view text);
void styled_append_char(StyledLine& line, StyleId id, char c);

// Styled user-echo line for the TUI prose path (no caret). Source lines stay
// unpadded; pad at emit via pad_styled_user_echo_line so trailing spaces in
// the payload are preserved across resize.
[[nodiscard]] StyledLine styled_user_echo(std::string_view text);
// Split on `\n` so multiline turns (`\` continuation) get one strip per row.
[[nodiscard]] std::vector<StyledLine> styled_user_echo_lines(std::string_view text);
[[nodiscard]] bool is_styled_user_echo_line(const StyledLine& line);
// Pad a single unpadded user-echo line to `cols` (does not mutate the source).
[[nodiscard]] StyledLine pad_styled_user_echo_line(const StyledLine& line, int cols);
// In-place pad helper for tests / callers that hold a temporary vector.
bool resize_styled_user_echo_lines(std::vector<StyledLine>& lines, int cols);

// Horizontal rule line (all '-', StyleId::Rule) sized to `cols`.
[[nodiscard]] bool is_styled_rule_line(const StyledLine& line);
[[nodiscard]] StyledLine styled_rule_line(int cols);
// Rewrite Rule lines in-place when wrap width changes. Returns true if any
// line text changed.
bool resize_styled_rule_lines(std::vector<StyledLine>& lines, int cols);

[[nodiscard]] std::string to_ansi(const StyledLine& line);
[[nodiscard]] std::string styled_lines_to_ansi(const std::vector<StyledLine>& lines);

} // namespace arbiter
