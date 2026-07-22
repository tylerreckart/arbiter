#pragma once

#include "tui/opentui/c_api.h"
#include "tui/opentui/ansi_scroll_append.h"
#include "tui/opentui/diff_panel.h"
#include "tui/opentui/span_scroll_append.h"
#include "commands.h"
#include "styled_text.h"
#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW
#include "tui/opentui/diff_view.h"
#endif

#include <memory>
#include "tui/tui.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter::opentui {

class Engine;

class PaneScrollView {
public:
    PaneScrollView();
    ~PaneScrollView();

    PaneScrollView(const PaneScrollView&) = delete;
    PaneScrollView& operator=(const PaneScrollView&) = delete;

    void bind(const TUI& tui);
    void set_wrap_cols(int cols);

    void append(std::string_view text, bool new_block = false);
    void append_prose(const std::vector<StyledLine>& lines, bool new_block = false);
    void append_code_open(std::string_view open_fence,
                          std::string_view lang,
                          size_t preview_rows,
                          bool new_block = false);
    void append_code_line(std::string_view line);
    void append_code_close(std::string_view close_fence);
    void append_diff(std::string_view patch);
    // Create or update an in-scroll tool activity row (by event.id).
    void upsert_tool(const ToolActivityEvent& event, bool new_block = false);
    // Append provider reasoning/thinking into a collapsible segment.
    // `agent_id` is retained for callers; chrome currently uses a shared label.
    void append_thinking(std::string_view delta,
                         bool new_block = false,
                         std::string_view agent_id = {});
    void clear();

    // Re-resolve scrollback colors after a TUI preset change.
    void retheme();

    // Toggle expand/collapse on a truncated code block or tool row in view.
    bool toggle_code_block_in_view(int scroll_offset);

    [[nodiscard]] bool has_gap() const;
    [[nodiscard]] int gap_remaining() const;
    // Creates/updates/removes the front-of-scrollback gap marker.
    // `remaining <= 0` removes it.
    void set_gap(int remaining);

    [[nodiscard]] int total_visual_rows() const;
    [[nodiscard]] int max_scroll_offset() const;

    // Visual rows (0 = top of scrollback) whose source line contains `term`,
    // case-insensitively, in top-to-bottom order.  Rows are approximate for
    // wrapped lines (the match reports the source line's position, clamped
    // into the owning segment) — good enough to jump the viewport to the
    // match.  Collapsed code-block bodies still match; their hits clamp to
    // the block's visible rows.
    [[nodiscard]] std::vector<int> find_rows(const std::string& term) const;

    void draw(OpenTuiHandle frame,
              TUI& tui,
              int scroll_offset,
              int new_while_scrolled);

private:
    struct Segment {
        virtual ~Segment() = default;
        [[nodiscard]] virtual bool is_text() const { return false; }
        [[nodiscard]] virtual bool is_prose() const { return false; }
        [[nodiscard]] virtual int visual_rows(int content_w) const = 0;
        virtual void set_wrap_cols(int cols) = 0;
        // Append this segment's plain source lines (ANSI stripped) for
        // find_rows(); line k maps approximately to the segment's k-th
        // visual row.  Default: nothing searchable.
        virtual void collect_lines(std::vector<std::string>& /*out*/) const {}
        // When true, find_rows skips line `index` but still counts it for
        // visual-row mapping (e.g. echoed `/find` chrome).
        [[nodiscard]] virtual bool find_skip_line(std::size_t /*index*/) const {
            return false;
        }
        virtual void draw(OpenTuiHandle frame,
                          int x,
                          int y,
                          int w,
                          int h,
                          int skip_rows) const = 0;
    };

    struct ProseSegment final : Segment {
        OpenTuiHandle buffer_{0};
        OpenTuiHandle view_{0};
        std::unique_ptr<SpanScrollAppender> span_append_;
        std::vector<StyledLine> source_;
        int wrap_cols_{80};

        ProseSegment();
        ~ProseSegment() override;

        [[nodiscard]] bool is_prose() const override { return true; }

        void append(const std::vector<StyledLine>& lines);
        void clear();
        void retheme();
        [[nodiscard]] bool is_empty() const;

        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void collect_lines(std::vector<std::string>& out) const override;
        [[nodiscard]] bool find_skip_line(std::size_t index) const override;

        void emit_line(const StyledLine& line);
        void emit_echo_run(const StyledLine* begin, const StyledLine* end);
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };

    struct TextSegment final : Segment {
        OpenTuiHandle buffer_{0};
        OpenTuiHandle view_{0};
        std::unique_ptr<AnsiScrollAppender> styled_append_;
        std::string source_;
        int wrap_cols_{80};

        TextSegment();
        ~TextSegment() override;

        [[nodiscard]] bool is_text() const override { return true; }

        void append(std::string_view text);
        void clear();
        void retheme();
        [[nodiscard]] bool is_empty() const;

        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void collect_lines(std::vector<std::string>& out) const override;
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };

    struct BlankSegment final : Segment {
        [[nodiscard]] int visual_rows(int /*content_w*/) const override { return 1; }
        void set_wrap_cols(int /*cols*/) override {}
        void draw(OpenTuiHandle /*frame*/,
                  int /*x*/,
                  int /*y*/,
                  int /*w*/,
                  int /*h*/,
                  int /*skip_rows*/) const override {}
    };

    // One-line marker sitting at the very front of scrollback when older
    // transcript history hasn't been replayed yet (see prepend_history /
    // set_gap below).
    struct HistoryGapSegment final : Segment {
        int remaining = 0;
        explicit HistoryGapSegment(int r) : remaining(r) {}
        [[nodiscard]] int visual_rows(int /*content_w*/) const override { return 1; }
        void set_wrap_cols(int /*cols*/) override {}
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };

