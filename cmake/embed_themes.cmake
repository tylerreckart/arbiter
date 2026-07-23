# cmake/embed_themes.cmake
#
# Reads every themes/*.json file and emits a header containing each file's
# contents as a raw-string literal plus an id→json table.  Invoked from the
# top-level CMakeLists.txt via add_custom_command, so the header regenerates
# whenever any input JSON changes.
#
# Inputs (passed as -D defines):
#   THEMES_DIR — absolute path to the themes/ source directory
#   OUT_FILE   — absolute path to the header to write
#
# The generated header lives in the build dir; nothing source-tracked is
# touched.  tui_design.cpp consumes it via #include "themes_data.h".

if(NOT DEFINED THEMES_DIR)
    message(FATAL_ERROR "embed_themes.cmake: THEMES_DIR not set")
endif()
if(NOT DEFINED OUT_FILE)
    message(FATAL_ERROR "embed_themes.cmake: OUT_FILE not set")
endif()

file(GLOB THEME_JSON_FILES "${THEMES_DIR}/*.json")
list(SORT THEME_JSON_FILES)

set(OUT_CONTENT
"// Auto-generated from themes/*.json by cmake/embed_themes.cmake — do not edit.
// Edit the source JSON in themes/<id>.json; the generator picks up changes
// on the next build.
#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace arbiter {

")

# Choose a raw-string delimiter that's vanishingly unlikely to appear inside
# a theme JSON document.
set(R_DELIM "ARBITER_THEME")

# Per-file string literals, then an id→pointer table.
# Theme stems may contain hyphens (high-contrast); C identifiers use '_'.
set(TABLE_ENTRIES "")
foreach(json_path IN LISTS THEME_JSON_FILES)
    get_filename_component(stem "${json_path}" NAME_WE)
    string(REPLACE "-" "_" stem_id "${stem}")
    file(READ "${json_path}" json_body)

    # Trim trailing whitespace/newlines so each literal ends cleanly.
    string(REGEX REPLACE "[ \t\r\n]+$" "" json_body "${json_body}")

    string(APPEND OUT_CONTENT
"static constexpr const char* kTheme_${stem_id}_json = R\"${R_DELIM}(${json_body})${R_DELIM}\";\n")
    string(APPEND TABLE_ENTRIES
"    { \"${stem}\", kTheme_${stem_id}_json },\n")
endforeach()

string(APPEND OUT_CONTENT
"
// Lookup table consumed by tui_design.cpp.  Keys are theme ids (file stem).
inline const std::unordered_map<std::string, const char*>& theme_json_table() {
    static const std::unordered_map<std::string, const char*> kTable = {
${TABLE_ENTRIES}    };
    return kTable;
}

inline const char* embedded_theme_json(const std::string& id) {
    const auto& table = theme_json_table();
    auto it = table.find(id);
    return it == table.end() ? nullptr : it->second;
}

inline std::vector<std::string> embedded_theme_names() {
    std::vector<std::string> out;
    out.reserve(theme_json_table().size());
    for (const auto& [id, _] : theme_json_table()) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace arbiter
")

# Only rewrite when the content changed — preserves mtime when nothing did,
# so downstream compilations don't re-run unnecessarily.
if(EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" existing)
    if(existing STREQUAL OUT_CONTENT)
        return()
    endif()
endif()

file(WRITE "${OUT_FILE}" "${OUT_CONTENT}")
message(STATUS "Generated ${OUT_FILE} from ${THEMES_DIR}/*.json")
