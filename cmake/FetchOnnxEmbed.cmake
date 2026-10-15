# Optional ONNX Runtime + bge-small-en-v1.5 weights.
# A failed download does not fail configure. The embedding router then
# compiles as a stub and live routing keeps the heuristic.
# Pins live in include/embed_pins.h.

set(ARBITER_HAS_ONNXRUNTIME OFF)
set(ARBITER_ONNXRUNTIME_INCLUDE "")
set(ARBITER_ONNXRUNTIME_LIBDIR "")
set(ARBITER_ONNXRUNTIME_LIB "")
set(ARBITER_EMBED_MODEL_PATH "")
set(ARBITER_EMBED_VOCAB_PATH "")

option(ARBITER_EMBED_ROUTER "Try to fetch and link the local embedding router" ON)
if(ARBITER_ASAN OR ARBITER_TSAN)
    set(ARBITER_EMBED_ROUTER OFF)
endif()

function(arbiter_read_pin key outvar)
    string(REGEX MATCH "${key}[ \t\r\n]*=[ \t\r\n]*\"([^\"]+)\"" _match "${ARBITER_EMBED_PINS_TEXT}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "embed_pins.h is missing ${key}")
    endif()
    set(${outvar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(arbiter_link_onnxruntime target)
    if(NOT ARBITER_HAS_ONNXRUNTIME)
        return()
    endif()
    target_include_directories(${target} PRIVATE "${ARBITER_ONNXRUNTIME_INCLUDE}")
    target_link_libraries(${target} PRIVATE "${ARBITER_ONNXRUNTIME_LIB}")
    # Do not append the fetch directory to RPATH. The binary's runtime
    # path is only $ORIGIN / @loader_path, and the library is staged
    # beside the executable (see arbiter_stage_onnxruntime).
    target_compile_definitions(${target} PRIVATE
        ARBITER_EMBED_MODEL_PATH="${ARBITER_EMBED_MODEL_PATH}"
        ARBITER_EMBED_VOCAB_PATH="${ARBITER_EMBED_VOCAB_PATH}")
endfunction()

# Place libonnxruntime next to `target` so a same-directory runtime path
# resolves it in the build tree, the release tarball, and install.sh.
function(arbiter_stage_onnxruntime target)
    if(NOT ARBITER_HAS_ONNXRUNTIME)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${ARBITER_ONNXRUNTIME_LIBDIR}
            -DDEST=$<TARGET_FILE_DIR:${target}>
            -P ${CMAKE_SOURCE_DIR}/cmake/stage_onnxruntime.cmake
        COMMENT "Staging ONNX Runtime next to ${target}"
        VERBATIM)
endfunction()

file(READ "${CMAKE_SOURCE_DIR}/include/embed_pins.h" ARBITER_EMBED_PINS_TEXT)
arbiter_read_pin("kEmbedModelUrl" ARBITER_PIN_MODEL_URL)
arbiter_read_pin("kEmbedModelSha256" ARBITER_PIN_MODEL_SHA)
arbiter_read_pin("kEmbedVocabUrl" ARBITER_PIN_VOCAB_URL)
arbiter_read_pin("kEmbedVocabSha256" ARBITER_PIN_VOCAB_SHA)
arbiter_read_pin("kOnnxRuntimeVersion" ARBITER_PIN_ORT_VERSION)
arbiter_read_pin("kOnnxRuntimeLinuxX64Url" ARBITER_PIN_ORT_LINUX_URL)
arbiter_read_pin("kOnnxRuntimeLinuxX64Sha256" ARBITER_PIN_ORT_LINUX_SHA)
arbiter_read_pin("kOnnxRuntimeOsxArm64Url" ARBITER_PIN_ORT_OSX_URL)
arbiter_read_pin("kOnnxRuntimeOsxArm64Sha256" ARBITER_PIN_ORT_OSX_SHA)

function(arbiter_fetch url dest sha label)
    set(_ok OFF PARENT_SCOPE)
    if(EXISTS "${dest}")
        file(SHA256 "${dest}" _have)
        if(_have STREQUAL "${sha}")
            set(_ok ON PARENT_SCOPE)
            return()
        endif()
        message(STATUS "${label} checksum changed; refetching")
        file(REMOVE "${dest}")
    endif()
    message(STATUS "Fetching ${label}")
    file(DOWNLOAD "${url}" "${dest}.partial"
        STATUS _st
        TIMEOUT 180
        TLS_VERIFY ON)
    list(GET _st 0 _code)
    list(GET _st 1 _msg)
    if(NOT _code EQUAL 0)
        file(REMOVE "${dest}.partial")
        message(STATUS "${label} fetch failed (${_code} ${_msg}); embedding router will fall back")
        return()
    endif()
    file(SHA256 "${dest}.partial" _got)
    if(NOT _got STREQUAL "${sha}")
        file(REMOVE "${dest}.partial")
        message(STATUS "${label} checksum mismatch; embedding router will fall back")
        return()
    endif()
    file(RENAME "${dest}.partial" "${dest}")
    set(_ok ON PARENT_SCOPE)
endfunction()

if(NOT ARBITER_EMBED_ROUTER)
    message(STATUS "Embedding router: stub (disabled or sanitizer build)")
    return()
endif()

set(_ort_url "")
set(_ort_sha "")
set(_ort_name "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_ort_url "${ARBITER_PIN_ORT_OSX_URL}")
    set(_ort_sha "${ARBITER_PIN_ORT_OSX_SHA}")
    set(_ort_name "onnxruntime-osx-arm64-${ARBITER_PIN_ORT_VERSION}")
elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(_ort_url "${ARBITER_PIN_ORT_LINUX_URL}")
    set(_ort_sha "${ARBITER_PIN_ORT_LINUX_SHA}")
    set(_ort_name "onnxruntime-linux-x64-${ARBITER_PIN_ORT_VERSION}")
endif()

if(_ort_url STREQUAL "")
    message(STATUS "Embedding router: stub (no pinned ONNX Runtime for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR})")
    return()
endif()

set(_dest "${CMAKE_BINARY_DIR}/embed-fetch")
file(MAKE_DIRECTORY "${_dest}")

set(_ok OFF)
arbiter_fetch("${_ort_url}" "${_dest}/${_ort_name}.tgz" "${_ort_sha}" "ONNX Runtime ${_ort_name}")
if(NOT _ok)
    return()
endif()

set(_ort_root "${_dest}/${_ort_name}")
if(NOT EXISTS "${_ort_root}/lib/libonnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}")
    message(STATUS "Extracting ONNX Runtime")
    file(ARCHIVE_EXTRACT INPUT "${_dest}/${_ort_name}.tgz" DESTINATION "${_dest}")
endif()
set(_ort_lib "${_ort_root}/lib/libonnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(_ort_inc "${_ort_root}/include")
if(NOT EXISTS "${_ort_lib}" OR NOT EXISTS "${_ort_inc}/onnxruntime_c_api.h")
    message(STATUS "Embedding router: stub (ONNX Runtime layout missing after extract)")
    return()
endif()

set(_ok OFF)
arbiter_fetch("${ARBITER_PIN_MODEL_URL}" "${_dest}/model_quantized.onnx" "${ARBITER_PIN_MODEL_SHA}" "bge-small-en-v1.5 quantized")
if(NOT _ok)
    message(STATUS "Embedding router: ONNX Runtime is present, weights are not; first run may fetch them")
else()
    set(ARBITER_EMBED_MODEL_PATH "${_dest}/model_quantized.onnx")
endif()

set(_ok OFF)
arbiter_fetch("${ARBITER_PIN_VOCAB_URL}" "${_dest}/vocab.txt" "${ARBITER_PIN_VOCAB_SHA}" "bge-small-en-v1.5 vocab")
if(_ok)
    set(ARBITER_EMBED_VOCAB_PATH "${_dest}/vocab.txt")
else()
    set(ARBITER_EMBED_MODEL_PATH "")
endif()

set(ARBITER_HAS_ONNXRUNTIME ON)
set(ARBITER_ONNXRUNTIME_INCLUDE "${_ort_inc}")
set(ARBITER_ONNXRUNTIME_LIBDIR "${_ort_root}/lib")
set(ARBITER_ONNXRUNTIME_LIB "${_ort_lib}")
message(STATUS "Embedding router: ONNX Runtime ${ARBITER_PIN_ORT_VERSION} (${_ort_name})")
