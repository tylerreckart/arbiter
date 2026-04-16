#pragma once
// index/include/repl/queues.h
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
//     Only the main thread calls drain() and writes to stdout, keeping the
//     terminal free of cross-thread tearing.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace index_ai {

class CommandQueue {
public:
    void push(std::string cmd);

    // Blocks until an item is available or the queue is stopped.
    // Returns false when stopped and empty.
    bool pop(std::string& out);

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

class OutputQueue {
public:
    // Append a raw chunk.  Use for streaming — many push() calls followed by
    // a single end_message() make up one logical message.  If a prior
    // message ended (via end_message or push_msg), the first push that
    // follows automatically gets a blank-line separator prepended.
    void push(const std::string& s);

    // Mark the current message as complete.  Idempotent — multiple
    // end_message() calls in a row collapse to a single separator.  No
    // content is written to the buffer until the next push; the separator
    // is materialised there so a drain in between doesn't emit it twice.
    void end_message();

    // Convenience — push(s) + end_message().  Use for single-call messages
    // (errors, status lines, one-shot command output).
    void push_msg(const std::string& s);

    std::string drain();

private:
    std::mutex  mu_;
    std::string buf_;
    // True when the previous push ended a message — the next push applies
    // exactly one blank-line separator before appending its content.
    bool        need_sep_ = false;
};

} // namespace index_ai
