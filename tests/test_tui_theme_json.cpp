#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/tui_design.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace arbiter;

namespace {

std::string temp_dir() {
    const std::string base = std::filesystem::temp_directory_path().string()
                           + "/arbiter_tui_theme_test_"
                           + std::to_string(static_cast<unsigned long>(
                                 std::hash<std::string>{}("tui_theme")));
    std::filesystem::create_directories(base);
    return base;
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream out(path);
    out << body;
}

} // namespace

TEST_CASE("tui_design_to_json exports color groups") {
    const std::string json = tui_design_to_json(tui_design_for_preset("nord"), "");
    CHECK(json.find("\"bg\"") != std::string::npos);
    CHECK(json.find("\"content\"") != std::string::npos);
    CHECK(json.find("#") != std::string::npos);
    CHECK(json.find("nord") == std::string::npos);
}

TEST_CASE("custom theme JSON overrides accent on top of preset") {
    const std::string dir = temp_dir();
    const std::string theme_path = dir + "/themes/purple.json";
    std::filesystem::create_directories(dir + "/themes");
    write_file(theme_path, R"({
  "preset": "onedark",
  "accent": { "primary": "#ff00ff" }
})");

    write_file(dir + "/tui.json", R"({ "theme_file": "themes/purple.json" })");

    load_tui_design(dir);
    CHECK(tui_design().accent.primary[0] == 255);
    CHECK(tui_design().accent.primary[1] == 0);
    CHECK(tui_design().accent.primary[2] == 255);
    CHECK(tui_active_theme_file().find("purple.json") != std::string::npos);
}

TEST_CASE("tui.json preset plus inline overrides merge") {
    const std::string dir = temp_dir() + "_merge";
    std::filesystem::create_directories(dir);
    write_file(dir + "/tui.json", R"({
  "preset": "nord",
  "bg": { "base": "#010203" }
})");

    load_tui_design(dir);
    CHECK(tui_active_preset() == "nord");
    CHECK(tui_design().bg.base[0] == 1);
    CHECK(tui_design().bg.base[1] == 2);
    CHECK(tui_design().bg.base[2] == 3);
}

TEST_CASE("user theme name resolves from themes directory") {
    const std::string dir = temp_dir() + "_name";
    std::filesystem::create_directories(dir + "/themes");
    write_file(dir + "/themes/brand.json", R"({ "preset": "dracula" })");
    CHECK(tui_theme_name_is_valid(dir, "brand"));
    CHECK_FALSE(tui_theme_name_is_valid(dir, "missing-brand"));
}

TEST_CASE("tui_design_to_json exports panel surface and rhythm tokens") {
    const std::string json = tui_design_to_json(tui_design_for_preset("onedark"), "");
    CHECK(json.find("\"code_bg\"") != std::string::npos);
    CHECK(json.find("\"diff_bg_add\"") != std::string::npos);
    CHECK(json.find("\"system_fg\"") != std::string::npos);
    CHECK(json.find("\"block_gap\"") != std::string::npos);
    CHECK(json.find("\"panel_gap\"") != std::string::npos);
    CHECK(json.find("\"prose_paragraph_gap\"") != std::string::npos);
}

TEST_CASE("missing panel surfaces derive from chrome colors") {
    const std::string dir = temp_dir() + "_surfaces";
    std::filesystem::create_directories(dir + "/themes");
    // No preset — surfaces must be derived from the chrome colors we set.
    write_file(dir + "/themes/sparse.json", R"({
  "bg": {
    "base": "#101010",
    "panel": "#112233",
    "header": "#223344",
    "scroll": "#101010",
    "status": "#101010",
    "input": "#101010",
    "footer": "#101010",
    "gutter": "#101010"
  },
  "content": { "text_dim": "#445566" }
})");
    write_file(dir + "/tui.json", R"({ "theme_file": "themes/sparse.json" })");
    load_tui_design(dir);
    CHECK(tui_design().content.code_bg[0] == 0x11);
    CHECK(tui_design().content.code_bg[1] == 0x22);
    CHECK(tui_design().content.code_bg[2] == 0x33);
    CHECK(tui_design().content.code_header_bg[0] == 0x22);
    CHECK(tui_design().content.system_fg[0] == 0x44);
    CHECK(tui_design().content.diff_bg_add[1] != 0);  // derived non-black
}

TEST_CASE("tui_pane_pad_x soft-lerps between compact and dense") {
    TuiDesign d = tui_design_for_preset("onedark");
    d.layout.compact_cols = 72;
    d.layout.dense_cols = 88;
    d.layout.pane_padding_x = 2;
    CHECK(tui_pane_pad_x(70, d) == 0);
    CHECK(tui_pane_pad_x(88, d) == 2);
    CHECK(tui_pane_pad_x(100, d) == 2);
    const int mid = tui_pane_pad_x(80, d);
    CHECK(mid >= 1);
    CHECK(mid <= 2);
}

TEST_CASE("light preset gets light diff backgrounds") {
    const TuiDesign d = tui_design_for_preset("light");
    // Light add tint should be brighter than the dark-theme default green.
    CHECK(d.content.diff_bg_add[1] > 0x80);
}

TEST_CASE("chrome_compact_rows reclaims bottom pad when footer hidden") {
    TuiDesign d = tui_design_for_preset("onedark");
    d.layout.chrome_compact_rows = true;
    d.layout.show_footer = true;
    CHECK(tui_bottom_pad_rows(true, d) == 3);
    CHECK(tui_bottom_pad_rows(false, d) == 1);

    d.layout.chrome_compact_rows = false;
    CHECK(tui_bottom_pad_rows(false, d) == 3);

    d.layout.chrome_compact_rows = true;
    d.layout.show_footer = false;
    CHECK(tui_bottom_pad_rows(true, d) == 1);
}

TEST_CASE("tui_pane_edge_pad and tui_input_pad_x share density ramp") {
    TuiDesign d = tui_design_for_preset("onedark");
    d.layout.compact_cols = 72;
    d.layout.dense_cols = 88;
    d.layout.pane_padding_x = 2;
    d.layout.input_padding_x = 2;
    CHECK(tui_pane_edge_pad(70, d) == 0);
    CHECK(tui_input_pad_x(70, d) == 0);
    CHECK(tui_pane_edge_pad(100, d) == 2);
    CHECK(tui_input_pad_x(100, d) == 2);
    CHECK(tui_input_pad_x(80, d) <= tui_pane_pad_x(80, d));
}

TEST_CASE("tui_design_to_json exports chrome_compact_rows") {
    const std::string json = tui_design_to_json(tui_design_for_preset("onedark"), "");
    CHECK(json.find("\"chrome_compact_rows\"") != std::string::npos);
    CHECK(json.find("\"scroll_gutter_cols\"") != std::string::npos);
}
