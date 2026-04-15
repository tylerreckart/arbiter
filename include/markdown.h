#pragma once
// index/include/markdown.h — Markdown-to-ANSI terminal renderer

#include <string>

namespace index_ai {

// Incremental renderer for streaming output.
// Feed chunks as they arrive; complete styled lines are returned immediately.
class MarkdownRenderer {
public:
    // Feed a streaming chunk. Returns any complete styled lines ready to print.
    std::string feed(const std::string& chunk);

    // Flush any partial final line. Call once after the stream ends.
    std::string flush();

    // Reset all state (call between independent responses if reusing).
    void reset();

private:
    std::string line_buf_;
    bool        in_code_block_ = false;
    // Models often open with a leading blank line; the REPL already pushes a
    // "\n" pad before the stream, so emitting another produces a double gap
    // under the user's prompt.  Swallow leading empty lines until the first
    // non-empty content arrives.
    bool        seen_content_  = false;

    std::string process_line(const std::string& line);
};

// Render a complete markdown string to ANSI-styled output.
std::string render_markdown(const std::string& text);

} // namespace index_ai
