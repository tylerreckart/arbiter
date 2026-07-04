#pragma once

#include <string_view>

namespace arbiter {

// True when argv is bare `arbiter`, `--no-exec`, or help-style REPL launchers.
inline bool argv_launches_interactive(int argc, char* argv[]) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--no-exec") continue;
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

} // namespace arbiter
