#pragma once
// arbiter/include/repl/queues.h

#include "styled_text.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace arbiter {

class CommandQueue {
public:
    void push(std::string cmd);

    // Blocks until an item is available or the queue is stopped.
    // Returns false when stopped and empty.
    bool pop(std::string& out);

    // Non-blocking — returns false if the queue is empty.  Used by a single
    // exec thread multiplexing multiple panes' queues (it polls them all
    // each tick instead of blocking on one).
    bool try_pop(std::string& out);

    void stop();

    // Items waiting to execute (does NOT count the currently-executing item).
    int pending() const;

    // Discard all pending (not-yet-started) commands.
    void drain();

    // True while the exec thread is processing a command.
    bool is_busy() const { return busy_.load(); }
    void set_busy(bool b) { busy_ = b; }

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::queue<std::string> items_;
    bool                    stopped_ = false;
    std::atomic<bool>       busy_{false};
};

struct OutputItem {
    enum class Kind : std::uint8_t { Text, Diff, Prose, Code };
    enum class CodeOp : std::uint8_t { Open, Line, Close };

    Kind kind = Kind::Text;
    std::string data;
    std::vector<StyledLine> styled_lines;
    CodeOp code_op = CodeOp::Open;
    size_t code_preview_rows = 8;
    std::string code_lang;
    bool new_block = false;
};

class OutputQueue {
public:
    // Append a raw text chunk.  Chunks from the same logical message are
    // coalesced until end_message() or push_msg() marks a boundary.
    void push(const std::string& s);

    // Mark the current message as complete.  Idempotent.
    void end_message();

    // Convenience — push(s) + end_message().
    void push_msg(const std::string& s);

    // Queue a diff patch.  Preserves stream order relative to text chunks.
    void push_diff(const std::string& patch);

    // Queue styled markdown lines (ProseSegment path — no ANSI round trip).
    void push_prose(const std::vector<StyledLine>& lines);

    // Single styled status line (push_prose + end_message).
    void push_prose_msg(const std::string& text, StyleId id = StyleId::Default);

    void push_code_open(const std::string& open_fence,
                        const std::string& lang,
                        size_t preview_rows);
    void push_code_line(const std::string& line);
    void push_code_close(const std::string& close_fence);

    std::vector<OutputItem> drain_items();

    void set_notify_fn(std::function<void()> fn);

private:
    std::mutex               mu_;
    std::vector<OutputItem>  items_;
    bool                     need_sep_ = false;
    bool                     split_after_diff_ = false;
    std::function<void()>    notify_fn_;
};

} // namespace arbiter
