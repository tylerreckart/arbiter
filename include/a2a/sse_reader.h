#pragma once
// include/a2a/sse_reader.h — Streaming Server-Sent Events parser.
//
// Feeds raw bytes (e.g. from a libcurl write callback) and dispatches
// fully-formed events to a sink.  Stateful across feed() calls — the
// caller can hand it data in arbitrary chunks; events are buffered
// internally until the terminating blank line arrives.
//
// Subset implemented (and the bits A2A actually needs):
//   • event: <name>     sets the event type for the in-progress event
//   • data: <text>      appends; multiple `data:` lines are joined by '\n'
//   • blank line        dispatches and resets
//   • : <comment>       ignored
//   • id, retry         accepted but not surfaced — A2A doesn't reconnect
//                       and doesn't use server-issued ids
//
// Events with no `event:` line default to "message" per the spec.
// Empty data is allowed (yields data="").

#include <functional>
#include <string>

namespace index_ai::a2a {

class SseReader {
public:
    using EventCallback = std::function<void(const std::string& event_name,
                                              const std::string& data)>;

    explicit SseReader(EventCallback cb);

    // Feed `n` bytes of raw response body.  Safe to call repeatedly with
    // any chunking; no thread-safety promises beyond what the caller
    // arranges (libcurl's write callback is single-threaded per handle).
    void feed(const char* data, size_t n);

    // Flush any in-progress event with its current data.  Call when the
    // upstream connection closes mid-event.  Per spec, an unterminated
    // event is supposed to be discarded — but for diagnostic flows where
    // the connection drops after a final-status update we'd rather
    // surface the partial than silently lose it.  Default behavior
    // discards (matches the spec); set `force_dispatch=true` to surface.
    void flush(bool force_dispatch = false);

private:
    void process_line(const std::string& line);
    void dispatch();

    EventCallback cb_;
    std::string   buf_;          // unread bytes pending newline
    std::string   event_name_;   // current event's name (default "message")
    std::string   data_;         // accumulated data lines, '\n' joined
    bool          have_event_ = false;
};

} // namespace index_ai::a2a
