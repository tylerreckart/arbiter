#pragma once
// arbiter/include/repl/queues.h

#include "commands.h"
#include "repl/prompt_attachments.h"
#include "styled_text.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

// One readline submission (or programmatic inject).  Text-only turns leave
// `attachments` empty; image drops / `/attach` populate it so the exec
// thread can call the multipart send_streaming overload.
struct QueuedCommand {
    std::string text;
    std::vector<PromptAttachment> attachments;
};

class CommandQueue {
public:
    static constexpr int kMaxDepth = 16;

    // Returns false when the queue is at capacity (item not enqueued).
    bool push(std::string cmd);
    bool push(QueuedCommand cmd);

    // Always enqueues, even at kMaxDepth.  Child-pane result frames must
    // not be dropped because the parent already has 16 user commands pending.
    void push_unbounded(std::string cmd);
    void push_unbounded(QueuedCommand cmd);

    // Blocks until an item is available or the queue is stopped.
    // Returns false when stopped and empty.
    bool pop(std::string& out);
    bool pop(QueuedCommand& out);

    // Non-blocking — returns false if the queue is empty.  Used by a single
    // exec thread multiplexing multiple panes' queues (it polls them all
    // each tick instead of blocking on one).
    bool try_pop(std::string& out);
    bool try_pop(QueuedCommand& out);

    void stop();

    // Items waiting to execute (does NOT count the currently-executing item).
    int pending() const;

    // Discard all pending (not-yet-started) commands.
    void drain();

    // True while the exec thread is processing a command.
    bool is_busy() const { return busy_.load(); }
    void set_busy(bool b) { busy_ = b; }

private:
    mutable std::mutex           mu_;
    std::condition_variable      cv_;
    std::queue<QueuedCommand>    items_;
    bool                         stopped_ = false;
    std::atomic<bool>            busy_{false};
};

struct OutputItem {
    enum class Kind : std::uint8_t { Text, Diff, Prose, Code, Tool, Thinking, UserEcho };
    enum class CodeOp : std::uint8_t { Open, Line, Close };

    Kind kind = Kind::Text;
    std::string data;
    std::vector<StyledLine> styled_lines;
    CodeOp code_op = CodeOp::Open;
    size_t code_preview_rows = 8;
    std::string code_lang;
    bool new_block = false;
    // Kind::Tool — upsert ToolSegment by tool.id (Started then Finished).
    ToolActivityEvent tool{};
    // Kind::Thinking — agent id for per-agent accent chrome (theme palette).
    std::string agent_id;
    // Kind::Diff — when >0 the proposal was registered on the producer
    // thread (stream pause path); the pump only renders it.
    int diff_proposal_id = 0;
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
    // When a review gate is set, registers the proposal, wakes the pump to
    // paint it, then blocks the caller until the user decides — pausing the
    // model stream the same way confirms pause tool dispatch.
    void push_diff(const std::string& patch);

    // Live TUI: register proposal → return id (0 to skip review).  Called on
    // the producer (exec) thread inside push_diff before the item is queued.
    using DiffRegisterFn = std::function<int(const std::string& patch)>;
    // Live TUI: block until the user reviews `id` (and apply the decision).
    using DiffReviewFn = std::function<void(int id, const std::string& patch)>;
    void set_diff_review_hooks(DiffRegisterFn reg, DiffReviewFn review);

    // Queue styled markdown lines (ProseSegment path — no ANSI round trip).
    void push_prose(const std::vector<StyledLine>& lines);

    // Single styled status line (push_prose + end_message).
    void push_prose_msg(const std::string& text, StyleId id = StyleId::Default);

    void push_code_open(const std::string& open_fence,
                        const std::string& lang,
                        size_t preview_rows);
    void push_code_line(const std::string& line);
    void push_code_close(const std::string& close_fence);

    // Queue a tool activity upsert (Started creates a row; Finished updates it).
    void push_tool(const ToolActivityEvent& event);

    // Append a reasoning/thinking delta into the current ThinkingSegment
    // (creates one if needed). Collapsed by default in the scroll view.
    // `agent_id` colors the left accent from the theme agent palette.
    void push_thinking(const std::string& delta, const std::string& agent_id = {});

    // User submit echo → UserEchoSegment (rounded box, title "user").
    // `notify` wakes the output pump; the input loop defers that until
    // CommandQueue::push succeeds so a rejected submit can still drop the echo.
    void push_user_echo(std::string_view text, bool notify = true);

    // Remove the most recent UserEcho if it is still the last queued item.
    // Returns false if the pump already drained it or the tail is another kind.
    bool try_drop_last_user_echo();

    std::vector<OutputItem> drain_items();

    void set_notify_fn(std::function<void()> fn);

private:
    std::mutex               mu_;
    std::vector<OutputItem>  items_;
    bool                     need_sep_ = false;
    bool                     split_after_diff_ = false;
    std::function<void()>    notify_fn_;
    DiffRegisterFn           diff_register_;
    DiffReviewFn             diff_review_;
};

} // namespace arbiter
