#pragma once
// arbiter/include/latex_math.h — LaTeX math → terminal-friendly Unicode

#include <string>
#include <string_view>

namespace arbiter {

// Convert a LaTeX math fragment (no surrounding delimiters) into a readable
// Unicode approximation for the TUI. Handles common LLM output: fractions,
// superscripts/subscripts, \times/\approx/\text{}, and Greek letters.
// Unrecognized commands are stripped to their name or arguments when safe.
[[nodiscard]] std::string latex_math_to_plain(std::string_view latex);

} // namespace arbiter
