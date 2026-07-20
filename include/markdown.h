#pragma once
// arbiter/include/markdown.h — Markdown-to-ANSI terminal renderer

#include "styled_text.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace arbiter {

// Incremental renderer for streaming output.
// Feed chunks as they arrive; prose and inline markdown emit as lines complete.
// Fenced blocks (```diff and other languages) buffer until the closing fence
// so long partial blocks do not flood scrollback mid-stream.
class MarkdownRenderer {
public:
    using DiffSink = std::function<void(const std::string& patch)>;

    void set_diff_sink(DiffSink sink) { diff_sink_ = std::move(sink); }

    using CodeOpenFn = std::function<void(std::string open_fence, std::string lang)>;
    using CodeLineFn = std::function<void(const std::string& line)>;
    using CodeCloseFn = std::function<void(std::string close_fence)>;
    void set_code_sink(CodeOpenFn on_open, CodeLineFn on_line, CodeCloseFn on_close);

    // Feed a streaming chunk. Returns styled lines ready to render.
    std::vector<StyledLine> feed_styled(const std::string& chunk);

    // Feed a streaming chunk. Returns ANSI for non-TUI callers (loop logs, etc.).
    std::string feed(const std::string& chunk);

    std::vector<StyledLine> flush_styled();
    std::string flush();

    void reset();

private:
    static bool is_diff_fence_lang(std::string_view lang);
    std::vector<StyledLine> render_buffered_code_block_styled(const std::string& close_fence);
    std::optional<StyledLine> process_line_styled(const std::string& line);
    void finish_code_close(const std::string& close_fence, std::vector<StyledLine>& out);

    std::string line_buf_;
    bool        in_code_block_ = false;
    bool        in_diff_block_ = false;
    bool        in_indent_code_ = false;  // 4-space / tab indented code → CodeSegment
    std::string diff_buf_;
    std::vector<std::string> code_buf_;
    std::string code_open_fence_;
    DiffSink    diff_sink_;
    CodeOpenFn  code_open_;
    CodeLineFn  code_line_;
    CodeCloseFn code_close_;
    bool        seen_content_  = false;
};

std::string render_markdown(const std::string& text);
std::string render_diff_ansi(const std::string& patch);

} // namespace arbiter
