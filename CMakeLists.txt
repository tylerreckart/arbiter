cmake_minimum_required(VERSION 3.20)
project(arbiter VERSION 0.7.3 LANGUAGES CXX)

# CMake's project(... VERSION x.y.z) only accepts numeric semver, so the
# prerelease suffix (-beta2, -rc1, …) lives in its own variable and is
# concatenated for the display string surfaced through INDEX_VERSION.
# Leave empty (or unset) for a final release.
set(ARBITER_VERSION_SUFFIX "")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Default build type ---
# Single-config generators (make, ninja) silently emit -O0 when
# CMAKE_BUILD_TYPE is unset, which we discovered costs ~20-30% on every
# hot path (TLS, JSON parse, HTML strip, markdown render).  Default to
# Release so fresh clones get optimized binaries; developers can still
# override with -DCMAKE_BUILD_TYPE=Debug or -DCMAKE_BUILD_TYPE=RelWithDebInfo.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# --- Link-time optimization ---
# LTO is a clean win for a mostly-single-binary project; probe availability
# first since some older toolchains can't do it, and skip when sanitizers
# are enabled (they don't compose cleanly with LTO).
include(CheckIPOSupported)
check_ipo_supported(RESULT HAVE_IPO OUTPUT IPO_ERROR)

# --- Platform detection ---
if(APPLE)
    add_compile_definitions(ARBITER_MACOS)
elseif(UNIX)
    add_compile_definitions(ARBITER_LINUX)
endif()

