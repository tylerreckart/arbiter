#include "tui/stream_filter.h"

#include "tui/block_parser.h"

namespace arbiter {

StreamFilter::StreamFilter(const Config& cfg, Sink sink)
    : cfg_(cfg),
      sink_(std::move(sink)),
      parser_(cfg.verbose, [this](std::string_view bytes) {
          if (!bytes.empty()) sink_(std::string(bytes));
      }) {}

void StreamFilter::feed(const std::string& chunk) {
    parser_.feed(chunk);
}

void StreamFilter::flush() {
    parser_.flush();
}

bool StreamFilter::in_write_block() const {
    return parser_.in_write_block();
}

} // namespace arbiter
