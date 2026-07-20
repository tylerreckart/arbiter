#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace arbiter {

// Incremental line classifier for streaming agent output.  Swallows /writ
// tool-call lines and framing markers when show_writs is false; passes
// everything else through to the sink as coalesced byte chunks (preserving
// newlines) so downstream markdown state stays consistent across chunk
// boundaries.
class BlockParser {
public:
    using Sink = std::function<void(std::string_view)>;

    BlockParser(bool show_writs, Sink sink);

    void feed(std::string_view chunk);
    void flush();

    void reset();

    bool in_write_block() const { return in_write_block_; }

private:
    bool should_swallow(const std::string& line);

    bool        show_writs_ = false;
    Sink        sink_;
    std::string buf_;
    bool        in_write_block_ = false;
    bool        in_todo_block_ = false;
    bool        pending_todo_body_ = false;
};

} // namespace arbiter
