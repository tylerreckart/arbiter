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
