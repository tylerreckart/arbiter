# cmake/embed_starters.cmake
#
# Reads every agents/*.json file and emits a header containing each file's
# contents as a raw-string literal plus an id→json table.  Invoked from the
# top-level CMakeLists.txt via add_custom_command, so the header regenerates
# whenever any input JSON changes.
#
# Inputs (passed as -D defines):
#   AGENTS_DIR — absolute path to the agents/ source directory
#   OUT_FILE   — absolute path to the header to write
#
# The generated header lives in the build dir; nothing source-tracked is
# touched.  starters.cpp consumes it via #include "starters_data.h".

if(NOT DEFINED AGENTS_DIR)
    message(FATAL_ERROR "embed_starters.cmake: AGENTS_DIR not set")
endif()
if(NOT DEFINED OUT_FILE)
    message(FATAL_ERROR "embed_starters.cmake: OUT_FILE not set")
endif()

file(GLOB AGENT_JSON_FILES "${AGENTS_DIR}/*.json")
list(SORT AGENT_JSON_FILES)

set(OUT_CONTENT
"// Auto-generated from agents/*.json by cmake/embed_starters.cmake — do not edit.
// Edit the source JSON in agents/<id>.json; the generator picks up changes
// on the next build.
#pragma once

#include <string>
#include <unordered_map>

namespace index_ai {

")

# Choose a raw-string delimiter that's vanishingly unlikely to appear inside
# an agent's prose rules.  ARBITER_STARTER fits that bill.
set(R_DELIM "ARBITER_STARTER")

# Per-file string literals, then an id→pointer table.
set(TABLE_ENTRIES "")
foreach(json_path IN LISTS AGENT_JSON_FILES)
    get_filename_component(stem "${json_path}" NAME_WE)
    file(READ "${json_path}" json_body)

    # Trim trailing whitespace/newlines so each literal ends cleanly.
    string(REGEX REPLACE "[ \t\r\n]+$" "" json_body "${json_body}")

    string(APPEND OUT_CONTENT
"static constexpr const char* kStarter_${stem}_json = R\"${R_DELIM}(${json_body})${R_DELIM}\";\n")
    string(APPEND TABLE_ENTRIES
"    { \"${stem}\", kStarter_${stem}_json },\n")
endforeach()

string(APPEND OUT_CONTENT
"
// Lookup table consumed by starters.cpp.  Keys are agent ids (file stem).
inline const std::unordered_map<std::string, const char*>& starter_json_table() {
    static const std::unordered_map<std::string, const char*> kTable = {
${TABLE_ENTRIES}    };
    return kTable;
}

} // namespace index_ai
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
message(STATUS "Generated ${OUT_FILE} from ${AGENTS_DIR}/*.json")
