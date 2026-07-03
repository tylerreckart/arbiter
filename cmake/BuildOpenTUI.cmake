# Build or locate libopentui for Phase 0+ TUI integration.
#
# Enable with: cmake -DARBITER_ENABLE_OPENTUI=ON
#
# On macOS, prebuilt npm binaries are the default (Zig 0.15.2 often cannot run
# `zig build` on newer host OS versions). On Linux, source builds via Zig are
# attempted when prebuilts are disabled.
#
# Override source tree:  -DARBITER_OPENTUI_ROOT=/path/to/opentui
# Pin library path:       -DARBITER_OPENTUI_LIBRARY=/path/to/libopentui.dylib
# Force source build:    -DARBITER_OPENTUI_USE_PREBUILT=OFF

option(ARBITER_ENABLE_OPENTUI "Link OpenTUI native core (required)" ON)

if(NOT ARBITER_ENABLE_OPENTUI)
    message(FATAL_ERROR
        "OpenTUI is required. Reconfigure with -DARBITER_ENABLE_OPENTUI=ON")
endif()

if(APPLE)
    set(_opentui_prebuilt_default ON)
else()
    set(_opentui_prebuilt_default OFF)
endif()
option(ARBITER_OPENTUI_USE_PREBUILT
    "Fetch platform libopentui from npm instead of compiling with Zig"
    ${_opentui_prebuilt_default})

set(ARBITER_OPENTUI_VERSION "0.4.2" CACHE STRING
    "OpenTUI release version for npm prebuilt packages")

set(_opentui_default_root "$ENV{HOME}/dev/opentui")
if(NOT EXISTS "${_opentui_default_root}/packages/core/package.json")
    set(_opentui_default_root "${CMAKE_SOURCE_DIR}/third_party/opentui")
endif()
set(ARBITER_OPENTUI_ROOT "${_opentui_default_root}" CACHE PATH
    "OpenTUI repository root (optional when using prebuilts)")

if(EXISTS "${ARBITER_OPENTUI_ROOT}/packages/core/package.json")
    file(READ "${ARBITER_OPENTUI_ROOT}/packages/core/package.json" _opentui_pkg_json)
    string(JSON _parsed_version GET "${_opentui_pkg_json}" version)
    if(_parsed_version)
        set(ARBITER_OPENTUI_VERSION "${_parsed_version}" CACHE STRING
            "OpenTUI release version for npm prebuilt packages" FORCE)
    endif()
endif()

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_opentui_target "aarch64-macos")
        set(_opentui_npm_pkg "@opentui/core-darwin-arm64")
        set(_opentui_npm_tarball "core-darwin-arm64")
    else()
        set(_opentui_target "x86_64-macos")
        set(_opentui_npm_pkg "@opentui/core-darwin-x64")
        set(_opentui_npm_tarball "core-darwin-x64")
    endif()
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(_opentui_target "aarch64-windows")
        set(_opentui_npm_pkg "@opentui/core-win32-arm64")
        set(_opentui_npm_tarball "core-win32-arm64")
    else()
        set(_opentui_target "x86_64-windows")
        set(_opentui_npm_pkg "@opentui/core-win32-x64")
        set(_opentui_npm_tarball "core-win32-x64")
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_opentui_target "aarch64-linux")
        set(_opentui_npm_pkg "@opentui/core-linux-arm64")
        set(_opentui_npm_tarball "core-linux-arm64")
    else()
        set(_opentui_target "x86_64-linux")
        set(_opentui_npm_pkg "@opentui/core-linux-x64")
        set(_opentui_npm_tarball "core-linux-x64")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform for OpenTUI integration")
endif()

if(APPLE)
    set(_opentui_lib_basename "libopentui.dylib")
elseif(WIN32)
    set(_opentui_lib_basename "opentui.dll")
else()
    set(_opentui_lib_basename "libopentui.so")
endif()

set(_opentui_staging "${CMAKE_BINARY_DIR}/opentui")
set(_opentui_default_lib "${_opentui_staging}/${_opentui_lib_basename}")
if(NOT ARBITER_OPENTUI_LIBRARY)
    set(ARBITER_OPENTUI_LIBRARY "${_opentui_default_lib}"
        CACHE FILEPATH "OpenTUI shared library")
endif()

# --- Prebuilt npm fetch --------------------------------------------------------

