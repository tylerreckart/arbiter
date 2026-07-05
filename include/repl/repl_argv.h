#pragma once

#include <string>
#include <string_view>

namespace arbiter {

inline bool argv_is_repl_flag(std::string_view arg) {
    return arg == "--no-exec" || arg == "--theme";
}

// True when argv is bare `arbiter`, `--no-exec`, `--theme PRESET`, or combos.
inline bool argv_launches_interactive(int argc, char* argv[]) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; ) {
        const std::string_view arg = argv[i];
        if (arg == "--no-exec") {
            ++i;
            continue;
        }
        if (arg == "--theme") {
            if (i + 1 >= argc) return false;
            i += 2;
            continue;
        }
        return false;
    }
    return true;
}

inline bool argv_has_no_exec(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--no-exec") return true;
    }
    return false;
}

inline bool argv_has_theme_flag(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--theme") return true;
    }
    return false;
}

// Empty when --theme is absent; caller should reject empty when --theme present.
inline std::string argv_repl_theme(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--theme") {
            if (i + 1 < argc) return argv[i + 1];
            return {};
        }
    }
    return {};
}

} // namespace arbiter
