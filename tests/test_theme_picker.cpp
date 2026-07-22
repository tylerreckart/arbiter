#include "tui/theme_picker.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using arbiter::ThemePickerState;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond << "\n"; \
        ++failures; \
    } \
} while (0)

int main() {
    ThemePickerState p;
    CHECK(!p.active());

    p.open({"onedark", "nord", "dracula"}, "nord");
    CHECK(p.active());
    CHECK(p.selected_theme() == "nord");
    CHECK(p.selected_index() == 1);

    p.move_selection(1, 8);
    CHECK(p.selected_theme() == "dracula");
    p.move_selection(1, 8);  // wrap
    CHECK(p.selected_theme() == "onedark");
    p.move_selection(-1, 8);  // wrap back
    CHECK(p.selected_theme() == "dracula");

    p.page_selection(-1, 2);
    CHECK(p.selected_index() == 1);  // clamped toward start

    auto snap = p.snapshot();
    CHECK(snap.active);
    CHECK(snap.themes.size() == 3);
    CHECK(snap.selected == 1);

    p.close();
    CHECK(!p.active());
    CHECK(p.selected_theme().empty());

    // Empty list stays inactive.
    ThemePickerState empty;
    empty.open({}, "nord");
    CHECK(!empty.active());

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
