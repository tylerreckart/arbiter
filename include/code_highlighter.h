#pragma once

#include "styled_text.h"

#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

// Normalize a markdown fence language tag (e.g. "C++" -> "cpp").
[[nodiscard]] std::string normalize_code_lang(std::string_view lang);

// Highlight one source line. Unknown languages fall back to StyleId::Code.
[[nodiscard]] StyledLine highlight_code_line(std::string_view lang, std::string_view line);

// Batch highlight for a fenced code block (called when the closing fence arrives).
[[nodiscard]] std::vector<StyledLine> highlight_code_block(
    std::string_view lang, const std::vector<std::string>& lines);

} // namespace arbiter
