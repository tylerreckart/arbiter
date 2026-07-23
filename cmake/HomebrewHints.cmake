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

# Intentionally do NOT prefer Homebrew's keg-only curl here.
# macOS ships a system libcurl that FindCURL resolves without help, and
# release binaries must not embed `/opt/homebrew/opt/curl/...` load paths
# (those only exist on machines with that Homebrew formula installed).
# Developers who specifically want brew curl can still pass
# -DCURL_ROOT="$(brew --prefix curl)" at configure time.
