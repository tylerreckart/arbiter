# Best-effort Homebrew path hints for keg-only formulae on macOS.
# Safe no-op on Linux/Windows and when Homebrew is absent.

if(NOT APPLE)
    return()
endif()

find_program(_ARBITER_BREW brew)
if(NOT _ARBITER_BREW)
    return()
endif()

function(_arbiter_brew_prefix formula out_var)
    execute_process(
        COMMAND "${_ARBITER_BREW}" --prefix "${formula}"
        OUTPUT_VARIABLE _prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc
    )
    if(_rc EQUAL 0 AND EXISTS "${_prefix}")
        set("${out_var}" "${_prefix}" PARENT_SCOPE)
    else()
        set("${out_var}" "" PARENT_SCOPE)
    endif()
endfunction()

# OpenSSL is keg-only; CMake's FindOpenSSL misses it without OPENSSL_ROOT_DIR.
if(NOT OPENSSL_ROOT_DIR)
    foreach(_openssl_formula openssl@3 openssl)
        _arbiter_brew_prefix("${_openssl_formula}" _openssl_prefix)
        if(_openssl_prefix AND EXISTS "${_openssl_prefix}/include/openssl/ssl.h")
            set(OPENSSL_ROOT_DIR "${_openssl_prefix}" CACHE PATH "OpenSSL root (Homebrew)" FORCE)
            message(STATUS "Homebrew hint: OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}")
            break()
        endif()
    endforeach()
endif()

# libcurl from Homebrew when the system framework isn't visible to FindCURL.
if(NOT CURL_ROOT AND NOT DEFINED ENV{CURL_ROOT})
    _arbiter_brew_prefix("curl" _curl_prefix)
    if(_curl_prefix AND EXISTS "${_curl_prefix}/include/curl/curl.h")
        set(CURL_ROOT "${_curl_prefix}" CACHE PATH "libcurl root (Homebrew)" FORCE)
        message(STATUS "Homebrew hint: CURL_ROOT=${CURL_ROOT}")
    endif()
endif()

# libedit for line editing (optional elsewhere in CMakeLists).
if(NOT EDITLINE_INCLUDE OR NOT EDITLINE_LIB)
    _arbiter_brew_prefix("libedit" _edit_prefix)
    if(_edit_prefix AND EXISTS "${_edit_prefix}/include/editline/readline.h")
        if(NOT EDITLINE_INCLUDE)
            set(EDITLINE_INCLUDE "${_edit_prefix}/include" CACHE PATH "libedit include (Homebrew)")
        endif()
        if(NOT EDITLINE_LIB)
            find_library(EDITLINE_LIB edit PATHS "${_edit_prefix}/lib" NO_DEFAULT_PATH)
            if(EDITLINE_LIB)
                set(EDITLINE_LIB "${EDITLINE_LIB}" CACHE FILEPATH "libedit library (Homebrew)")
            endif()
        endif()
    endif()
endif()
