#pragma once
// arbiter/include/tui/stream_filter.h — Tool-call line swallow (BlockParser shim)

#include "config.h"
#include "tui/block_parser.h"

#include <functional>
#include <string>

namespace arbiter {

class StreamFilter {
public:
    using Sink = std::function<void(const std::string&)>;

    StreamFilter(const Config& cfg, Sink sink);

    void feed(const std::string& chunk);
    void flush();

    bool in_write_block() const;

private:
    const Config& cfg_;
    Sink          sink_;
    BlockParser   parser_;
};

} // namespace arbiter
