#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

struct ThemePickerSnapshot {
    bool active = false;
    int selected = 0;
    int scroll_offset = 0;
    std::vector<std::string> themes;
};

// Modal theme browser: Up/Down cycle with live preview; Enter commits;
// Esc cancels and restores the prior look.
class ThemePickerState {
public:
    void open(std::vector<std::string> themes, std::string_view active_name);
    void close();
    [[nodiscard]] bool active() const;

    // delta ±1; wraps. Updates scroll so the selection stays visible.
    void move_selection(int delta, int visible_rows);
    void page_selection(int direction, int visible_rows);

    [[nodiscard]] std::string selected_theme() const;
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] ThemePickerSnapshot snapshot() const;

private:
    void clamp_scroll_locked(int visible_rows);

    mutable std::mutex mu_;
    bool active_ = false;
    int selected_ = 0;
    int scroll_offset_ = 0;
    std::vector<std::string> themes_;
};

} // namespace arbiter
