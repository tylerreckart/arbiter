#pragma once
// arbiter/include/markdown.h — Markdown-to-ANSI terminal renderer

#include <functional>
#include <string>

namespace arbiter {

// Incremental renderer for streaming output.
// Feed chunks as they arrive; complete styled lines are returned immediately.
class MarkdownRenderer {
public:
    using DiffSink = std::function<void(const std::string& patch)>;

    // When set, ```diff fenced blocks are delivered here instead of as styled
    // code lines.  The sink receives the raw unified-diff body (no fences).
    void set_diff_sink(DiffSink sink) { diff_sink_ = std::move(sink); }

    // Feed a streaming chunk. Returns any complete styled lines ready to print.
    std::string feed(const std::string& chunk);

    // Flush any partial final line. Call once after the stream ends.
    std::string flush();

    // Reset all state (call between independent responses if reusing).
    void reset();

private:
    static bool is_diff_fence_lang(std::string_view lang);

    std::string line_buf_;
    bool        in_code_block_ = false;
    bool        in_diff_block_ = false;
    std::string diff_buf_;
    DiffSink    diff_sink_;
    // Models often open with a leading blank line; the REPL already pushes a
    // "\n" pad before the stream, so emitting another produces a double gap
    // under the user's prompt.  Swallow leading empty lines until the first
    // non-empty content arrives.
    bool        seen_content_  = false;

    std::string process_line(const std::string& line);
};

// Render a complete markdown string to ANSI-styled output.
std::string render_markdown(const std::string& text);

// Style a unified-diff patch for terminal scrollback (+ green, - red, @@ dim).
std::string render_diff_ansi(const std::string& patch);

} // namespace arbiter
