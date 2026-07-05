#pragma once

#include "styled_text.h"

#include <cstddef>
#include <string>
#include <vector>

namespace arbiter {

struct RenderPolicy {
    bool     show_writs        = false;
    size_t   code_preview_rows = 8;
    size_t   max_rows          = 0;
    size_t   max_cols          = 0;
    bool     collapse_fences   = false;
    StyleId  base_style        = StyleId::Default;
};

inline constexpr RenderPolicy kMasterStream{
    false, 8, 0, 0, false, StyleId::Default};
inline constexpr RenderPolicy kInterim{
    false, 0, 8, 480, true, StyleId::Dim};
inline constexpr RenderPolicy kVerbose{
    true, 0, 0, 0, false, StyleId::Default};

void apply_base_style(StyledLine& line, StyleId base);
[[nodiscard]] std::vector<StyledLine> apply_prose_policy(
    std::vector<StyledLine> lines, const RenderPolicy& policy);

[[nodiscard]] StyledLine styled_plain_line(std::string text, StyleId id);
[[nodiscard]] std::vector<StyledLine> styled_plain_lines(
    const std::vector<std::string>& rows, StyleId id);

[[nodiscard]] std::vector<StyledLine> tool_call_summary_lines(int total, int failed);

} // namespace arbiter
