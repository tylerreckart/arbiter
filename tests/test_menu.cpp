#include "tui/menu.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using arbiter::MenuItem;
using arbiter::MenuPurpose;
using arbiter::MenuState;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond << "\n"; \
        ++failures; \
    } \
} while (0)

int main() {
    MenuState p;
    CHECK(!p.active());

    p.open(MenuPurpose::Theme,
           "Themes",
           "hint",
           arbiter::menu_items_themes({"onedark", "nord", "dracula"}, "nord"),
           "nord");
    CHECK(p.active());
    CHECK(p.purpose() == MenuPurpose::Theme);
    CHECK(p.selected_item().id == "nord");
    CHECK(p.selected_index() == 1);
    CHECK(p.selected_item().current);

    p.move_selection(1, 8);
    CHECK(p.selected_item().id == "dracula");
    p.move_selection(1, 8);  // wrap
    CHECK(p.selected_item().id == "onedark");
    p.move_selection(-1, 8);  // wrap back
    CHECK(p.selected_item().id == "dracula");

    p.page_selection(-1, 2);
    CHECK(p.selected_index() == 1);  // clamped toward start

    auto snap = p.snapshot();
    CHECK(snap.active);
    CHECK(snap.items.size() == 3);
    CHECK(snap.selected == 1);
    CHECK(snap.title == "Themes");

    p.close();
    CHECK(!p.active());
    CHECK(p.selected_item().id.empty());
    CHECK(p.purpose() == MenuPurpose::None);

    // Empty list stays inactive.
    MenuState empty;
    empty.open(MenuPurpose::Theme, "Themes", "", {}, "nord");
    CHECK(!empty.active());

    // Sections are skipped when moving.
    MenuState sections;
    std::vector<MenuItem> items;
    {
        MenuItem sec;
        sec.section = true;
        sec.label = "Group";
        items.push_back(sec);
        MenuItem a;
        a.id = "a";
        a.label = "A";
        a.shortcut = "a";
        items.push_back(a);
        MenuItem b;
        b.id = "b";
        b.label = "B";
        b.shortcut = "b";
        items.push_back(b);
    }
    sections.open(MenuPurpose::Help, "Help", "", std::move(items));
    CHECK(sections.active());
    CHECK(sections.selected_item().id == "a");
    sections.move_selection(-1, 8);  // wrap to b
    CHECK(sections.selected_item().id == "b");
    sections.move_selection(1, 8);   // wrap to a (skip section)
    CHECK(sections.selected_item().id == "a");
    CHECK(sections.select_shortcut('b', 8));
    CHECK(sections.selected_item().id == "b");

    auto help = arbiter::menu_items_help();
    CHECK(!help.empty());
    bool saw_find = false;
    bool saw_section = false;
    for (const auto& it : help) {
        if (it.section) saw_section = true;
        if (it.id == "find") saw_find = true;
    }
    CHECK(saw_section);
    CHECK(saw_find);

    auto models = arbiter::menu_items_models("anthropic/claude-sonnet-5");
    CHECK(!models.empty());
    bool saw_current = false;
    for (const auto& it : models) {
        if (it.current) {
            CHECK(it.id == "anthropic/claude-sonnet-5");
            saw_current = true;
        }
    }
    CHECK(saw_current);

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
