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

void styled_append(StyledLine& line, StyleId id, std::string_view text);
void styled_append_char(StyledLine& line, StyleId id, char c);

// Styled "> text" user-echo line for the TUI prose path.
[[nodiscard]] StyledLine styled_user_echo(std::string_view text);

[[nodiscard]] std::string to_ansi(const StyledLine& line);
[[nodiscard]] std::string styled_lines_to_ansi(const std::vector<StyledLine>& lines);

} // namespace arbiter
