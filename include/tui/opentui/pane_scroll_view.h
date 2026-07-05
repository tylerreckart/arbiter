#pragma once

#include "tui/opentui/c_api.h"
#include "tui/opentui/ansi_scroll_append.h"
#include "tui/opentui/diff_panel.h"
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
    void append_diff(std::string_view patch);
    void clear();

    [[nodiscard]] int total_visual_rows() const;
    [[nodiscard]] int max_scroll_offset() const;

    void draw(OpenTuiHandle frame,
              TUI& tui,
              int scroll_offset,
              int new_while_scrolled);

private:
    struct Segment {
        virtual ~Segment() = default;
        [[nodiscard]] virtual bool is_text() const { return false; }
        [[nodiscard]] virtual int visual_rows(int content_w) const = 0;
        virtual void set_wrap_cols(int cols) = 0;
        virtual void draw(OpenTuiHandle frame,
                          int x,
                          int y,
                          int w,
                          int h,
                          int skip_rows) const = 0;
    };

    struct TextSegment final : Segment {
        OpenTuiHandle buffer_{0};
        OpenTuiHandle view_{0};
        std::unique_ptr<AnsiScrollAppender> styled_append_;
        int wrap_cols_{80};

        TextSegment();
        ~TextSegment() override;

        [[nodiscard]] bool is_text() const override { return true; }

        void append(std::string_view text);
        void clear();
        [[nodiscard]] bool is_empty() const;

        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
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

    struct DiffSegment final : Segment {
        std::string patch_;
        mutable DiffPanel panel_;
        mutable int cached_rows_{0};
        mutable bool cached_{false};

        explicit DiffSegment(std::string patch);
        [[nodiscard]] int visual_rows(int content_w) const override;
        void set_wrap_cols(int cols) override;
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

    TextSegment& current_text();
    void start_block();
    void append_blank_row();
    [[nodiscard]] bool has_rendered_content() const;

    std::vector<std::unique_ptr<Segment>> segments_;
    int buf_x_{0};
    int buf_y_{0};
    int viewport_w_{0};
    int viewport_h_{0};
    int wrap_cols_{80};
};

} // namespace arbiter::opentui
