#include "render_policy.h"

namespace arbiter {

void apply_base_style(StyledLine& line, StyleId base) {
    if (base == StyleId::Default || line.text.empty()) return;
    StyledLine wrapped;
    styled_append(wrapped, base, "  ");
    if (line.spans.empty()) {
        styled_append(wrapped, base, line.text);
    } else {
        const std::uint32_t prefix_len = 2;
        wrapped.text += line.text;
        for (const auto& span : line.spans) {
            wrapped.spans.push_back({span.begin + prefix_len, span.end + prefix_len, span.id});
        }
        wrapped.spans.insert(wrapped.spans.begin(),
                               {0, prefix_len, base});
    }
    line = std::move(wrapped);
}

std::vector<StyledLine> apply_prose_policy(std::vector<StyledLine> lines,
                                             const RenderPolicy& policy) {
    if (lines.empty()) return lines;

    if (policy.collapse_fences) {
        std::vector<StyledLine> collapsed;
        bool in_fence = false;
        for (const StyledLine& line : lines) {
            const bool fence_marker =
                line.text.find("```") != std::string::npos ||
                line.text.find("~~~") != std::string::npos;
            if (fence_marker) {
                if (!in_fence) {
                    in_fence = true;
                    collapsed.push_back(
                        styled_plain_line("  … (fenced block) …", StyleId::Dim));
                } else {
                    in_fence = false;
                }
                continue;
            }
            if (in_fence) continue;
            collapsed.push_back(line);
        }
        lines = std::move(collapsed);
    }

    if (policy.max_rows == 0 && policy.max_cols == 0) {
        if (policy.base_style != StyleId::Default) {
            for (auto& line : lines) apply_base_style(line, policy.base_style);
        }
        return lines;
    }

    std::vector<StyledLine> out;
    size_t rows = 0;
    size_t cols = 0;
    bool truncated = false;

    for (StyledLine line : lines) {
        if (policy.base_style != StyleId::Default) {
            apply_base_style(line, policy.base_style);
        }
        const size_t line_cols = display_width(line.text);
        if (policy.max_rows > 0 && rows >= policy.max_rows) {
            truncated = true;
            break;
        }
        if (policy.max_cols > 0 && cols + line_cols > policy.max_cols) {
            truncated = true;
            break;
        }
        out.push_back(std::move(line));
        ++rows;
        cols += line_cols;
    }

    if (truncated) {
        out.push_back(styled_plain_line(
            "  … [truncated — full result in synthesis turn]", StyleId::Dim));
    }
    return out;
}

StyledLine styled_plain_line(std::string text, StyleId id) {
    StyledLine line;
    styled_append(line, id, std::move(text));
    return line;
}

std::vector<StyledLine> styled_plain_lines(const std::vector<std::string>& rows,
                                             StyleId id) {
    std::vector<StyledLine> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(styled_plain_line(row, id));
    }
    return out;
}

std::vector<StyledLine> tool_call_summary_lines(int total, int failed) {
    if (total <= 0) return {};
    StyledLine line;
    styled_append(line, failed == 0 ? StyleId::Success : StyleId::Error,
                  failed == 0 ? "\u2713 " : "\u2717 ");
    std::string rest = std::to_string(total) + " tool call";
    if (total != 1) rest += 's';
    if (failed > 0) {
        rest += " (";
        rest += std::to_string(failed);
        rest += " failed)";
    }
    styled_append(line, StyleId::System, rest);
    return {line};
}

} // namespace arbiter
