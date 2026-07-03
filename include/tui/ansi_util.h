#pragma once

#include <string>
#include <string_view>

namespace arbiter {

// Drop CSI/OSC and other two-byte ESC sequences.  Plain text for OpenTUI
// TextBuffer until NativeSpanFeed carries styled spans.
std::string strip_ansi(std::string_view raw);

// Streaming variant: holds an incomplete trailing ESC sequence or UTF-8 code
// point in `hold` across chunk boundaries (output pump drains).
class AnsiStripStream {
public:
    std::string feed(std::string_view chunk);
    std::string flush();
    void reset();

private:
    std::string hold_;
};

} // namespace arbiter
