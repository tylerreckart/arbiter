// index/src/cli_helpers.cpp — see cli_helpers.h

#include "cli_helpers.h"
#include "commands.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/ioctl.h>
#include <unistd.h>
#include <pwd.h>

namespace fs = std::filesystem;

namespace index_ai {

const char* BANNER =
    "\n"
    "                   iiii              iiii                   \n"
    "                 iiiiiiii          iiiiiiii                 \n"
    "               iiiiiiiiiiii      iiiiiiiiiiii               \n"
    "             iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii             \n"
    "           iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii           \n"
    "         iiiiiiiiiiiiiiiiiii    iiiiiiiiiiiiiiiiiii         \n"
    "       iiiiiiiiiiiiiiiiiii        iiiiiiiiiiiiiiiiiii       \n"
    "     iiiiiiiiiiiiiiiiiii            iiiiiiiiiiiiiiiiiii     \n"
    "   iiiiiiiiiiiiiiiiiii                 iiiiiiiiiiiiiiiiii   \n"
    " iiiiiiiiiiiiiiiiii                      iiiiiiiiiiiiiiiiii \n"
    "iiiiiiiiiiiiiiiii                          iiiiiiiiiiiiiiiii\n"
    " iiiiiiiiiiiiii        iiiiiiiiiiiiii        iiiiiiiiiiiiii \n"
    "   iiiiiiiiii         iiiiiiiiiiiiiiii         iiiiiiiiii   \n"
    "     iiiiiii          iiiiiiiiiiiiiiii          iiiiiii     \n"
    "      iiiii           iiiiiiiiiiiiiiii           iiiii      \n"
    "     iiiiii           iiiiiiiiiiiiiiii           iiiiii     \n"
    "    iiiiiiiii         iiiiiiiiiiiiiiii          iiiiiiii    \n"
    "  iiiiiiiiiiii         iiiiiiiiiiiiii         iiiiiiiiiiii  \n"
    "iiiiiiiiiiiiiiiii                           iiiiiiiiiiiiiiii\n"
    "iiiiiiiiiiiiiiiiiii                      iiiiiiiiiiiiiiiiiii\n"
    "  iiiiiiiiiiiiiiiiiii                  iiiiiiiiiiiiiiiiiii  \n"
    "    iiiiiiiiiiiiiiiiiii              iiiiiiiiiiiiiiiiiii    \n"
    "      iiiiiiiiiiiiiiiiiii          iiiiiiiiiiiiiiiiiii      \n"
    "        iiiiiiiiiiiiiiiiiii      iiiiiiiiiiiiiiiiiii        \n"
    "          iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii          \n"
    "            iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii            \n"
    "              iiiiiiiiiiiiii    iiiiiiiiiiiiii              \n"
    "                iiiiiiiiii        iiiiiiiiii                \n"
    "                  iiiiii             iiiii                  \n"
    "\n";

std::string agent_color(const std::string& agent_id) {
    if (agent_id == "index") return "\033[38;5;208m";  // orange

    static const int palette[] = {
        214,  // amber
        172,  // medium orange
        166,  // burnt orange
        220,  // gold
        209,  // salmon
        178,  // dark gold
        216,  // light peach
        130,  // dark amber
        173,  // terracotta
        215,  // soft peach
        202,  // red-orange
        180,  // warm tan
    };
    static const int palette_size = sizeof(palette) / sizeof(palette[0]);

    size_t h = std::hash<std::string>{}(agent_id);
    int code = palette[h % palette_size];

    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[38;5;%dm", code);
    return buf;
}

std::string get_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || home[0] == '\0')
        throw std::runtime_error("Cannot determine home directory: $HOME unset and getpwuid failed");
    std::string dir = std::string(home) + "/.index";
    fs::create_directories(dir);
    return dir;
}

std::string get_memory_dir() {
    // Memory is cwd-scoped: agents never see notes from other projects.
    // The directory is created lazily by the writers (cmd_mem_write etc.) —
    // don't auto-create on every resolve or we'd scatter empty .index/memory
    // folders into every cwd that happens to launch the binary.
    return (fs::current_path() / ".index" / "memory").string();
}

std::string write_memory(const std::string& agent_id, const std::string& text) {
    return index_ai::cmd_mem_write(agent_id, text, get_memory_dir());
}

std::string read_memory(const std::string& agent_id) {
    return index_ai::cmd_mem_read(agent_id, get_memory_dir());
}

std::string fetch_url(const std::string& url) {
    return index_ai::cmd_fetch(url);
}

std::string get_api_key() {
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key && key[0]) return key;

    std::string path = get_config_dir() + "/api_key";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string k;
        std::getline(f, k);
        if (!k.empty()) return k;
    }

    std::cerr << "ERR: Set ANTHROPIC_API_KEY or write key to ~/.index/api_key\n";
    std::exit(1);
}

int term_cols() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

int term_rows() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

} // namespace index_ai
