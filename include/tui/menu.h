#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

// Why the overlay is open — drives Enter / preview / Esc side effects in the
// input loop.  Drawing is purpose-agnostic.
enum class MenuPurpose {
    None,
    Theme,   // ↑↓ live-previews; Enter commits; Esc restores disk theme
    Help,    // browse commands; Enter shows detail (or topic help)
    Model,   // browse catalogue; Enter applies to context agent
};

struct MenuItem {
    std::string id;          // action payload (theme name, model id, help topic)
    std::string label;       // primary row text
    std::string detail;      // secondary muted text (ctx window, one-liner)
    std::string shortcut;    // optional single-key accelerator
    bool section = false;    // non-selectable group header
    bool destructive = false;
    bool current = false;    // show checkmark (active theme / model)
    bool disabled = false;
};

struct MenuSnapshot {
    bool active = false;
    MenuPurpose purpose = MenuPurpose::None;
    std::string title;
    std::string hint;
    std::string context;     // e.g. agent id for Model
    int selected = 0;
    int scroll_offset = 0;
    int max_width = 48;
    std::vector<MenuItem> items;
};

// Modal overlay list shared by /theme, /help, /model, and (later) other
// catalogue UIs.  Thread-safe: opened from the slash-command path, driven
// from the input loop, snapshotted on the paint thread.
class MenuState {
public:
    void open(MenuPurpose purpose,
              std::string title,
              std::string hint,
              std::vector<MenuItem> items,
              std::string_view select_id = {},
              std::string context = {},
              int max_width = 48);
    void close();
    [[nodiscard]] bool active() const;
    [[nodiscard]] MenuPurpose purpose() const;
    [[nodiscard]] std::string context() const;

    // delta ±1; wraps across selectable rows only.  Updates scroll so the
    // selection stays in the visible window.
    void move_selection(int delta, int visible_rows);
    void page_selection(int direction, int visible_rows);

    // Jump to the first selectable item whose shortcut matches (case-
    // insensitive).  Returns false when nothing matched.
    bool select_shortcut(char key, int visible_rows);

    [[nodiscard]] MenuItem selected_item() const;
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] MenuSnapshot snapshot() const;

private:
    void clamp_scroll_locked(int visible_rows);
    [[nodiscard]] int next_selectable_locked(int from, int delta) const;
    [[nodiscard]] int first_selectable_locked() const;

    mutable std::mutex mu_;
    bool active_ = false;
    MenuPurpose purpose_ = MenuPurpose::None;
    std::string title_;
    std::string hint_;
    std::string context_;
    int selected_ = 0;
    int scroll_offset_ = 0;
    int max_width_ = 48;
    std::vector<MenuItem> items_;
};

// Builders for catalogue menus (kept free of OpenTUI for unit tests).
[[nodiscard]] std::vector<MenuItem>
menu_items_themes(const std::vector<std::string>& themes,
                  std::string_view active_name);

[[nodiscard]] std::vector<MenuItem> menu_items_help();

[[nodiscard]] std::vector<MenuItem>
menu_items_models(std::string_view current_model_id);

} // namespace arbiter
