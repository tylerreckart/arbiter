#include "stream_renderer.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace arbiter {

StreamRenderer::StreamRenderer(RenderPolicy policy, OutputQueue& queue)
    : policy_(policy),
      queue_(queue),
      parser_(policy.show_writs,
              [this](std::string_view bytes) { on_passthrough(bytes); }) {
    md_.set_diff_sink([this](const std::string& patch) {
        if (!patch.empty()) queue_.push_diff(patch);
    });

    if (policy.collapse_fences) {
        md_.set_code_sink(
            [this](std::string /*open_fence*/, std::string /*lang*/) {
                emit_prose({styled_plain_line("  … (fenced block) …", StyleId::Dim)});
            },
            [](const std::string& /*line*/) {},
            [](std::string /*close_fence*/) {});
    } else {
        md_.set_code_sink(
            [this](std::string open_fence, std::string lang) {
                queue_.push_code_open(open_fence, lang, policy_.code_preview_rows);
            },
            [this](const std::string& line) { queue_.push_code_line(line); },
            [this](std::string close_fence) { queue_.push_code_close(close_fence); });
    }
}

void StreamRenderer::emit_prose(std::vector<StyledLine> lines) {
    lines = apply_prose_policy(std::move(lines), policy_);
    if (!lines.empty()) queue_.push_prose(lines);
}

void StreamRenderer::emit_live() {
    auto live = md_.peek_live_styled();
    if (!live) {
        queue_.clear_live_prose();
        return;
    }
    std::vector<StyledLine> one;
    one.push_back(std::move(*live));
    auto styled = apply_prose_policy(std::move(one), policy_);
    if (styled.empty() || styled.front().text.empty()) {
        queue_.clear_live_prose();
        return;
    }
    queue_.set_live_prose(styled.front());
}

void StreamRenderer::on_passthrough(std::string_view bytes) {
    if (bytes.empty()) return;
    emit_prose(md_.feed_styled(std::string(bytes)));
    emit_live();
}

void StreamRenderer::feed(std::string_view chunk) {
    parser_.feed(chunk);
}

void StreamRenderer::flush() {
    parser_.flush();
    emit_prose(md_.flush_styled());
    emit_live();
}

void StreamRenderer::reset() {
    parser_.reset();
    md_.reset();
    queue_.clear_live_prose();
}

} // namespace arbiter