function(_arbiter_fetch_opentui_prebuilt npm_pkg tarball_name version dest_lib)
    set(_url "https://registry.npmjs.org/${npm_pkg}/-/${tarball_name}-${version}.tgz")
    set(_staging "${CMAKE_BINARY_DIR}/.opentui-fetch")
    set(_archive "${_staging}/${tarball_name}-${version}.tgz")
    set(_extract "${_staging}/extract")
    file(MAKE_DIRECTORY "${_staging}" "${_extract}" "${_opentui_staging}")

    if(NOT EXISTS "${_archive}")
        message(STATUS "Fetching OpenTUI prebuilt ${npm_pkg}@${version}")
        file(DOWNLOAD "${_url}" "${_archive}" STATUS _dl_status SHOW_PROGRESS)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            message(FATAL_ERROR "Failed to download ${npm_pkg}@${version}: ${_dl_msg}")
        endif()
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extract}")
    set(_src "${_extract}/package/${_opentui_lib_basename}")
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR
            "OpenTUI prebuilt archive did not contain package/${_opentui_lib_basename}")
    endif()

    file(COPY "${_src}" DESTINATION "${_opentui_staging}")
    if(NOT EXISTS "${dest_lib}")
        message(FATAL_ERROR "Failed to stage OpenTUI library at ${dest_lib}")
    endif()
    message(STATUS "OpenTUI prebuilt staged: ${dest_lib}")
endfunction()

# --- Zig source build (optional) -----------------------------------------------

set(ARBITER_OPENTUI_ZIG_DIR "${ARBITER_OPENTUI_ROOT}/packages/core/src/zig")
if(EXISTS "${ARBITER_OPENTUI_ROOT}/.zig-version")
    file(READ "${ARBITER_OPENTUI_ROOT}/.zig-version" ARBITER_OPENTUI_ZIG_VERSION)
    string(STRIP "${ARBITER_OPENTUI_ZIG_VERSION}" ARBITER_OPENTUI_ZIG_VERSION)
else()
    set(ARBITER_OPENTUI_ZIG_VERSION "0.15.2")
endif()

set(ARBITER_ZIG_EXECUTABLE "" CACHE FILEPATH
    "Zig executable for building OpenTUI (must be ${ARBITER_OPENTUI_ZIG_VERSION})")
option(ARBITER_ZIG_AUTO_FETCH
    "Download Zig ${ARBITER_OPENTUI_ZIG_VERSION} into the build dir when not found"
    ON)

function(_arbiter_zig_version zig_bin out_var)
    if(NOT EXISTS "${zig_bin}")
        set("${out_var}" "" PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND "${zig_bin}" version
        OUTPUT_VARIABLE _ver
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc
    )
    if(_rc EQUAL 0)
        set("${out_var}" "${_ver}" PARENT_SCOPE)
    else()
        set("${out_var}" "" PARENT_SCOPE)
    endif()
endfunction()

function(_arbiter_fetch_zig version target_triplet dest_dir zig_bin_out)
    if(WIN32)
        set(_archive_ext "zip")
        set(_url "https://ziglang.org/download/${version}/zig-${target_triplet}-${version}.zip")
    else()
        set(_archive_ext "tar.xz")
        set(_url "https://ziglang.org/download/${version}/zig-${target_triplet}-${version}.tar.xz")
    endif()

    set(_staging "${CMAKE_BINARY_DIR}/.zig-fetch")
    set(_archive "${_staging}/zig-${target_triplet}-${version}.${_archive_ext}")
    set(_extract_root "${_staging}/extract")
    file(MAKE_DIRECTORY "${_staging}" "${_extract_root}")

    message(STATUS "Fetching Zig ${version} from ${_url}")
    file(DOWNLOAD "${_url}" "${_archive}" STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        message(FATAL_ERROR "Failed to download Zig ${version}: ${_dl_msg}")
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extract_root}")
    set(_extracted "${_extract_root}/zig-${target_triplet}-${version}")
    if(NOT EXISTS "${_extracted}/zig")
        file(GLOB _extracted_candidates "${_extract_root}/zig-*/zig")
        if(_extracted_candidates)
            list(GET _extracted_candidates 0 _extracted_zig)
            get_filename_component(_extracted "${_extracted_zig}" DIRECTORY)
        else()
            message(FATAL_ERROR "Zig archive did not contain an expected layout under ${_extract_root}")
        endif()
    endif()

    file(COPY "${_extracted}/" DESTINATION "${dest_dir}")
    set(_zig_bin "${dest_dir}/zig")
    if(WIN32)
        set(_zig_bin "${dest_dir}/zig.exe")
    endif()
    set("${zig_bin_out}" "${_zig_bin}" PARENT_SCOPE)
endfunction()

