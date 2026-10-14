# SHA-256 pinned downloads.
#
# Same shape as cmake/FetchOnnxEmbed.cmake's arbiter_fetch: write a
# partial, hash it, and only then publish the destination. OpenTUI is
# required, so a mismatch is fatal instead of a stub fallback.

function(arbiter_require_sha256 file expected label)
    if(expected STREQUAL "")
        message(FATAL_ERROR "${label}: missing SHA256 pin")
    endif()
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${label}: missing file ${file}")
    endif()
    file(SHA256 "${file}" _got)
    string(TOLOWER "${expected}" _exp)
    string(TOLOWER "${_got}" _got_l)
    if(NOT _got_l STREQUAL "${_exp}")
        file(REMOVE "${file}")
        message(FATAL_ERROR
            "${label}: SHA256 mismatch\n  expected ${_exp}\n  actual   ${_got_l}")
    endif()
endfunction()

# Download `url` to `dest`, replacing a cached file whose digest does not
# match. Deletes the partial (and a bad cache) on mismatch.
function(arbiter_fetch_pinned url dest expected label)
    if(expected STREQUAL "")
        message(FATAL_ERROR "${label}: missing SHA256 pin")
    endif()
    string(TOLOWER "${expected}" _exp)
    if(EXISTS "${dest}")
        file(SHA256 "${dest}" _have)
        string(TOLOWER "${_have}" _have_l)
        if(_have_l STREQUAL "${_exp}")
            return()
        endif()
        message(STATUS "${label}: cached file failed SHA256; refetching")
        file(REMOVE "${dest}")
    endif()

    message(STATUS "Fetching ${label}")
    set(_partial "${dest}.partial")
    file(DOWNLOAD "${url}" "${_partial}"
        STATUS _st
        TIMEOUT 180
        TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _msg)
        file(REMOVE "${_partial}")
        message(FATAL_ERROR "Failed to download ${label}: ${_msg}")
    endif()
    arbiter_require_sha256("${_partial}" "${_exp}" "${label}")
    file(RENAME "${_partial}" "${dest}")
endfunction()
