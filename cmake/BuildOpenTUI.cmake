# Build or locate libopentui for Phase 0+ TUI integration.
#
# Enable with: cmake -DARBITER_ENABLE_OPENTUI=ON
#
# Requires Zig 0.15.2 (see OpenTUI's .zig-version). Set ARBITER_ZIG_EXECUTABLE
# to a 0.15.2 binary if `zig` on PATH is a different version.
#
# Override source tree: -DARBITER_OPENTUI_ROOT=/path/to/opentui
# Skip rebuild:         -DARBITER_OPENTUI_LIBRARY=/path/to/libopentui.dylib

option(ARBITER_ENABLE_OPENTUI "Link OpenTUI native core (Zig 0.15.2 required)" OFF)

if(NOT ARBITER_ENABLE_OPENTUI)
    return()
endif()

set(_opentui_default_root "$ENV{HOME}/dev/opentui")
if(NOT EXISTS "${_opentui_default_root}/packages/core/src/zig/build.zig")
    set(_opentui_default_root "${CMAKE_SOURCE_DIR}/third_party/opentui")
endif()

set(ARBITER_OPENTUI_ROOT "${_opentui_default_root}" CACHE PATH "OpenTUI repository root")
set(ARBITER_OPENTUI_ZIG_DIR "${ARBITER_OPENTUI_ROOT}/packages/core/src/zig")

if(NOT EXISTS "${ARBITER_OPENTUI_ZIG_DIR}/build.zig")
    message(FATAL_ERROR
        "OpenTUI not found at ${ARBITER_OPENTUI_ROOT}. "
        "Clone https://github.com/anomalyco/opentui or set ARBITER_OPENTUI_ROOT.")
endif()

file(READ "${ARBITER_OPENTUI_ROOT}/.zig-version" ARBITER_OPENTUI_ZIG_VERSION)
string(STRIP "${ARBITER_OPENTUI_ZIG_VERSION}" ARBITER_OPENTUI_ZIG_VERSION)

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_opentui_target "aarch64-macos")
    else()
        set(_opentui_target "x86_64-macos")
    endif()
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_opentui_target "aarch64-windows")
    else()
        set(_opentui_target "x86_64-windows")
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_opentui_target "aarch64-linux")
    else()
        set(_opentui_target "x86_64-linux")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform for OpenTUI integration")
endif()

set(_opentui_built_lib "${ARBITER_OPENTUI_ROOT}/packages/core/lib/${_opentui_target}")
if(APPLE)
    set(_opentui_lib_basename "libopentui.dylib")
elseif(WIN32)
    set(_opentui_lib_basename "opentui.dll")
else()
    set(_opentui_lib_basename "libopentui.so")
endif()

set(ARBITER_OPENTUI_LIBRARY "${_opentui_built_lib}/${_opentui_lib_basename}"
    CACHE FILEPATH "Prebuilt OpenTUI shared library")

if(NOT EXISTS "${ARBITER_OPENTUI_LIBRARY}")
    set(ARBITER_ZIG_EXECUTABLE "zig" CACHE FILEPATH "Zig executable (must be ${ARBITER_OPENTUI_ZIG_VERSION})")
    find_program(_zig_bin NAMES "${ARBITER_ZIG_EXECUTABLE}" zig)
    if(NOT _zig_bin)
        message(FATAL_ERROR "Zig not found. Install ${ARBITER_OPENTUI_ZIG_VERSION} or set ARBITER_ZIG_EXECUTABLE.")
    endif()

    execute_process(
        COMMAND "${_zig_bin}" version
        OUTPUT_VARIABLE _zig_version_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _zig_version_out STREQUAL "${ARBITER_OPENTUI_ZIG_VERSION}")
        message(FATAL_ERROR
            "OpenTUI requires Zig ${ARBITER_OPENTUI_ZIG_VERSION}; found '${_zig_version_out}' at ${_zig_bin}. "
            "Download from https://ziglang.org/download/${ARBITER_OPENTUI_ZIG_VERSION}/ "
            "and pass -DARBITER_ZIG_EXECUTABLE=/path/to/zig")
    endif()

    set(_zig_build_env "")
    if(APPLE)
        execute_process(
            COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE _macos_sdk
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_macos_sdk)
            list(APPEND _zig_build_env "SDKROOT=${_macos_sdk}")
        endif()
    endif()

    set(_zig_optimize "ReleaseFast")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_zig_optimize "Debug")
    endif()

    add_custom_command(
        OUTPUT "${ARBITER_OPENTUI_LIBRARY}"
        COMMAND ${CMAKE_COMMAND} -E env ${_zig_build_env}
            "${_zig_bin}" build -Doptimize=${_zig_optimize}
        WORKING_DIRECTORY "${ARBITER_OPENTUI_ZIG_DIR}"
        COMMENT "Building OpenTUI (${_opentui_target}) with Zig ${_zig_version_out}"
        VERBATIM
    )

    add_custom_target(opentui_core DEPENDS "${ARBITER_OPENTUI_LIBRARY}")
endif()

add_library(OpenTUI::core SHARED IMPORTED GLOBAL)
set_target_properties(OpenTUI::core PROPERTIES
    IMPORTED_LOCATION "${ARBITER_OPENTUI_LIBRARY}"
)

if(TARGET opentui_core)
    add_dependencies(OpenTUI::core opentui_core)
endif()

set(ARBITER_OPENTUI_LIB_DIR "${_opentui_built_lib}")
message(STATUS "OpenTUI: ${ARBITER_OPENTUI_LIBRARY}")
