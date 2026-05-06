// src/a2a/sse_reader.cpp — line-buffered SSE parser

#include "a2a/sse_reader.h"

#include <utility>

namespace index_ai::a2a {

SseReader::SseReader(EventCallback cb) : cb_(std::move(cb)) {}

void SseReader::feed(const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char c = data[i];
        if (c == '\r') {
            // Some servers send \r\n.  Treat \r as a soft separator —
            // if the next char is \n we'll process the line then;
            // otherwise we still want to dispatch on \r alone, since
            // RFC 6455-style SSE allows it.  We pick the simpler rule:
            // ignore \r outright and key off \n.
            continue;
        }
        if (c == '\n') {
            process_line(buf_);
            buf_.clear();
            continue;
        }
        buf_ += c;
    }
}

void SseReader::flush(bool force_dispatch) {
    // Trailing line without a terminator gets processed if we have
    // anything pending, then dispatch optionally.
    if (!buf_.empty()) {
        process_line(buf_);
        buf_.clear();
    }
    if (force_dispatch && have_event_) dispatch();
}

void SseReader::process_line(const std::string& line) {
    // Empty line is the dispatch signal.  Per the spec we dispatch
    // *only* if we've accumulated at least one field — an event with
    // no data is allowed (keep-alive) but we're conservative and
    // require some signal to surface.
    if (line.empty()) {
        if (have_event_) dispatch();
        return;
    }
    // Comment: line starting with ':' — ignored, often used as
    // a heartbeat to keep proxies from idling out.
    if (line[0] == ':') return;

    // Field: parse "name: value" with optional space after the colon.
    // Lines without a colon are treated as field-name-only with empty
    // value, per the spec — rare but legal.
    auto colon = line.find(':');
    std::string name, value;
    if (colon == std::string::npos) {
        name = line;
    } else {
        name = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value = value.substr(1);
    }

    if (name == "event") {
        event_name_ = value;
        have_event_ = true;
    } else if (name == "data") {
        if (!data_.empty()) data_ += '\n';
        data_ += value;
        have_event_ = true;
    }
    // id and retry are accepted-and-ignored.  A2A clients don't
    // reconnect with Last-Event-ID and don't honor retry hints.
}

void SseReader::dispatch() {
    // Default event type is "message" per the spec.  Fire callback,
    // then reset.  cb_ may throw — let it propagate so the caller can
    // unwind cleanly; we just release our own state first so a second
    // dispatch on the same reader after a throw starts fresh.
    std::string ev = event_name_.empty() ? "message" : event_name_;
    std::string data = std::move(data_);
    event_name_.clear();
    data_.clear();
    have_event_ = false;
    if (cb_) cb_(ev, data);
}

} // namespace index_ai::a2a