function(_arbiter_resolve_zig version_required target_triplet zig_bin_out)
    set(_candidates "")
    if(ARBITER_ZIG_EXECUTABLE)
        list(APPEND _candidates "${ARBITER_ZIG_EXECUTABLE}")
    endif()
    list(APPEND _candidates
        "${CMAKE_SOURCE_DIR}/build/zig-${version_required}/zig"
        "${CMAKE_BINARY_DIR}/zig-${version_required}/zig"
        "$ENV{HOME}/.cache/arbiter/zig-${version_required}/zig"
    )
    find_program(_path_zig NAMES zig)
    if(_path_zig)
        list(APPEND _candidates "${_path_zig}")
    endif()

    foreach(_candidate IN LISTS _candidates)
        _arbiter_zig_version("${_candidate}" _candidate_version)
        if(_candidate_version STREQUAL "${version_required}")
            set("${zig_bin_out}" "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    if(ARBITER_ZIG_AUTO_FETCH)
        set(_dest "${CMAKE_BINARY_DIR}/zig-${version_required}")
        _arbiter_fetch_zig("${version_required}" "${target_triplet}" "${_dest}" _fetched)
        _arbiter_zig_version("${_fetched}" _fetched_version)
        if(_fetched_version STREQUAL "${version_required}")
            set("${zig_bin_out}" "${_fetched}" PARENT_SCOPE)
            return()
        endif()
    endif()

    set("${zig_bin_out}" "" PARENT_SCOPE)
endfunction()

# --- Acquire library -----------------------------------------------------------

if(ARBITER_OPENTUI_USE_PREBUILT)
    if(NOT EXISTS "${_opentui_default_lib}")
        _arbiter_fetch_opentui_prebuilt(
            "${_opentui_npm_pkg}"
            "${_opentui_npm_tarball}"
            "${ARBITER_OPENTUI_VERSION}"
            "${_opentui_default_lib}"
        )
    endif()
    set(ARBITER_OPENTUI_LIBRARY "${_opentui_default_lib}" CACHE FILEPATH
        "OpenTUI shared library" FORCE)
    message(STATUS "OpenTUI: using prebuilt ${ARBITER_OPENTUI_LIBRARY}")
elseif(EXISTS "${ARBITER_OPENTUI_LIBRARY}")
    message(STATUS "OpenTUI: using ${ARBITER_OPENTUI_LIBRARY}")
else()
    if(NOT EXISTS "${ARBITER_OPENTUI_ZIG_DIR}/build.zig")
        message(FATAL_ERROR
            "OpenTUI source not found at ${ARBITER_OPENTUI_ROOT}. "
            "Clone https://github.com/anomalyco/opentui, set ARBITER_OPENTUI_ROOT, "
            "or enable ARBITER_OPENTUI_USE_PREBUILT=ON.")
    endif()

    set(_opentui_built_lib "${ARBITER_OPENTUI_ROOT}/packages/core/lib/${_opentui_target}")
    set(_source_lib "${_opentui_built_lib}/${_opentui_lib_basename}")
    set(ARBITER_OPENTUI_LIBRARY "${_source_lib}" CACHE FILEPATH
        "OpenTUI shared library" FORCE)

    _arbiter_resolve_zig("${ARBITER_OPENTUI_ZIG_VERSION}" "${_opentui_target}" _zig_bin)
    if(NOT _zig_bin)
        message(FATAL_ERROR
            "OpenTUI requires Zig ${ARBITER_OPENTUI_ZIG_VERSION}.\n"
            "  • Enable prebuilts: -DARBITER_OPENTUI_USE_PREBUILT=ON (default on macOS), or\n"
            "  • Install Zig from https://ziglang.org/download/${ARBITER_OPENTUI_ZIG_VERSION}/")
    endif()

    set(ARBITER_ZIG_EXECUTABLE "${_zig_bin}" CACHE FILEPATH
        "Zig executable for building OpenTUI (must be ${ARBITER_OPENTUI_ZIG_VERSION})" FORCE)
    _arbiter_zig_version("${_zig_bin}" _zig_version_out)
    message(STATUS "OpenTUI Zig: ${_zig_bin} (${_zig_version_out})")

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
    message(STATUS "OpenTUI: will build ${ARBITER_OPENTUI_LIBRARY}")
endif()

get_filename_component(ARBITER_OPENTUI_LIB_DIR "${ARBITER_OPENTUI_LIBRARY}" DIRECTORY)

add_library(OpenTUI::core SHARED IMPORTED GLOBAL)
set_target_properties(OpenTUI::core PROPERTIES
    IMPORTED_LOCATION "${ARBITER_OPENTUI_LIBRARY}"
)

if(TARGET opentui_core)
    add_dependencies(OpenTUI::core opentui_core)
endif()