    struct CodeSegment final : Segment {
        std::string lang_;
        std::string close_fence_;
        std::vector<std::string> lines_;
        std::vector<StyledLine> highlighted_;
        bool closed_ = false;
        bool expanded_ = false;
        size_t preview_rows_ = 8;
        mutable int cached_rows_{-1};

        void open(std::string lang, size_t preview_rows);
        void append_line(std::string line);
        void close(std::string close_fence);
        void toggle_expanded();
        void rehighlight();

        [[nodiscard]] bool is_truncated() const {
            return preview_rows_ > 0 && lines_.size() > preview_rows_;
        }

        [[nodiscard]] bool has_content() const { return !lines_.empty() || !lang_.empty(); }
        [[nodiscard]] size_t visible_body_count() const;
        [[nodiscard]] int gutter_width() const;
        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void collect_lines(std::vector<std::string>& out) const override;
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };

    struct DiffSegment final : Segment {
        std::string patch_;
        mutable DiffPanel panel_;
        mutable int cached_rows_{0};
        mutable bool cached_{false};

        explicit DiffSegment(std::string patch);
        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void collect_lines(std::vector<std::string>& out) const override;
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };

    // Collapsible provider reasoning/thinking block. Collapsed by default.
    // Rounded-box chrome with plain "thinking" breaking the top border; body
    // is markdown-rendered (StyledLine) with a two-cell inset.
    struct ThinkingSegment final : Segment {
        std::string text_;
        std::string agent_id_;
        bool expanded_ = false;
        mutable int wrap_cols_{80};
        static constexpr int kPreviewRows = 3;
        static constexpr int kExpandedCap = 40;
        // Border column + two-cell inset on each side.
        static constexpr int kBoxChromeCols = 6;
        static constexpr int kBodyInset = 2;

        void append(std::string_view delta);
        void set_agent_id(std::string_view agent_id);
        void toggle_expanded();
        [[nodiscard]] bool can_expand() const;
        [[nodiscard]] std::string header_text() const;
        [[nodiscard]] int body_content_cols(int content_w) const;
        [[nodiscard]] const std::vector<StyledLine>& wrapped_body(int body_cols) const;
        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void collect_lines(std::vector<std::string>& out) const override;
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;

    private:
        void invalidate_cache() const;
        mutable std::string cache_src_;
        mutable int cache_cols_{-1};
        mutable std::vector<StyledLine> body_cache_;
    };

    // Compact per-tool activity row (Claude Code–style timeline).
    // Collapsed = 1 status line; expanded = detail + result preview.
    struct ToolSegment final : Segment {
        std::string id_;
        std::string label_;
        std::string kind_;
        std::string detail_;
        std::string result_preview_;
        bool finished_ = false;
        bool ok_ = true;
        bool expanded_ = false;
        mutable int wrap_cols_{80};

        void apply(const ToolActivityEvent& event);
        void toggle_expanded();
        [[nodiscard]] bool can_expand() const;
        [[nodiscard]] std::string status_glyph() const;
        [[nodiscard]] std::string header_text() const;
        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void collect_lines(std::vector<std::string>& out) const override;
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };

#ifdef ARBITER_HAS_NATIVE_DIFF_VIEW
    struct NativeDiffSegment final : Segment {
        std::string patch_;
        mutable DiffView diff_;
        mutable int cached_rows_{0};
        mutable int cached_width_{0};

        explicit NativeDiffSegment(std::string patch);
        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
        void draw(OpenTuiHandle frame,
                  int x,
                  int y,
                  int w,
                  int h,
                  int skip_rows) const override;
    };
#endif

public:
    // --- Lazy transcript replay (Part 3.2) ---------------------------------
    // Moves the current segments out (leaving this view freshly empty, like
    // clear()) — used to render an older chunk of history into a scratch
    // PaneScrollView and transplant the result. Segments own their OpenTUI
    // buffers independent of which view/vector they sit in, so moving them
    // between instances is safe.
    std::vector<std::unique_ptr<Segment>> take_segments();
    // Inserts `segs` at the very front, re-applying this view's wrap_cols.
    void splice_front(std::vector<std::unique_ptr<Segment>> segs);

private:
    enum class SegmentKind : std::uint8_t {
        None,
        Prose,
        Text,
        Code,
        Diff,
        Tool,
        Thinking,
        Other,
    };

    TextSegment& current_text();
    ProseSegment& current_prose();
    CodeSegment& current_code();
    void start_block();
    void start_block_gap(int gap_rows);
    void append_blank_row();
    [[nodiscard]] bool has_rendered_content() const;
    [[nodiscard]] SegmentKind last_content_kind() const;
    // Drop trailing soft blank prose/text so BlankSegment gaps stay exact.
    void trim_trailing_soft_blanks();
    // Empty visual rows already present at the end of scrollback (BlankSegments
    // excluded — those are popped first): trailing empty prose / echo-pad lines.
    [[nodiscard]] int trailing_separator_rows() const;
    // Ensure exactly `gap_rows` blank rows before a new content kind.
    // Tool→Tool stays tight unless `force` (turn boundary via new_block).
    void ensure_block_gap(SegmentKind next, int gap_rows, bool force = false);

    std::vector<std::unique_ptr<Segment>> segments_;
    int buf_x_{0};
    int buf_y_{0};
    int viewport_w_{0};
    int viewport_h_{0};
    int wrap_cols_{80};
};

} // namespace arbiter::opentui