# --- Sanitizer options (debug builds) ---
option(ARBITER_ASAN "Enable AddressSanitizer" OFF)
option(ARBITER_TSAN "Enable ThreadSanitizer" OFF)
if(ARBITER_ASAN AND ARBITER_TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled simultaneously")
endif()
if(ARBITER_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(ARBITER_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# --- OpenTUI (required) ---
include(${CMAKE_SOURCE_DIR}/cmake/BuildOpenTUI.cmake)

# --- Find system deps ---
find_package(Threads REQUIRED)

include(${CMAKE_SOURCE_DIR}/cmake/HomebrewHints.cmake)

# OpenSSL / LibreSSL for TLS (1.1+ required; deprecated init APIs removed)
find_package(OpenSSL 1.1)
if(NOT OpenSSL_FOUND)
    if(APPLE)
        message(FATAL_ERROR
            "OpenSSL not found.\n"
            "  brew install openssl@3\n"
            "  cmake -B build -DOPENSSL_ROOT_DIR=\"$(brew --prefix openssl@3)\" ...")
    else()
        message(FATAL_ERROR
            "OpenSSL not found. On Debian/Ubuntu: sudo apt install libssl-dev")
    endif()
endif()

# libcurl for safe HTTP fetching (no shell injection via popen)
find_package(CURL REQUIRED)

# SQLite3 for the tenant + usage-accounting store backing `arbiter --api`.
# Ships in the macOS SDK; on Linux the `libsqlite3-dev` package provides it.
find_package(SQLite3 REQUIRED)

# CMake's FindSQLite3 module canonicalised the imported-target name from
# `SQLite::SQLite3` to `SQLite3::SQLite3` in CMake 4.3.  Older CMake
# versions (notably Ubuntu 22.04's apt CMake 3.22, which we use in CI)
# only expose the `SQLite::` form.  Alias the new name to the old when
# only the old exists so the rest of the CMake graph can use the
# canonical `SQLite3::SQLite3` everywhere.
if(NOT TARGET SQLite3::SQLite3 AND TARGET SQLite::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

# --- Source files ---
set(ARBITER_SOURCES
    src/main.cpp
    src/cli_helpers.cpp
    src/repl/queues.cpp
    src/repl/layout.cpp
    src/repl/pane_history.cpp
    src/repl/conversation_store.cpp
    src/repl/conversation_titling.cpp
    src/repl/transcript_replay.cpp
    src/repl/transcript_replay_predicates.cpp
    src/atomic_file.cpp
    src/loop_manager.cpp
    src/title_generator.cpp
    src/cli.cpp
    src/starters.cpp
    src/theme.cpp
    src/styled_text.cpp
    src/render_policy.cpp
    src/stream_renderer.cpp
    src/tui/tui.cpp
    src/tui/tui_design.cpp
    src/tui/style_resolver.cpp
    src/tui/block_parser.cpp
    src/tui/ansi_util.cpp
    src/tui/stream_filter.cpp
    src/api_client.cpp
    src/agent.cpp
    src/constitution.cpp
    src/orchestrator.cpp
    src/advisor_gate.cpp
    src/advisor.cpp
    src/api_server.cpp
    src/sandbox.cpp
    src/idempotency_cache.cpp
    src/metrics.cpp
    src/circuit_breaker.cpp
    src/logger.cpp
    src/tenant_limiter.cpp
    src/tenant_store.cpp
    src/schedule_parser.cpp
    src/notification_bus.cpp
    src/request_event_bus.cpp
    src/scheduler.cpp
    src/json.cpp
    src/commands.cpp
    src/mcp/types.cpp
    src/mcp/subprocess.cpp
    src/mcp/client.cpp
    src/mcp/manager.cpp
    src/a2a/types.cpp
    src/a2a/server.cpp
    src/a2a/event_translator.cpp
    src/a2a/sse_reader.cpp
    src/a2a/http.cpp
    src/a2a/client.cpp
    src/a2a/manager.cpp
    src/readline_wrapper.cpp
    src/markdown.cpp
    src/code_highlighter.cpp
)

# --- Starter agent definitions: embed agents/*.json into the binary -------
# `arbiter --init` and the first-run wizard ship the same starter agents
# users can edit on disk later.  The JSON files in agents/ are the single
# source of truth; this build step embeds them as raw-string literals so
# the binary is self-contained and the C++ side has no shadow definitions
# that could drift.  Header regenerates whenever any input JSON changes.
set(STARTERS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(STARTERS_GENERATED_HEADER "${STARTERS_GENERATED_DIR}/starters_data.h")
file(MAKE_DIRECTORY "${STARTERS_GENERATED_DIR}")
file(GLOB STARTER_JSON_INPUTS "${CMAKE_SOURCE_DIR}/agents/*.json")

add_custom_command(
    OUTPUT ${STARTERS_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DAGENTS_DIR=${CMAKE_SOURCE_DIR}/agents
        -DOUT_FILE=${STARTERS_GENERATED_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_starters.cmake
    DEPENDS
        ${STARTER_JSON_INPUTS}
        ${CMAKE_SOURCE_DIR}/cmake/embed_starters.cmake
    COMMENT "Embedding agents/*.json into starters_data.h"
    VERBATIM
)

# Phony target so anything that needs the embedded header can depend on it.
add_custom_target(starters_data_header DEPENDS ${STARTERS_GENERATED_HEADER})

add_executable(arbiter ${ARBITER_SOURCES})
add_dependencies(arbiter starters_data_header)
target_include_directories(arbiter PRIVATE ${STARTERS_GENERATED_DIR})

# Surface the project version to C++ (sidebar version label, etc.).
# the bottom border.  Keep CMakeLists.txt's `project(... VERSION x.y.z)`
# and ARBITER_VERSION_SUFFIX in sync with your brew formula / release tags.
target_compile_definitions(arbiter PRIVATE
    INDEX_VERSION="${PROJECT_VERSION}${ARBITER_VERSION_SUFFIX}")

# Enable IPO/LTO on Release-ish configs when the toolchain supports it and
# no sanitizer is active.  Guarded here (not globally) so tests and dev
# builds don't pay the extra link cost.
if(HAVE_IPO AND NOT ARBITER_ASAN AND NOT ARBITER_TSAN)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR
       CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR
       CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set_target_properties(arbiter PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

target_include_directories(arbiter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(arbiter PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    SQLite3::SQLite3
)

target_sources(arbiter PRIVATE
    src/tui/opentui/engine.cpp
    src/tui/opentui/session.cpp
    src/tui/opentui/pane_scroll_view.cpp
    src/tui/opentui/diff_panel.cpp
    src/tui/opentui/ansi_scroll_append.cpp
    src/tui/opentui/span_scroll_append.cpp
    src/tui/opentui/pane_input_editor.cpp
    src/tui/palette.cpp
    src/tui/sidebar.cpp
    src/tui/sidebar_format.cpp
    src/tui/history_sidebar.cpp
    src/tui/opentui/sidebar_frame.cpp
    src/tui/opentui/history_sidebar_frame.cpp
    src/tui/opentui/top_header_frame.cpp
    src/tui/opentui/pane_frame.cpp
)
target_link_libraries(arbiter PRIVATE OpenTUI::core)
if(APPLE)
    target_link_libraries(arbiter PRIVATE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
    target_link_options(arbiter PRIVATE "-Wl,-ld_classic")
    set_target_properties(arbiter PROPERTIES
        BUILD_RPATH "${ARBITER_OPENTUI_LIB_DIR}"
        INSTALL_RPATH "@executable_path/../lib/opentui"
    )
elseif(UNIX)
    target_link_libraries(arbiter PRIVATE dl pthread)
    set_target_properties(arbiter PROPERTIES
        BUILD_RPATH "${ARBITER_OPENTUI_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN/../lib/opentui"
    )
endif()

# --- Readline / libedit for interactive line editing ---
# Try libedit first (BSD license, ships on macOS)
find_library(EDITLINE_LIB edit)
find_path(EDITLINE_INCLUDE editline/readline.h)

if(NOT EDITLINE_LIB OR NOT EDITLINE_INCLUDE)
    find_library(EDITLINE_LIB readline)
    find_path(EDITLINE_INCLUDE readline/readline.h)
    if(EDITLINE_LIB AND EDITLINE_INCLUDE)
        set(READLINE_GNU ON)
    endif()
endif()

if(EDITLINE_LIB AND EDITLINE_INCLUDE)
    message(STATUS "Found readline/libedit: ${EDITLINE_LIB}")
    target_compile_definitions(arbiter PRIVATE ARBITER_HAS_READLINE)
    if(NOT READLINE_GNU)
        target_compile_definitions(arbiter PRIVATE ARBITER_USE_EDITLINE)
    endif()
    target_include_directories(arbiter PRIVATE ${EDITLINE_INCLUDE})
    target_link_libraries(arbiter PRIVATE ${EDITLINE_LIB})
else()
    message(WARNING "readline/libedit not found; falling back to raw getline (no line editing)")
endif()

# --- Install ---
install(TARGETS arbiter RUNTIME DESTINATION bin)
install(DIRECTORY agents/ DESTINATION share/arbiter/agents
        FILES_MATCHING PATTERN "*.json")
install(DIRECTORY themes/ DESTINATION share/arbiter/themes
        FILES_MATCHING PATTERN "*.json")

target_compile_definitions(arbiter PRIVATE
    ARBITER_THEMES_DIR="${CMAKE_SOURCE_DIR}/themes")

# --- Tests ---
# PTY-based integration tests that drive the real `arbiter` binary through
# a pseudo-terminal.  Enabled by default; disable with -DINDEX_BUILD_TESTS=OFF
# if you're cross-compiling or just don't want the extra target.
option(INDEX_BUILD_TESTS "Build PTY integration tests" ON)
if(INDEX_BUILD_TESTS)
    enable_testing()

    add_executable(line_editor_tests
        tests/pty_harness.cpp
        tests/test_line_editor.cpp
    )
    target_include_directories(line_editor_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    # Absolute path so tests find the binary regardless of CWD when ctest runs.
    target_compile_definitions(line_editor_tests PRIVATE
        INDEX_TEST_BINARY="$<TARGET_FILE:arbiter>"
    )
    # forkpty() lives in libutil on Linux; Darwin has it in libSystem so no
    # link flag needed, but target_link_libraries for libutil is a no-op
    # there — safe either way.
    if(UNIX AND NOT APPLE)
        target_link_libraries(line_editor_tests PRIVATE util)
    endif()

    add_test(NAME line_editor COMMAND line_editor_tests)
    # The tests spawn arbiter; they'll fail fast if the binary isn't built yet.
    add_dependencies(line_editor_tests arbiter)

    add_executable(sidebar_tui_tests
        tests/pty_harness.cpp
        tests/test_sidebar_tui.cpp
    )
    target_include_directories(sidebar_tui_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_compile_definitions(sidebar_tui_tests PRIVATE
        INDEX_TEST_BINARY="$<TARGET_FILE:arbiter>"
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(sidebar_tui_tests PRIVATE util)
    endif()
    add_test(NAME sidebar_tui COMMAND sidebar_tui_tests)

    add_executable(transcript_replay_tui_tests
        tests/pty_harness.cpp
        tests/test_transcript_replay_tui.cpp
    )
    target_include_directories(transcript_replay_tui_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_compile_definitions(transcript_replay_tui_tests PRIVATE
        INDEX_TEST_BINARY="$<TARGET_FILE:arbiter>"
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(transcript_replay_tui_tests PRIVATE util)
    endif()
    add_test(NAME transcript_replay_tui COMMAND transcript_replay_tui_tests)
    add_dependencies(transcript_replay_tui_tests arbiter)
    add_dependencies(sidebar_tui_tests arbiter)

    add_executable(chat_command_tui_tests
        tests/pty_harness.cpp
        tests/test_chat_command_tui.cpp
    )
    target_include_directories(chat_command_tui_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_compile_definitions(chat_command_tui_tests PRIVATE
        INDEX_TEST_BINARY="$<TARGET_FILE:arbiter>"
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(chat_command_tui_tests PRIVATE util)
    endif()
    add_test(NAME chat_command_tui COMMAND chat_command_tui_tests)
    add_dependencies(chat_command_tui_tests arbiter)

    # --- Unit tests for security-critical command functions ---
    add_executable(unit_commands
        tests/test_commands.cpp
        src/commands.cpp
    )
    target_include_directories(unit_commands PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_commands PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME unit_commands COMMAND unit_commands)

    # --- Unit tests for API client parsing logic ---
    # Links the real ApiClient + json so the new vision-related tests exercise
    # the actual build_body_* static methods (rather than mirroring the logic
    # like parse_host has to, since it's file-static in api_client.cpp).
    add_executable(unit_api_client
        tests/test_api_client.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/json.cpp
    )
    target_include_directories(unit_api_client PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_api_client PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )
    add_test(NAME unit_api_client COMMAND unit_api_client)

    # Concurrency regression test for issue #48 — its own binary so the
    # provider registry initializes against this test's loopback OLLAMA_HOST
    # rather than a default already pinned by unit_api_client.
    add_executable(unit_api_client_pool
        tests/test_api_client_pool.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/json.cpp
    )
    target_include_directories(unit_api_client_pool PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_api_client_pool PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )
    add_test(NAME unit_api_client_pool COMMAND unit_api_client_pool)

    # --- Unit tests for the tool-call stream filter ---
    # Exercises line-buffered reassembly, /cmd swallow rules, and the
    # verbose pass-through path without needing a TUI or the real orchestrator.
    add_executable(unit_stream_filter
        tests/test_stream_filter.cpp
        src/tui/stream_filter.cpp
        src/tui/block_parser.cpp
    )
    target_include_directories(unit_stream_filter PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_stream_filter COMMAND unit_stream_filter)

    add_executable(unit_markdown_diff
        tests/test_markdown_diff.cpp
        src/markdown.cpp
        src/styled_text.cpp
        src/render_policy.cpp
        src/theme.cpp
        src/tui/tui_design.cpp
        src/tui/style_resolver.cpp
        src/json.cpp
    )
    target_include_directories(unit_markdown_diff PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_compile_definitions(unit_markdown_diff PRIVATE
        ARBITER_THEMES_DIR="${CMAKE_SOURCE_DIR}/themes")
    add_test(NAME unit_markdown_diff COMMAND unit_markdown_diff)

    add_executable(unit_code_highlighter
        tests/test_code_highlighter.cpp
        src/code_highlighter.cpp
        src/styled_text.cpp
        src/theme.cpp
        src/tui/tui_design.cpp
        src/tui/style_resolver.cpp
        src/json.cpp
    )
    target_include_directories(unit_code_highlighter PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_code_highlighter COMMAND unit_code_highlighter)

    add_executable(unit_tui_theme_json
        tests/test_tui_theme_json.cpp
        src/tui/tui_design.cpp
        src/json.cpp
    )
    target_include_directories(unit_tui_theme_json PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_compile_definitions(unit_tui_theme_json PRIVATE
        ARBITER_THEMES_DIR="${CMAKE_SOURCE_DIR}/themes")
    add_test(NAME unit_tui_theme_json COMMAND unit_tui_theme_json)

    add_executable(unit_palette
        tests/test_palette.cpp
        src/tui/palette.cpp
    )
    target_include_directories(unit_palette PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_palette COMMAND unit_palette)

    add_executable(unit_kitty_key_decode
        tests/test_kitty_key_decode.cpp
    )
    target_include_directories(unit_kitty_key_decode PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_kitty_key_decode COMMAND unit_kitty_key_decode)

    add_executable(unit_mouse_decode
        tests/test_mouse_decode.cpp
    )
    target_include_directories(unit_mouse_decode PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_mouse_decode COMMAND unit_mouse_decode)

    add_executable(unit_sidebar_format
        tests/test_sidebar_format.cpp
        src/tui/sidebar_format.cpp
    )
    target_include_directories(unit_sidebar_format PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_sidebar_format COMMAND unit_sidebar_format)

    add_executable(unit_sidebar
        tests/test_sidebar.cpp
        src/tui/sidebar.cpp
        src/tui/sidebar_format.cpp
    )
    target_include_directories(unit_sidebar PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    add_test(NAME unit_sidebar COMMAND unit_sidebar)

    # --- Unit tests for the history sidebar (id-pinned selection across
    # reorder, key->action table including rename/delete/PgUp/PgDn). Same
    # dependency set as unit_conversation_store since HistorySidebarState
    # holds a ConversationStore reference.
    add_executable(unit_history_sidebar
        tests/test_history_sidebar.cpp
        tests/orchestrator_session_stub.cpp
        src/tui/history_sidebar.cpp
        src/repl/conversation_store.cpp
        src/repl/conversation_titling.cpp
        src/atomic_file.cpp
        src/json.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/styled_text.cpp
        src/theme.cpp
        src/tui/tui_design.cpp
        src/tui/style_resolver.cpp
    )
    target_include_directories(unit_history_sidebar PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_history_sidebar PRIVATE
        Threads::Threads
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME unit_history_sidebar COMMAND unit_history_sidebar)

    # --- Unit tests for the conversation-history store (persistence, atomic
    # writes, manifest recovery, soft delete, titling). Links a stub for the
    # two Orchestrator methods it calls instead of the full orchestrator.cpp
    # dependency chain — see tests/orchestrator_session_stub.cpp. Still
    # needs api_client.cpp + friends because conversation_store.cpp's title
    # job calls ApiClient::complete() directly (no Orchestrator construction
    # required for that symbol to resolve).
    add_executable(unit_conversation_store
        tests/test_conversation_store.cpp
        tests/orchestrator_session_stub.cpp
        src/repl/conversation_store.cpp
        src/repl/conversation_titling.cpp
        src/atomic_file.cpp
        src/json.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/styled_text.cpp
        src/theme.cpp
        src/tui/tui_design.cpp
        src/tui/style_resolver.cpp
    )
    target_include_directories(unit_conversation_store PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_conversation_store PRIVATE
        Threads::Threads
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME unit_conversation_store COMMAND unit_conversation_store)

    # --- Autosave tests: exercise save_async/flush against a real (but
    # network-inert) Orchestrator. Needs ApiClient's real constructor, so
    # this links api_client.cpp + its own deps — see
    # tests/orchestrator_constructible_stub.cpp for why that's still far
    # short of the full orchestrator.cpp dependency chain.
    add_executable(unit_conversation_store_autosave
        tests/test_conversation_store_autosave.cpp
        tests/orchestrator_constructible_stub.cpp
        src/repl/conversation_store.cpp
        src/repl/conversation_titling.cpp
        src/atomic_file.cpp
        src/json.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/styled_text.cpp
        src/theme.cpp
        src/tui/tui_design.cpp
        src/tui/style_resolver.cpp
    )
    target_include_directories(unit_conversation_store_autosave PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_conversation_store_autosave PRIVATE
        Threads::Threads
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME unit_conversation_store_autosave COMMAND unit_conversation_store_autosave)

    # --- Unit tests for conversation titling (deterministic title, model-
    # reply sanitizer) — pure string functions, no Orchestrator needed.
    add_executable(unit_conversation_titling
        tests/test_conversation_titling.cpp
        src/repl/conversation_titling.cpp
        src/styled_text.cpp
        src/theme.cpp
        src/tui/tui_design.cpp
        src/tui/style_resolver.cpp
        src/json.cpp
    )
    target_include_directories(unit_conversation_titling PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_conversation_titling COMMAND unit_conversation_titling)

    # --- Unit tests for transcript-replay's pure logic (tail-window math,
    # tool-result/pane-result noise filtering) — no OpenTUI dependency.
    add_executable(unit_transcript_replay_predicates
        tests/test_transcript_replay_predicates.cpp
        src/repl/transcript_replay_predicates.cpp
    )
    target_include_directories(unit_transcript_replay_predicates PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    add_test(NAME unit_transcript_replay_predicates COMMAND unit_transcript_replay_predicates)

    # --- Unit tests for the natural-language schedule parser ---
    add_executable(unit_schedule_parser
        tests/test_schedule_parser.cpp
        src/schedule_parser.cpp
    )
    target_include_directories(unit_schedule_parser PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_schedule_parser COMMAND unit_schedule_parser)

    # --- Unit tests for the agent-facing todo store ---
    add_executable(unit_todos
        tests/test_todos.cpp
        src/tenant_store.cpp
    )
    target_include_directories(unit_todos PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_todos PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
    )
    add_test(NAME unit_todos COMMAND unit_todos)

    # --- Unit tests for the agent-scoped lesson store ---
    add_executable(unit_lessons
        tests/test_lessons.cpp
        src/tenant_store.cpp
    )
    target_include_directories(unit_lessons PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_lessons PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
    )
    add_test(NAME unit_lessons COMMAND unit_lessons)

    # --- Unit tests for the durable request log (resubscribe surface) ---
    add_executable(unit_request_log
        tests/test_request_log.cpp
        src/tenant_store.cpp
    )
    target_include_directories(unit_request_log PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_request_log PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
    )
    add_test(NAME unit_request_log COMMAND unit_request_log)

    # --- Unit tests for the per-tenant rate / concurrency limiter ---
    add_executable(unit_tenant_limiter
        tests/test_tenant_limiter.cpp
        src/tenant_limiter.cpp
    )
    target_include_directories(unit_tenant_limiter PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_tenant_limiter COMMAND unit_tenant_limiter)

    # --- Unit tests for the Idempotency-Key dedup cache ---
    add_executable(unit_idempotency_cache
        tests/test_idempotency_cache.cpp
        src/idempotency_cache.cpp
    )
    target_include_directories(unit_idempotency_cache PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_idempotency_cache COMMAND unit_idempotency_cache)

    # --- Unit tests for the Prometheus-style metrics registry ---
    add_executable(unit_metrics
        tests/test_metrics.cpp
        src/metrics.cpp
    )
    target_include_directories(unit_metrics PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_metrics COMMAND unit_metrics)

    # --- Unit tests for the provider circuit breaker ---
    add_executable(unit_circuit_breaker
        tests/test_circuit_breaker.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
    )
    target_include_directories(unit_circuit_breaker PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_circuit_breaker COMMAND unit_circuit_breaker)

    # --- Unit tests for the structured logger ---
    add_executable(unit_logger
        tests/test_logger.cpp
        src/logger.cpp
    )
    target_include_directories(unit_logger PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_logger COMMAND unit_logger)

    # --- Unit tests for the structured-memory store ---
    # Pins the tenant_store CRUD contract that backs both /v1/memory/entries
    # and the agent-side /mem entries|entry|search reader.
    add_executable(unit_memory_entries
        tests/test_memory_entries.cpp
        src/tenant_store.cpp
    )
    target_include_directories(unit_memory_entries PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_memory_entries PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
    )
    add_test(NAME unit_memory_entries COMMAND unit_memory_entries)

    # Per-tenant agent catalog (POST/GET/PATCH/DELETE /v1/agents storage).
    add_executable(unit_tenant_agents
        tests/test_tenant_agents.cpp
        src/tenant_store.cpp
    )
    target_include_directories(unit_tenant_agents PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_tenant_agents PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
    )
    add_test(NAME unit_tenant_agents COMMAND unit_tenant_agents)

    # Per-tenant + per-conversation artifact store (POST/GET/DELETE
    # /v1/.../artifacts and the agent /write --persist surface).
    add_executable(unit_artifacts
        tests/test_artifacts.cpp
        src/tenant_store.cpp
    )
    target_include_directories(unit_artifacts PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_artifacts PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite3::SQLite3
    )
    add_test(NAME unit_artifacts COMMAND unit_artifacts)

    # MCP layer (JSON-RPC framing, subprocess wrapper, /mcp dispatch).
    # Live-server tests are intentionally not wired here — manual smoke
    # via `arbiter --api` covers the playwright path.
    add_executable(unit_mcp
        tests/test_mcp.cpp
        src/mcp/types.cpp
        src/mcp/subprocess.cpp
        src/commands.cpp
        src/json.cpp
    )
    target_include_directories(unit_mcp PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_mcp PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME unit_mcp COMMAND unit_mcp)

    # A2A protocol layer.  Five slices exercised in-process:
    #   • wire-type round-trips for every public struct;
    #   • JSON-RPC envelope serialise + parse + error shaping;
    #   • SSE parser handling chunked feeds and field syntax;
    #   • agent-card builder (Constitution → AgentCard);
    #   • event translator emitting JSON-RPC-wrapped status/artifact frames.
    # Live conformance against the upstream a2a-tck suite lives outside CMake
    # — it talks to a running `arbiter --api` and is run manually.
    add_executable(unit_a2a
        tests/test_a2a.cpp
        src/a2a/types.cpp
        src/a2a/server.cpp
        src/a2a/event_translator.cpp
        src/a2a/sse_reader.cpp
        src/constitution.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/json.cpp
    )
    target_include_directories(unit_a2a PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_a2a PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )
    add_test(NAME unit_a2a COMMAND unit_a2a)

    # Constitution / prompt composer.  Pins the capability-bundle resolution
    # and verifies that empty-capabilities still emits the legacy surface
    # (back-compat for the master orchestrator).
    add_executable(unit_constitution
        tests/test_constitution.cpp
        src/constitution.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/json.cpp
    )
    target_include_directories(unit_constitution PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_constitution PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )
    add_test(NAME unit_constitution COMMAND unit_constitution)

    # Per-conversation Agent history isolation (ConversationScope / #40).
    add_executable(unit_agent_conversation
        tests/test_agent_conversation.cpp
        src/agent.cpp
        src/constitution.cpp
        src/api_client.cpp
        src/circuit_breaker.cpp
        src/metrics.cpp
        src/json.cpp
    )
    target_include_directories(unit_agent_conversation PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
        ${OPENSSL_INCLUDE_DIR}
    )
    target_link_libraries(unit_agent_conversation PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        Threads::Threads
    )
    add_test(NAME unit_agent_conversation COMMAND unit_agent_conversation)

    # Advisor gate signal parser.  The parser is the only thing translating
    # an advisor's reply into a CONTINUE/REDIRECT/HALT decision; format
    # drift here means the runtime gate silently misroutes.  Kept as a
    # dedicated tiny target so the parser lives independently of the
    # orchestrator's heavy dependency graph.
    add_executable(unit_advisor_gate
        tests/test_advisor_gate.cpp
        src/advisor_gate.cpp
    )
    target_include_directories(unit_advisor_gate PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_link_libraries(unit_advisor_gate PRIVATE CURL::libcurl)
    add_test(NAME unit_advisor_gate COMMAND unit_advisor_gate)
endif()
