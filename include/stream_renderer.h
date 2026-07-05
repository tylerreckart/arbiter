#pragma once

#include "markdown.h"
#include "render_policy.h"
#include "repl/queues.h"
#include "tui/block_parser.h"

#include <functional>
#include <string>
#include <string_view>

namespace arbiter {

// Unified streaming pipeline: BlockParser (writ suppression) → MarkdownRenderer
// (styled prose / diff / code) → RenderPolicy → OutputQueue sinks.
class StreamRenderer {
public:
    StreamRenderer(RenderPolicy policy, OutputQueue& queue);

    void feed(std::string_view chunk);
    void flush();
    void reset();

    MarkdownRenderer& markdown() { return md_; }

private:
    void emit_prose(std::vector<StyledLine> lines);
    void on_passthrough(std::string_view bytes);

    RenderPolicy   policy_;
    OutputQueue&   queue_;
    MarkdownRenderer md_;
    BlockParser    parser_;
};

} // namespace arbiter
