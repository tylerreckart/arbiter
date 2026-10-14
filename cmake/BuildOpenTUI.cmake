# Build or locate libopentui for Phase 0+ TUI integration.
#
# Enable with: cmake -DARBITER_ENABLE_OPENTUI=ON
#
# Prebuilt npm binaries are the default (no local opentui clone or Zig required).
# Disable with -DARBITER_OPENTUI_USE_PREBUILT=OFF to compile from source.
#
# Override source tree:  -DARBITER_OPENTUI_ROOT=/path/to/opentui
# Pin library path:       -DARBITER_OPENTUI_LIBRARY=/path/to/libopentui.dylib
# Force source build:    -DARBITER_OPENTUI_USE_PREBUILT=OFF

option(ARBITER_ENABLE_OPENTUI "Link OpenTUI native core (required)" ON)

if(NOT ARBITER_ENABLE_OPENTUI)
    message(FATAL_ERROR
        "OpenTUI is required. Reconfigure with -DARBITER_ENABLE_OPENTUI=ON")
endif()

option(ARBITER_OPENTUI_USE_PREBUILT
    "Fetch platform libopentui from npm instead of compiling with Zig"
    ON)

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

# --- Pinned downloads ----------------------------------------------------------
# Digests match the bytes at these exact versions. A new OpenTUI or Zig
# release needs a new pin (or -DARBITER_OPENTUI_SHA256 / -DARBITER_ZIG_SHA256).
# Mismatch deletes the partial and fails configure — OpenTUI is required,
# so this does not fall back the way the optional ONNX embed fetch does.

include("${CMAKE_CURRENT_LIST_DIR}/VerifyDownload.cmake")

set(ARBITER_OPENTUI_SHA256 "" CACHE STRING
    "SHA256 override for the OpenTUI prebuilt tarball (required when the version is not pinned)")
set(ARBITER_ZIG_SHA256 "" CACHE STRING
    "SHA256 override for the Zig archive (required when the version is not pinned)")

function(_arbiter_lookup_sha256 kind key out_var)
    if(kind STREQUAL "opentui" AND NOT "${ARBITER_OPENTUI_SHA256}" STREQUAL "")
        set("${out_var}" "${ARBITER_OPENTUI_SHA256}" PARENT_SCOPE)
        return()
    endif()
    if(kind STREQUAL "zig" AND NOT "${ARBITER_ZIG_SHA256}" STREQUAL "")
        set("${out_var}" "${ARBITER_ZIG_SHA256}" PARENT_SCOPE)
        return()
    endif()

    set(_sha "")
    if(kind STREQUAL "opentui")
        if(key STREQUAL "core-darwin-arm64@0.4.2")
            set(_sha "7d20003cafd2f24944851b21eefa11b9e1e3bda6111322dc93ac57df9e7dbc05")
        elseif(key STREQUAL "core-darwin-x64@0.4.2")
            set(_sha "bcea753a24fbeaf711ac3f95bb8e1190ef3ec6687f43bcb7d670c30f8eaaac0f")
        elseif(key STREQUAL "core-win32-arm64@0.4.2")
            set(_sha "f086763ce0341e7569a389eaa479a760918d8363a2c4813f837484b52e4f5390")
        elseif(key STREQUAL "core-win32-x64@0.4.2")
            set(_sha "a0c228a09637087e63fbcc0bcebbe93654be1e02e5fe77b618f541e85296fdf9")
        elseif(key STREQUAL "core-linux-arm64@0.4.2")
            set(_sha "2438b79abc5db661484b030c078e1c32ff0087be4d63407d6ae037a33c48f62e")
        elseif(key STREQUAL "core-linux-x64@0.4.2")
            set(_sha "1a4473663c4ff06cce1d660fee1673fc32bfe18f876420eac9b7a438970003c6")
        endif()
    elseif(kind STREQUAL "zig")
        if(key STREQUAL "x86_64-linux@0.15.2")
            set(_sha "02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239")
        elseif(key STREQUAL "aarch64-linux@0.15.2")
            set(_sha "958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f")
        elseif(key STREQUAL "aarch64-macos@0.15.2")
            set(_sha "3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b")
        elseif(key STREQUAL "x86_64-macos@0.15.2")
            set(_sha "375b6909fc1495d16fc2c7db9538f707456bfc3373b14ee83fdd3e22b3d43f7f")
        elseif(key STREQUAL "x86_64-windows@0.15.2")
            set(_sha "3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c")
        elseif(key STREQUAL "aarch64-windows@0.15.2")
            set(_sha "b926465f8872bf983422257cd9ec248bb2b270996fbe8d57872cca13b56fc370")
        endif()
    endif()

    if(_sha STREQUAL "")
        message(FATAL_ERROR
            "No SHA256 pin for ${kind} '${key}'. "
            "Pass -DARBITER_OPENTUI_SHA256= or -DARBITER_ZIG_SHA256= for an unpinned version.")
    endif()
    set("${out_var}" "${_sha}" PARENT_SCOPE)
endfunction()

# --- Prebuilt npm fetch --------------------------------------------------------

function(_arbiter_fetch_opentui_prebuilt npm_pkg tarball_name version dest_lib)
    set(_url "https://registry.npmjs.org/${npm_pkg}/-/${tarball_name}-${version}.tgz")
    set(_staging "${CMAKE_BINARY_DIR}/.opentui-fetch")
    set(_archive "${_staging}/${tarball_name}-${version}.tgz")
    set(_extract "${_staging}/extract")
    file(MAKE_DIRECTORY "${_staging}" "${_extract}" "${_opentui_staging}")

    _arbiter_lookup_sha256("opentui" "${tarball_name}@${version}" _opentui_sha)
    arbiter_fetch_pinned(
        "${_url}"
        "${_archive}"
        "${_opentui_sha}"
        "OpenTUI prebuilt ${npm_pkg}@${version}")

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

    _arbiter_lookup_sha256("zig" "${target_triplet}@${version}" _zig_sha)
    arbiter_fetch_pinned(
        "${_url}"
        "${_archive}"
        "${_zig_sha}"
        "Zig ${version} (${target_triplet})")

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
