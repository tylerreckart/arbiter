cmake_minimum_required(VERSION 3.20)
project(index_ai VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Platform detection ---
if(APPLE)
    add_compile_definitions(INDEX_AI_MACOS)
elseif(UNIX)
    add_compile_definitions(INDEX_AI_LINUX)
endif()

# --- Sanitizer options (debug builds) ---
option(INDEX_AI_ASAN "Enable AddressSanitizer" OFF)
option(INDEX_AI_TSAN "Enable ThreadSanitizer" OFF)
if(INDEX_AI_ASAN AND INDEX_AI_TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled simultaneously")
endif()
if(INDEX_AI_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(INDEX_AI_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# --- Find system deps ---
find_package(Threads REQUIRED)

# OpenSSL / LibreSSL for TLS (1.1+ required; deprecated init APIs removed)
find_package(OpenSSL 1.1 REQUIRED)

# libcurl for safe HTTP fetching (no shell injection via popen)
find_package(CURL REQUIRED)

# --- Source files ---
set(INDEX_AI_SOURCES
    src/main.cpp
    src/cli_helpers.cpp
    src/repl/queues.cpp
    src/loop_manager.cpp
    src/title_generator.cpp
    src/cli.cpp
    src/tui/tui.cpp
    src/tui/scroll_buffer.cpp
    src/tui/line_editor.cpp
    src/api_client.cpp
    src/agent.cpp
    src/constitution.cpp
    src/orchestrator.cpp
    src/server.cpp
    src/auth.cpp
    src/json.cpp
    src/commands.cpp
    src/readline_wrapper.cpp
    src/cost_tracker.cpp
    src/markdown.cpp
)

add_executable(index ${INDEX_AI_SOURCES})

# Surface the project version to C++ so the welcome card can render it on
# the bottom border.  Keep CMakeLists.txt's `project(... VERSION x.y.z)` in
# sync with your brew formula / release tags.
target_compile_definitions(index PRIVATE INDEX_VERSION="${PROJECT_VERSION}")

target_include_directories(index PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(index PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
)

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
    target_compile_definitions(index PRIVATE INDEX_AI_HAS_READLINE)
    if(NOT READLINE_GNU)
        target_compile_definitions(index PRIVATE INDEX_AI_USE_EDITLINE)
    endif()
    target_include_directories(index PRIVATE ${EDITLINE_INCLUDE})
    target_link_libraries(index PRIVATE ${EDITLINE_LIB})
else()
    message(WARNING "readline/libedit not found; falling back to raw getline (no line editing)")
endif()

# --- Install ---
install(TARGETS index RUNTIME DESTINATION bin)
install(DIRECTORY agents/ DESTINATION share/index/agents
        FILES_MATCHING PATTERN "*.json")

# --- Tests ---
# PTY-based integration tests that drive the real `index` binary through a
# pseudo-terminal.  Enabled by default; disable with -DINDEX_BUILD_TESTS=OFF
# if you're cross-compiling or just don't want the extra target.
option(INDEX_BUILD_TESTS "Build TUI integration tests" ON)
if(INDEX_BUILD_TESTS)
    enable_testing()

    add_executable(index_tests
        tests/pty_harness.cpp
        tests/test_tui.cpp
        tests/test_line_editor.cpp
    )
    target_include_directories(index_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    # Absolute path so tests find the binary regardless of CWD when ctest runs.
    target_compile_definitions(index_tests PRIVATE
        INDEX_TEST_BINARY="$<TARGET_FILE:index>"
    )
    # forkpty() lives in libutil on Linux; Darwin has it in libSystem so no
    # link flag needed, but target_link_libraries for libutil is a no-op
    # there — safe either way.
    if(UNIX AND NOT APPLE)
        target_link_libraries(index_tests PRIVATE util)
    endif()

    add_test(NAME tui_integration COMMAND index_tests)
    # The tests spawn index; they'll fail fast if the binary isn't built yet.
    add_dependencies(index_tests index)

    # --- Unit tests for security-critical command functions ---
    add_executable(unit_commands
        tests/test_commands.cpp
        src/commands.cpp
    )
    target_include_directories(unit_commands PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    target_link_libraries(unit_commands PRIVATE CURL::libcurl)
    add_test(NAME unit_commands COMMAND unit_commands)

    # --- Unit tests for API client parsing logic ---
    add_executable(unit_api_client
        tests/test_api_client.cpp
    )
    target_include_directories(unit_api_client PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/doctest
    )
    add_test(NAME unit_api_client COMMAND unit_api_client)
endif()
