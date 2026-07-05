#pragma once
// arbiter/include/repl/queues.h
//
// Two thread-safe queues used to decouple the REPL's readline-owning main
// thread from the background execution thread(s):
//
//   • CommandQueue — user input lines waiting to run.  push() by the main
//     thread as soon as the user hits Enter; pop() blocks in the exec thread.
//     The user can queue up the next command while the current one is still
//     streaming.
//
//   • OutputQueue — formatted text the exec / loop threads want rendered.
//     Only the main thread calls drain_items() and writes to the scroll view.

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
    enum class Kind : std::uint8_t { Text, Diff };
    Kind kind = Kind::Text;
    std::string data;
    // When true, PaneScrollView inserts one blank row before this item.
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

    // Drain all pending items in submission order.
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
