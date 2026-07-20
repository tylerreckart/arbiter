// arbiter/src/cli_helpers.cpp — see cli_helpers.h

#include "cli_helpers.h"
#include "commands.h"
#include "mcp/manager.h"
#include "starters.h"
#include "theme.h"
#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace arbiter {

std::string agent_color(const std::string& agent_id) {
    const Theme& t = theme();
    if (agent_id == "index") return t.agent_master;
    size_t h = std::hash<std::string>{}(agent_id);
    return t.agent_palette[h % t.agent_palette.size()];
}

std::string get_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || home[0] == '\0')
        throw std::runtime_error("Cannot determine home directory: $HOME unset and getpwuid failed");
    std::string dir = std::string(home) + "/.arbiter";
    fs::create_directories(dir);
    return dir;
}

std::string get_memory_dir() {
    // Memory is cwd-scoped: agents never see notes from other projects.
    // The directory is created lazily by the writers (cmd_mem_write etc.) —
    // don't auto-create on every resolve or we'd scatter empty .arbiter/memory
    // folders into every cwd that happens to launch the binary.
    return (fs::current_path() / ".arbiter" / "memory").string();
}

std::string write_memory(const std::string& agent_id, const std::string& text) {
    return arbiter::cmd_mem_write(agent_id, text, get_memory_dir());
}

std::string read_memory(const std::string& agent_id) {
    return arbiter::cmd_mem_read(agent_id, get_memory_dir());
}

std::string fetch_url(const std::string& url) {
    return arbiter::cmd_fetch(url);
}

namespace {

// Load a key file with ownership + mode hardening.  No env lookup.
// Empty return = file missing / empty.  Wrong-uid exits(1) as tamper.
std::string load_key_file(const std::string& file_path) {
    struct stat st{};
    if (::stat(file_path.c_str(), &st) == 0) {
        if (st.st_uid != ::geteuid()) {
            std::cerr << "ERR: " << file_path << " is not owned by the current user; "
                         "refusing to load (potential credential tamper)\n";
            std::exit(1);
        }
        const mode_t perms = st.st_mode & 0777;
        if (perms & (S_IRWXG | S_IRWXO)) {
            std::cerr << "WARN: " << file_path << " was world/group-accessible "
                         "(mode " << std::oct << perms << std::dec
                      << ") — tightening to 0600\n";
            ::chmod(file_path.c_str(), S_IRUSR | S_IWUSR);
        }
    }

    std::ifstream f(file_path);
    if (f.is_open()) {
        std::string k;
        std::getline(f, k);
        if (!k.empty()) return k;
    }
    return {};
}

// Resolve a provider API key.  Order of precedence:
//   1. env var  — short-circuits the file path entirely
//   2. file on disk — see load_key_file
// Empty return = "no key found for this provider" — the caller falls back
// to whatever that means (first-run wizard for get_api_keys, or a clear
// "no API key configured" failure at the ApiClient wire layer).
std::string load_key(const char* env_var, const std::string& file_path) {
    if (env_var && env_var[0]) {
        const char* env = std::getenv(env_var);
        if (env && env[0]) return env;
    }
    return load_key_file(file_path);
}

std::string trim_ws(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && std::isspace(static_cast<unsigned char>(s[lead]))) ++lead;
    return s.substr(lead);
}

bool search_key_from_env() {
    if (const char* k = std::getenv("ARBITER_SEARCH_API_KEY"); k && *k) {
        if (!trim_ws(k).empty()) return true;
    }
    if (const char* k = std::getenv("BRAVE_SEARCH_API_KEY"); k && *k) {
        if (!trim_ws(k).empty()) return true;
    }
    return false;
}

bool search_key_file_present() {
    struct stat st{};
    return ::stat((get_config_dir() + "/search_api_key").c_str(), &st) == 0
        && S_ISREG(st.st_mode);
}

struct ProviderSetup {
    const char* name;       // "openrouter"
    const char* display;    // "OpenRouter (hosted models)"
    const char* key_file;   // "openrouter_api_key"
    const char* env_var;    // "OPENROUTER_API_KEY"
    const char* hint;       // key-format hint shown to the user
    const char* console_url;// where to grab a key
};

constexpr ProviderSetup kProviderSetups[] = {
    { "openrouter",
      "OpenRouter (hosted models)",
      "openrouter_api_key",
      "OPENROUTER_API_KEY",
      "starts with sk-or-",
      "https://openrouter.ai/settings/keys" },
};
constexpr size_t kNumProviders = sizeof(kProviderSetups) / sizeof(kProviderSetups[0]);

// Recommended models shown on the "pick a default" step.  The wizard filters
// these by which providers the user configured.  The first entry of a given
// provider acts as its recommended default if the user presses Enter.
struct ModelOption {
    const char* provider;   // "openrouter"
    const char* id;         // model id passed to the API router
    const char* blurb;      // one-liner shown to the user
};

constexpr ModelOption kModelOptions[] = {
    { "openrouter", "~openai/gpt-latest",          "OpenAI latest — via OpenRouter (recommended)" },
    { "openrouter", "openai/gpt-5.2",              "GPT-5.2 — via OpenRouter" },
    { "openrouter", "google/gemini-3.1-flash-lite","Gemini Flash Lite — via OpenRouter" },
};

constexpr std::uint32_t kAttrBold = 1u << 0;

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        out.emplace_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

class SetupTui {
public:
    enum Key {
        KEY_NONE = 0,
        KEY_ENTER = 1000,
        KEY_ESC,
        KEY_BACKSPACE,
        KEY_UP,
        KEY_DOWN,
        KEY_LEFT,
        KEY_RIGHT,
        KEY_SPACE,
        KEY_CTRL_C,
    };

    SetupTui()
        : engine_(static_cast<std::uint32_t>(std::max(1, term_cols())),
                  static_cast<std::uint32_t>(std::max(1, term_rows()))) {
        if (::tcgetattr(STDIN_FILENO, &saved_stdin_) == 0) {
            have_saved_stdin_ = true;
            termios raw = saved_stdin_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG | IEXTEN));
            raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        engine_.setup_terminal(true);
        drain_stdin_spurious(300);
    }

    ~SetupTui() {
        engine_.shutdown_terminal();
        // Must finish draining before restoring ECHO below — bytes that
        // arrive after ECHO is back on get echoed to the screen by the
        // kernel the instant they're received; reading them afterward
        // doesn't undo that.
        drain_stdin_spurious(300);
        if (have_saved_stdin_) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_stdin_);
        }
    }

    void message(const std::string& title,
                 const std::vector<std::string>& body,
                 const std::string& footer = "enter continue") {
        render(title, body, {}, 0, footer);
        while (true) {
            const int key = read_key();
            if (key == KEY_ENTER || key == KEY_ESC || key == KEY_CTRL_C) return;
        }
    }

    int menu(const std::string& title,
             const std::vector<std::string>& body,
             const std::vector<std::string>& options,
             int selected,
             const std::string& footer = "↑/↓ move  enter select") {
        if (options.empty()) return -1;
        selected = std::clamp(selected, 0, static_cast<int>(options.size()) - 1);
        while (true) {
            render(title, body, options, selected, footer);
            const int key = read_key();
            if (key == KEY_UP) {
                selected = (selected + static_cast<int>(options.size()) - 1)
                         % static_cast<int>(options.size());
            } else if (key == KEY_DOWN) {
                selected = (selected + 1) % static_cast<int>(options.size());
            } else if (key == KEY_ENTER) {
                return selected;
            } else if (key == KEY_CTRL_C || key == KEY_ESC) {
                return -1;
            }
        }
    }

    std::optional<std::set<size_t>> multi_select(
        const std::string& title,
        const std::vector<std::string>& body,
        const std::vector<std::string>& options) {
        std::set<size_t> picked;
        int selected = 0;
        while (true) {
            std::vector<std::string> rows;
            rows.reserve(options.size());
            for (size_t i = 0; i < options.size(); ++i) {
                rows.push_back(std::string(picked.count(i) ? "[x] " : "[ ] ") + options[i]);
            }
            render(title, body, rows, selected,
                   "↑/↓ move  space toggle  enter continue  a all  esc skip");
            const int key = read_key();
            if (key == KEY_UP && !options.empty()) {
                selected = (selected + static_cast<int>(options.size()) - 1)
                         % static_cast<int>(options.size());
            } else if (key == KEY_DOWN && !options.empty()) {
                selected = (selected + 1) % static_cast<int>(options.size());
            } else if (key == KEY_SPACE && !options.empty()) {
                if (picked.count(static_cast<size_t>(selected)))
                    picked.erase(static_cast<size_t>(selected));
                else
                    picked.insert(static_cast<size_t>(selected));
            } else if (key == 'a' || key == 'A') {
                if (picked.size() == options.size()) picked.clear();
                else {
                    picked.clear();
                    for (size_t i = 0; i < options.size(); ++i) picked.insert(i);
                }
            } else if (key == KEY_ENTER) {
                return picked;
            } else if (key == KEY_ESC) {
                return std::set<size_t>{};
            } else if (key == KEY_CTRL_C) {
                return std::nullopt;
            }
        }
    }

    std::optional<std::string> input(const std::string& title,
                                     const std::vector<std::string>& body,
                                     const std::string& placeholder,
                                     bool secret,
                                     const std::string& initial = {}) {
        std::string text = initial;
        while (true) {
            std::vector<std::string> lines = body;
            std::string shown = secret
                ? std::string(text.size(), '*')
                : text;
            if (shown.empty()) shown = placeholder;
            lines.push_back("");
            lines.push_back(shown);
            render(title, lines, {}, 0, "type text  enter accept  esc cancel");

            const int key = read_key();
            if (key == KEY_ENTER) return text;
            if (key == KEY_ESC) return std::nullopt;
            if (key == KEY_CTRL_C) return std::nullopt;
            if (key == KEY_BACKSPACE) {
                if (!text.empty()) text.pop_back();
                continue;
            }
            if (key >= 32 && key < 127) {
                text.push_back(static_cast<char>(key));
            }
        }
    }

private:
    opentui::Engine engine_;
    termios saved_stdin_{};
    bool    have_saved_stdin_ = false;

    static constexpr int kEscByteTimeoutMs = 100;

    static bool parse_uint(std::string_view s, int& out) {
        if (s.empty()) return false;
        int v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    }

    static std::optional<int> map_kitty_keycode(int code) {
        switch (code) {
            case 27:
            case 57344: return KEY_ESC;
            case 13:
            case 57345: return KEY_ENTER;
            case 127:
            case 57347: return KEY_BACKSPACE;
            case 32:    return KEY_SPACE;
            case 57352: return KEY_UP;
            case 57353: return KEY_DOWN;
            case 57350: return KEY_LEFT;
            case 57351: return KEY_RIGHT;
            default:
                if (code >= 32 && code < 127) return code;
                return std::nullopt;
        }
    }

    static std::optional<int> decode_csi(char final, std::string_view params) {
        if (final == 'A') return KEY_UP;
        if (final == 'B') return KEY_DOWN;
        if (final == 'C') return KEY_RIGHT;
        if (final == 'D') return KEY_LEFT;

        if (final == 'u') {
            if (params.empty() || params[0] == '?' || params[0] == '<') return std::nullopt;
            const auto semi = params.find(';');
            const std::string_view code = params.substr(0, semi);
            int kc = 0;
            if (!parse_uint(code, kc)) return std::nullopt;
            return map_kitty_keycode(kc);
        }

        if (final == '~') {
            if (params == "3") return KEY_BACKSPACE;
        }

        return std::nullopt;
    }

    void resize_if_needed() {
        const int w = std::max(1, term_cols());
        const int h = std::max(1, term_rows());
        if (static_cast<std::uint32_t>(w) != engine_.width() ||
            static_cast<std::uint32_t>(h) != engine_.height()) {
            engine_.resize(static_cast<std::uint32_t>(w),
                           static_cast<std::uint32_t>(h));
        }
    }

    void draw_text(OpenTuiHandle frame,
                   std::uint32_t x,
                   std::uint32_t y,
                   std::string_view text,
                   const TuiRgba& fg,
                   const TuiRgba& bg,
                   std::uint32_t attrs = 0) {
        if (text.empty()) return;
        bufferDrawText(frame,
                       text.data(),
                       static_cast<std::uint32_t>(text.size()),
                       x,
                       y,
                       fg.data(),
                       bg.data(),
                       attrs);
    }

    void fill(OpenTuiHandle frame,
              std::uint32_t x,
              std::uint32_t y,
              std::uint32_t w,
              std::uint32_t h,
              const TuiRgba& bg) {
        if (w == 0 || h == 0) return;
        bufferFillRect(frame, x, y, w, h, bg.data());
    }

    void render(const std::string& title,
                const std::vector<std::string>& body,
                const std::vector<std::string>& options,
                int selected,
                const std::string& footer) {
        resize_if_needed();
        const auto& d = tui_design();
        engine_.draw([&](OpenTuiHandle frame, std::uint32_t width, std::uint32_t height) {
            bufferClear(frame, d.bg.scroll.data());
            if (width == 0 || height == 0) return;

            const std::uint32_t margin_x = std::min<std::uint32_t>(4, width / 12);
            const std::uint32_t x = margin_x;
            const std::uint32_t y = height > 28 ? 2 : 1;
            const std::uint32_t panel_w = width > margin_x * 2
                ? width - margin_x * 2
                : width;
            const std::uint32_t panel_h = height > y * 2
                ? height - y * 2
                : height;

            fill(frame, x, y, panel_w, panel_h, d.bg.panel);
            fill(frame, x, y, 1, panel_h, d.accent.primary);
            fill(frame, x + 1, y, panel_w > 1 ? panel_w - 1 : 0, 3, d.bg.header);

            std::uint32_t cy = y + 1;
            draw_text(frame, x + 3, cy, title, d.text.primary, d.bg.header, kAttrBold);
            cy = y + 4;

            for (const auto& line : body) {
                if (cy + 2 >= y + panel_h) break;
                draw_text(frame, x + 3, cy++, line, d.text.subtle, d.bg.panel);
            }
            if (!options.empty()) ++cy;

            for (size_t i = 0; i < options.size(); ++i) {
                if (cy + 2 >= y + panel_h) break;
                const bool active = static_cast<int>(i) == selected;
                const TuiRgba& bg = active ? d.accent.primary : d.bg.input;
                const TuiRgba& fg = active ? d.text.inverse : d.text.primary;
                fill(frame, x + 2, cy, panel_w > 4 ? panel_w - 4 : 0, 1, bg);
                draw_text(frame,
                          x + 4,
                          cy,
                          active ? "› " : "  ",
                          fg,
                          bg,
                          active ? kAttrBold : 0);
                draw_text(frame,
                          x + 6,
                          cy,
                          options[i],
                          fg,
                          bg,
                          active ? kAttrBold : 0);
                ++cy;
            }

            if (!footer.empty() && panel_h >= 2) {
                fill(frame, x + 1, y + panel_h - 2,
                     panel_w > 1 ? panel_w - 1 : 0, 1, d.bg.footer);
                draw_text(frame,
                          x + 3,
                          y + panel_h - 2,
                          footer,
                          d.text.muted,
                          d.bg.footer);
            }
        });
        engine_.render(true);
    }

    bool read_byte_timed(int& out, int ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        const int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) return false;
        unsigned char c = 0;
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0) return false;
        out = c;
        return true;
    }

    void discard_osc() {
        while (true) {
            int b = 0;
            if (!read_byte_timed(b, 100)) break;
            if (b == 0x07 || b == 0x9C) break;
            if (b == 0x1B) {
                int b2 = 0;
                if (read_byte_timed(b2, 10) && b2 == '\\') break;
            }
        }
    }

    void discard_string_terminated() {
        while (true) {
            int b = 0;
            if (!read_byte_timed(b, 100)) break;
            if (b == 0x9C) break;
            if (b == 0x1B) {
                int b2 = 0;
                if (read_byte_timed(b2, 10) && b2 == '\\') break;
            }
        }
    }

    bool is_terminal_response_csi(const std::string& params, char final) const {
        if (final == 'R') return true;
        if (final == 'c' && !params.empty()
            && (params[0] == '?' || params[0] == '>' || params[0] == '=')) {
            return true;
        }
        if (final == 'y' && params.find('$') != std::string::npos) return true;
        if (final == 'u' && !params.empty() && params[0] == '?') return true;
        return false;
    }

    int read_key() {
        while (true) {
            int b = 0;
            if (!read_byte_timed(b, 1000)) return KEY_NONE;
            if (b == 3) return KEY_CTRL_C;
            if (b == '\r' || b == '\n') return KEY_ENTER;
            if (b == 127 || b == 8) return KEY_BACKSPACE;
            if (b == ' ') return KEY_SPACE;
            if (b == 0x1B) {
                int b2 = 0;
                if (!read_byte_timed(b2, kEscByteTimeoutMs)) return KEY_ESC;

                if (b2 == '[') {
                    std::string params;
                    char final = 0;
                    while (true) {
                        int b3 = 0;
                        if (!read_byte_timed(b3, kEscByteTimeoutMs)) break;
                        if ((b3 >= '0' && b3 <= '9') || b3 == ';' || b3 == '?'
                            || b3 == '$' || b3 == '>' || b3 == '=') {
                            params.push_back(static_cast<char>(b3));
                            continue;
                        }
                        if (b3 >= 0x40 && b3 <= 0x7E) {
                            final = static_cast<char>(b3);
                            break;
                        }
                        break;
                    }
                    if (final) {
                        if (is_terminal_response_csi(params, final)) continue;
                        if (auto key = decode_csi(final, params)) return *key;
                    }
                    continue;
                }

                if (b2 == 'O') {
                    int b3 = 0;
                    if (read_byte_timed(b3, kEscByteTimeoutMs)) {
                        if (auto key = decode_csi(static_cast<char>(b3), "")) return *key;
                    }
                    continue;
                }

                if (b2 == ']') {
                    discard_osc();
                    continue;
                }
                if (b2 == 'P' || b2 == '^' || b2 == '_') {
                    discard_string_terminated();
                    continue;
                }
                return KEY_ESC;
            }
            if (b >= 32 && b < 127) return b;
        }
    }
};

// Read one line from stdin with echo disabled — for API keys so they don't
// get mirrored into the scrollback.  Falls back to visible input if stdin
// isn't a TTY (shouldn't happen here since get_api_keys only enters the
// wizard when isatty is true, but defend against it anyway).
std::string read_secret_line() {
    termios orig{};
    const bool tty = (tcgetattr(STDIN_FILENO, &orig) == 0);
    if (tty) {
        termios muted = orig;
        muted.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &muted);
    }
    std::string line;
    std::getline(std::cin, line);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        std::cout << "\n";  // the Enter keystroke was swallowed with the echo
    }
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.back()))) {
        line.pop_back();
    }
    return line;
}

// Write the key to ~/.arbiter/<filename> with mode 0600.  Opens with the
// restrictive mode upfront so the file is never world-readable in between.
bool write_key_file(const std::string& filename, const std::string& key) {
    const std::string path = get_config_dir() + "/" + filename;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    // open(2) mode is masked by umask — fchmod so the key is never
    // briefly group/world-readable.
    if (::fchmod(fd, 0600) != 0) { ::close(fd); return false; }
    size_t off = 0;
    while (off < key.size()) {
        const ssize_t n = ::write(fd, key.data() + off, key.size() - off);
        if (n <= 0) { ::close(fd); return false; }
        off += static_cast<size_t>(n);
    }
    ::write(fd, "\n", 1);
    ::close(fd);
    // Re-apply mode in case the file pre-existed with wider perms.
    ::chmod(path.c_str(), 0600);
    return true;
}

// Read a trimmed line.  Exits(1) on EOF/Ctrl-D so the wizard can't spin.
std::string read_line_or_exit() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cerr << "\nSetup aborted.\n";
        std::exit(1);
    }
    while (!line.empty() &&
           std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    // also strip leading whitespace
    size_t lead = 0;
    while (lead < line.size() &&
           std::isspace(static_cast<unsigned char>(line[lead]))) ++lead;
    return line.substr(lead);
}

// Write the chosen master-model id to ~/.arbiter/master_model.  Orchestrator
// reads this file (see load_master_model_override() in orchestrator.cpp) and
// applies it to the index master agent at startup.
bool write_master_model(const std::string& model_id) {
    const std::string path = get_config_dir() + "/master_model";
    std::ofstream f(path);
    if (!f) return false;
    f << model_id << "\n";
    return f.good();
}

// Step 1: collect API keys.  Loops until the user has configured at least
// one provider and chosen "done".
void wizard_step_keys(SetupTui& ui, std::map<std::string, std::string>& keys) {
    const auto& ps = kProviderSetups[0];
    while (true) {
        const bool configured = keys.count(ps.name) > 0;
        std::vector<std::string> body = {
            "[1/3] Provider API keys",
            "Hosted models route through OpenRouter.",
            "Keys are saved with mode 0600; environment variables override files.",
            "",
            std::string("Status: ") + (configured ? "OpenRouter configured" : "OpenRouter key required"),
        };
        const int pick = ui.menu("Welcome to index",
                                 body,
                                 configured
                                     ? std::vector<std::string>{"Continue", "Replace OpenRouter key", "Quit setup"}
                                     : std::vector<std::string>{"Paste OpenRouter key", "Quit setup"},
                                 0);
        if (pick < 0) std::exit(1);
        if (configured && pick == 0) {
            return;
        }
        if ((!configured && pick == 1) || (configured && pick == 2)) {
            std::exit(1);
        }

        auto key = ui.input("OpenRouter key",
                            {
                                ps.display,
                                std::string("Get a key: ") + ps.console_url,
                                std::string("Saves to ~/.arbiter/") + ps.key_file,
                                std::string(ps.env_var) + " overrides the file.",
                            },
                            ps.hint,
                            true);
        if (!key || key->empty()) {
            continue;
        }
        if (!write_key_file(ps.key_file, *key)) {
            std::cerr << "ERR: could not write "
                      << get_config_dir() << "/" << ps.key_file << "\n\n";
            continue;
        }
        keys[ps.name] = *key;
        ui.message("OpenRouter key saved",
                   {"The key was written to ~/.arbiter/openrouter_api_key.",
                    "You can replace it later with OPENROUTER_API_KEY or by editing the file."});
    }
}

// Step 2: pick the default model for the master "index" agent.  Shown even
// when only one model is available so the user knows what they're getting.
// Enter accepts the first (recommended) option.  The choice is persisted to
// ~/.arbiter/master_model; the orchestrator applies it at startup.
// Returns the chosen model id so Step 3 can use it as a fallback when a
// starter's default model refers to a provider the user didn't configure.
std::string wizard_step_default_model(
    SetupTui& ui,
    const std::map<std::string, std::string>& keys) {
    std::vector<const ModelOption*> available;
    for (const auto& m : kModelOptions) {
        if (keys.count(m.provider)) available.push_back(&m);
    }
    if (available.empty()) return {};  // shouldn't happen — step 1 guarantees a key

    std::vector<std::string> options;
    for (size_t i = 0; i < available.size(); ++i) {
        options.push_back(available[i]->blurb);
    }
    const int pick = ui.menu("Choose default model",
                             {
                                 "[2/3] Default model",
                                 "The index master agent uses this model for routing and orchestration.",
                                 "You can change it later with /model index <model-id>.",
                             },
                             options,
                             0);
    if (pick < 0) std::exit(1);

    const auto* chosen = available[static_cast<size_t>(pick)];
    if (!write_master_model(chosen->id)) {
        // Non-fatal: orchestrator still falls back to a sensible default
        // based on which keys are configured.
        std::cerr << "WARN: could not write ~/.arbiter/master_model — "
                     "will use default each session.\n";
    }
    ui.message("Default model set",
               {std::string("index will use ") + chosen->id + "."});
    return chosen->id;
}

// Check that a model string can route to a configured provider.  Used when
// computing per-starter defaults in Step 3 — if a starter's default model
// refers to a provider the user didn't set up, we swap in the master model
// so the starter works on first message instead of erroring.
bool model_usable(const std::string& model,
                  const std::map<std::string, std::string>& keys) {
    if (model.empty()) return true;
    if (model.rfind("ollama/", 0) == 0) return true;   // ollama is keyless
    return keys.count("openrouter") > 0;
}

// Step 3: multi-select starter agents; let the user customize each one's
// model and advisor before saving to ~/.arbiter/agents/<id>.json.
void wizard_step_starter_agents(
    SetupTui& ui,
    const std::map<std::string, std::string>& keys,
    const std::string& master_model) {
    auto starters = starter_agents();
    if (starters.empty()) return;

    size_t max_id_len = 0;
    for (auto& s : starters) max_id_len = std::max(max_id_len, s.id.size());
    std::vector<std::string> options;
    for (size_t i = 0; i < starters.size(); ++i) {
        options.push_back(starters[i].id
            + std::string(max_id_len - starters[i].id.size() + 2, ' ')
            + starters[i].blurb);
    }
    auto picked_result = ui.multi_select("Install starter agents",
                                         {
                                             "[3/3] Starter agents",
                                             "Pick example agents to install.",
                                             "Each selected agent can be customized before it is saved.",
                                         },
                                         options);
    if (!picked_result) std::exit(1);
    const std::set<size_t> picked = *picked_result;
    if (picked.empty()) {
        ui.message("Starter agents skipped",
                   {"Run arbiter --init later to create them,",
                    "or make your own in ~/.arbiter/agents/."});
        return;
    }

    const std::string agents_dir = get_config_dir() + "/agents";
    fs::create_directories(agents_dir);

    int saved = 0;
    for (size_t idx : picked) {
        auto& s = starters[idx];

        // Adapt defaults so every installed starter works on first message.
        // If the starter's model refers to an unconfigured provider, swap in
        // the master model.  If its advisor refers to an unconfigured one,
        // drop it to unset (users can enter one explicitly if they want).
        if (!model_usable(s.config.model, keys) && !master_model.empty()) {
            s.config.model = master_model;
        }
        if (!model_usable(s.config.advisor_model, keys)) {
            s.config.advisor_model.clear();
        }

        auto mdl = ui.input("Configure " + s.id,
                            {
                                "Model id",
                                "Press Enter to keep the current value.",
                            },
                            s.config.model.empty() ? master_model : s.config.model,
                            false,
                            s.config.model);
        if (mdl && !mdl->empty()) s.config.model = *mdl;

        const std::string adv_display = s.config.advisor_model.empty()
            ? "unset"
            : s.config.advisor_model;
        auto adv = ui.input("Configure " + s.id,
                            {
                                "Advisor model",
                                "Use 'none' to unset. Press Enter to keep the current value.",
                            },
                            adv_display,
                            false,
                            s.config.advisor_model);
        if (adv) {
            std::string adv_lower = *adv;
            for (auto& c : adv_lower) c = static_cast<char>(std::tolower((unsigned char)c));
            if (adv_lower == "none") s.config.advisor_model.clear();
            else if (!adv->empty())  s.config.advisor_model = *adv;
        }

        s.config.save(agents_dir + "/" + s.id + ".json");
        ++saved;
    }

    ui.message("Starter agents saved",
               {"Saved " + std::to_string(saved) + " starter agent"
                    + (saved == 1 ? "" : "s") + ".",
                agents_dir});
}

// ── Tools wizard (search / browse / MCP) ─────────────────────────────

std::string mask_key_preview(const std::string& key) {
    if (key.empty()) return "(not set)";
    if (key.size() <= 8) return std::string(key.size(), '*');
    return key.substr(0, 4) + "…" + key.substr(key.size() - 4);
}

mcp::ServerSpec make_playwright_spec(bool headless) {
    mcp::ServerSpec s;
    s.name = "playwright";
    s.argv = {"npx", "-y", "@playwright/mcp@latest"};
    if (headless) s.argv.push_back("--headless");
    s.init_timeout = std::chrono::milliseconds(90000);
    s.call_timeout = std::chrono::milliseconds(30000);
    return s;
}

mcp::ServerSpec make_hosted_mcp_spec(const std::string& name,
                                     const std::string& url) {
    mcp::ServerSpec s;
    s.name = name;
    s.argv = {"npx", "-y", "mcp-remote", url};
    s.init_timeout = std::chrono::milliseconds(90000);
    return s;
}

void upsert_server(std::vector<mcp::ServerSpec>& specs, mcp::ServerSpec spec) {
    for (auto& existing : specs) {
        if (existing.name == spec.name) {
            existing = std::move(spec);
            return;
        }
    }
    specs.push_back(std::move(spec));
}

bool remove_server(std::vector<mcp::ServerSpec>& specs, const std::string& name) {
    const auto before = specs.size();
    specs.erase(std::remove_if(specs.begin(), specs.end(),
                               [&](const mcp::ServerSpec& s) {
                                   return s.name == name;
                               }),
                specs.end());
    return specs.size() != before;
}

bool has_server(const std::vector<mcp::ServerSpec>& specs, const std::string& name) {
    for (const auto& s : specs) if (s.name == name) return true;
    return false;
}

std::string argv_blurb(const mcp::ServerSpec& s) {
    std::string out;
    for (size_t i = 0; i < s.argv.size(); ++i) {
        if (i) out += " ";
        out += s.argv[i];
        if (out.size() > 56) {
            out.resize(53);
            out += "...";
            break;
        }
    }
    return out;
}

std::vector<std::string> tools_status_body(const std::string& search_key,
                                           const std::vector<mcp::ServerSpec>& specs) {
    const bool browse_ok = has_server(specs, "playwright");
    const bool from_env = search_key_from_env();
    std::string search_line = "Search:  not configured  (/search disabled)";
    if (!search_key.empty()) {
        search_line = std::string("Search:  Brave key ") + mask_key_preview(search_key)
            + (from_env ? "  (env)" : "  (file)");
    }
    std::vector<std::string> body = {
        "Configure the tools agents use mid-turn.",
        "",
        std::move(search_line),
        std::string("Browse:  ") + (browse_ok
            ? "playwright MCP ready  (/browse enabled)"
            : "not configured  (/browse needs playwright)"),
        std::string("MCP:     ") + std::to_string(specs.size())
            + (specs.size() == 1 ? " server" : " servers")
            + " in ~/.arbiter/mcp_servers.json",
    };
    if (!specs.empty()) {
        body.push_back("");
        constexpr size_t kMaxListed = 8;
        const size_t n = std::min(specs.size(), kMaxListed);
        for (size_t i = 0; i < n; ++i) {
            body.push_back("  • " + specs[i].name + " — " + argv_blurb(specs[i]));
        }
        if (specs.size() > kMaxListed) {
            body.push_back("  … and "
                + std::to_string(specs.size() - kMaxListed) + " more");
        }
    }
    return body;
}

bool persist_mcp_registry(SetupTui& ui, const std::vector<mcp::ServerSpec>& specs) {
    const std::string path = get_config_dir() + "/mcp_servers.json";
    try {
        if (!mcp::save_server_registry(path, specs)) {
            ui.message("Could not save MCP registry",
                       {"Failed to write " + path + ".",
                        "Check directory permissions and try again."});
            return false;
        }
    } catch (const std::exception& e) {
        ui.message("Could not save MCP registry",
                   {"Failed to serialize the registry:", e.what()});
        return false;
    }
    return true;
}

// Mutate `specs`, persist, and roll back the in-memory vector if the write
// fails so the UI never drifts from ~/.arbiter/mcp_servers.json.
bool mutate_and_persist(SetupTui& ui,
                        std::vector<mcp::ServerSpec>& specs,
                        const std::function<void(std::vector<mcp::ServerSpec>&)>& mut) {
    auto previous = specs;
    mut(specs);
    if (persist_mcp_registry(ui, specs)) return true;
    specs = std::move(previous);
    return false;
}

void wizard_step_search(SetupTui& ui) {
    while (true) {
        const std::string key = get_search_api_key();
        const bool from_env = search_key_from_env();
        const bool file_present = search_key_file_present();
        std::vector<std::string> body = {
            "Web search (/search)",
            "Uses the Brave Search API. Free tier: 2,000 queries/month.",
            "",
            std::string("Status: ") + (key.empty()
                ? "not configured"
                : (from_env ? "active via environment variable (overrides file)"
                            : "saved in ~/.arbiter/search_api_key")),
            std::string("Key:    ") + mask_key_preview(key),
            "",
            "Get a key: https://brave.com/search/api/",
        };
        if (from_env) {
            body.push_back("The env var wins until you unset it.");
        }
        std::vector<std::string> options;
        if (key.empty()) {
            options = {"Paste Brave Search API key"};
            // Orphan whitespace-only / empty file: offer removal.
            if (file_present) options.push_back("Remove empty key file");
            options.push_back("Back");
        } else if (from_env) {
            options = {"Write key to file (unused while env is set)"};
            if (file_present) options.push_back("Clear saved key file");
            options.push_back("Back");
        } else {
            options = {
                "Replace saved API key",
                "Clear saved API key",
                "Back",
            };
        }
        const int pick = ui.menu("Web search", body, options, 0);
        const int back = static_cast<int>(options.size()) - 1;
        if (pick < 0 || pick == back) return;
        // Clear/remove is the middle option whenever the menu has 3 entries.
        if (pick == 1 && options.size() == 3) {
            if (!clear_search_api_key()) {
                ui.message("Could not clear key",
                           {"Failed to remove ~/.arbiter/search_api_key."});
                continue;
            }
            if (from_env) {
                ui.message("Saved key file cleared",
                           {"~/.arbiter/search_api_key was removed.",
                            "Search still uses the environment variable until you unset it."});
            } else if (key.empty()) {
                ui.message("Empty key file removed",
                           {"~/.arbiter/search_api_key was removed."});
            } else {
                ui.message("Search key cleared",
                           {"/search will return ERR until a key is configured again."});
            }
            continue;
        }
        auto pasted = ui.input("Brave Search API key",
                               {
                                   "Paste your Brave Search API key.",
                                   "Saved to ~/.arbiter/search_api_key (mode 0600).",
                                   from_env
                                       ? "Env var still overrides this file until unset."
                                       : "ARBITER_SEARCH_API_KEY overrides the file.",
                               },
                               "BSA…",
                               true);
        if (!pasted) continue;
        if (!set_search_api_key(*pasted)) {
            ui.message("Could not save key",
                       {"Key was empty after trimming, or the file could not be written."});
            continue;
        }
        if (from_env) {
            ui.message("Key written to file",
                       {"Saved ~/.arbiter/search_api_key.",
                        "Runtime still uses the environment variable.",
                        "Unset ARBITER_SEARCH_API_KEY / BRAVE_SEARCH_API_KEY to use the file."});
        } else {
            ui.message("Search key saved",
                       {"Agents can use /search on the next launch.",
                        "Restart a running arbiter process to pick up the change."});
        }
    }
}

void wizard_step_browse(SetupTui& ui, std::vector<mcp::ServerSpec>& specs) {
    while (true) {
        const bool ready = has_server(specs, "playwright");
        std::vector<std::string> body = {
            "Browse (/browse)",
            "/browse is a convenience over the playwright MCP server:",
            "navigate → accessibility snapshot for JS-rendered pages.",
            "",
            std::string("Status: ") + (ready
                ? "playwright MCP configured"
                : "not configured — /browse will ERR"),
            "",
            "Requires Node.js + npx. First run may download Chromium.",
        };
        std::vector<std::string> options;
        if (ready) {
            options = {
                "Reinstall Playwright (headless)",
                "Reinstall Playwright (headed)",
                "Remove Playwright",
                "Back",
            };
        } else {
            options = {
                "Enable Playwright (headless) — recommended",
                "Enable Playwright (headed)",
                "Back",
            };
        }
        const int pick = ui.menu("Browse", body, options, 0);
        if (pick < 0) return;
        if (!ready && pick == 2) return;
        if (ready && pick == 3) return;
        if (ready && pick == 2) {
            if (mutate_and_persist(ui, specs, [](auto& s) {
                    remove_server(s, "playwright");
                })) {
                ui.message("Playwright removed",
                           {"/browse is disabled until playwright is added again.",
                            "Other MCP servers were left unchanged."});
            }
            continue;
        }
        const bool headless = (pick == 0);
        if (mutate_and_persist(ui, specs, [&](auto& s) {
                upsert_server(s, make_playwright_spec(headless));
            })) {
            ui.message(headless ? "Playwright enabled (headless)"
                                : "Playwright enabled (headed)",
                       {"Wrote playwright entry to ~/.arbiter/mcp_servers.json.",
                        "/browse and /mcp call playwright … are ready",
                        "after the next arbiter launch."});
        }
    }
}

std::vector<std::string> split_args(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool valid_server_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    // Reject names that are only dots — they collide with path semantics
    // and are useless as /mcp call targets.
    bool any_alnum = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) any_alnum = true;
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return any_alnum;
}

// Trim + lowercase so /browse's hard-coded "playwright" name and /mcp
// call targets stay consistent regardless of how the operator typed them.
std::string normalize_server_name(std::string name) {
    name = trim_ws(std::move(name));
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

// Lowercase the URI scheme; reject whitespace (breaks argv / mcp-remote).
std::string normalize_mcp_url(std::string url) {
    url = trim_ws(std::move(url));
    const auto scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        for (size_t i = 0; i < scheme_end; ++i) {
            url[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(url[i])));
        }
    }
    return url;
}

bool valid_mcp_url(const std::string& url) {
    if (url.find_first_of(" \t\r\n") != std::string::npos) return false;
    if (url.rfind("https://", 0) == 0) return url.size() > std::strlen("https://");
    if (url.rfind("http://", 0) == 0)  return url.size() > std::strlen("http://");
    return false;
}

struct CanonicalizeResult {
    bool renamed = false;
    size_t dropped = 0;
    bool changed() const { return renamed || dropped > 0; }
};

// Lowercase names + collapse case-only duplicates so a hand-edited
// "Playwright" entry can't silently break /browse (exact "playwright").
// Also drops entries with empty argv or invalid names.
CanonicalizeResult canonicalize_registry(std::vector<mcp::ServerSpec>& specs) {
    std::map<std::string, mcp::ServerSpec> by_name;
    CanonicalizeResult result;
    for (auto& s : specs) {
        auto n = normalize_server_name(s.name);
        if (n != s.name) result.renamed = true;
        s.name = std::move(n);
        if (s.name.empty() || !valid_server_name(s.name) || s.argv.empty()) {
            ++result.dropped;
            continue;
        }
        auto [it, inserted] = by_name.emplace(s.name, s);
        if (!inserted) {
            ++result.dropped;  // keep the first, drop the duplicate
            (void)it;
        }
    }
    specs.clear();
    for (auto& [_, s] : by_name) specs.push_back(std::move(s));
    return result;
}

// Ask before overwriting an existing registry entry.  Returns true when
// the name is free or the user explicitly chose Replace.
bool confirm_overwrite_server(SetupTui& ui,
                              const std::vector<mcp::ServerSpec>& specs,
                              const std::string& name) {
    if (!has_server(specs, name)) return true;
    const int pick = ui.menu("Replace existing server?",
                             {
                                 "A server named '" + name + "' is already configured.",
                                 "Replacing it overwrites that entry in mcp_servers.json.",
                             },
                             {"Replace '" + name + "'", "Cancel"},
                             1);
    return pick == 0;
}

void wizard_step_mcp(SetupTui& ui, std::vector<mcp::ServerSpec>& specs) {
    while (true) {
        auto body = tools_status_body(get_search_api_key(), specs);
        body.insert(body.begin(), "MCP servers");
        body.insert(body.begin() + 1, "stdio servers registered in ~/.arbiter/mcp_servers.json");
        const int pick = ui.menu("MCP servers",
                                 body,
                                 {
                                     "Add Playwright (browse)",
                                     "Add hosted MCP (mcp-remote)",
                                     "Add custom stdio server",
                                     "Remove a server",
                                     "Back",
                                 },
                                 0);
        if (pick < 0 || pick == 4) return;

        if (pick == 0) {
            if (!confirm_overwrite_server(ui, specs, "playwright")) continue;
            if (mutate_and_persist(ui, specs, [](auto& s) {
                    upsert_server(s, make_playwright_spec(true));
                })) {
                ui.message("Playwright added",
                           {"Server name: playwright",
                            "Command: npx -y @playwright/mcp@latest --headless"});
            }
            continue;
        }

        if (pick == 1) {
            auto name_in = ui.input("Hosted MCP server",
                                    {
                                        "Logical name agents will use in /mcp call <name> …",
                                        "Stored lowercase. Examples: sentry, linear, slack",
                                    },
                                    "sentry",
                                    false);
            if (!name_in) continue;
            const std::string name = normalize_server_name(*name_in);
            if (!valid_server_name(name)) {
                ui.message("Invalid name",
                           {"Use letters, digits, '.', '_', or '-' (max 64 chars).",
                            "Names must include at least one letter or digit."});
                continue;
            }
            auto url_in = ui.input("Hosted MCP server",
                                   {
                                       "HTTPS MCP endpoint URL.",
                                       "Proxied through npx mcp-remote (OAuth on first connect).",
                                   },
                                   "https://mcp.example.com/mcp",
                                   false);
            if (!url_in) continue;
            const std::string url = normalize_mcp_url(*url_in);
            if (!valid_mcp_url(url)) {
                ui.message("Invalid URL",
                           {"Enter a full http:// or https:// MCP endpoint",
                            "(no spaces; scheme alone is not enough)."});
                continue;
            }
            if (!confirm_overwrite_server(ui, specs, name)) continue;
            if (mutate_and_persist(ui, specs, [&](auto& s) {
                    upsert_server(s, make_hosted_mcp_spec(name, url));
                })) {
                ui.message("Hosted MCP added",
                           {"Server '" + name + "' → " + url,
                            "First connect may open a browser for OAuth.",
                            "Tokens persist under ~/.mcp-auth/."});
            }
            continue;
        }

        if (pick == 2) {
            auto name_in = ui.input("Custom MCP server",
                                    {
                                        "Logical name for /mcp call <name> …",
                                        "Stored lowercase.",
                                    },
                                    "my-server",
                                    false);
            if (!name_in) continue;
            const std::string name = normalize_server_name(*name_in);
            if (!valid_server_name(name)) {
                ui.message("Invalid name",
                           {"Use letters, digits, '.', '_', or '-' (max 64 chars).",
                            "Names must include at least one letter or digit."});
                continue;
            }
            if (!confirm_overwrite_server(ui, specs, name)) continue;
            auto command_in = ui.input("Custom MCP server",
                                       {"Executable (PATH-resolved)."},
                                       "npx",
                                       false);
            if (!command_in) continue;
            const std::string command = trim_ws(*command_in);
            if (command.empty()) continue;
            // Single PATH token only — spaces would be one broken argv[0].
            if (command.find_first_of(" \t") != std::string::npos) {
                ui.message("Invalid command",
                           {"Use a single executable name or path (no spaces).",
                            "Put flags in the arguments field instead."});
                continue;
            }
            auto args_line = ui.input("Custom MCP server",
                                      {
                                          "Arguments after the command (space-separated).",
                                          "Enter with empty field for none. Esc cancels.",
                                          "Quoting is not supported.",
                                      },
                                      "-y some-mcp-package",
                                      false,
                                      "");
            // Esc aborts the whole add — Enter on an empty field means no args.
            if (!args_line) continue;
            const std::string args_trimmed = trim_ws(*args_line);
            if (mutate_and_persist(ui, specs, [&](auto& all) {
                    mcp::ServerSpec s;
                    s.name = name;
                    s.argv = {command};
                    for (auto& a : split_args(args_trimmed))
                        s.argv.push_back(std::move(a));
                    upsert_server(all, std::move(s));
                })) {
                ui.message("Custom MCP server added",
                           {"Wrote '" + name + "' to ~/.arbiter/mcp_servers.json."});
            }
            continue;
        }

        if (pick == 3) {
            if (specs.empty()) {
                ui.message("No servers to remove",
                           {"The MCP registry is empty."});
                continue;
            }
            std::vector<std::string> options;
            for (const auto& s : specs) options.push_back(s.name + " — " + argv_blurb(s));
            options.push_back("Cancel");
            const int which = ui.menu("Remove MCP server",
                                      {"Select a server to remove from the registry."},
                                      options,
                                      0);
            if (which < 0 || which == static_cast<int>(options.size()) - 1) continue;
            const std::string removed = specs[static_cast<size_t>(which)].name;
            if (mutate_and_persist(ui, specs, [&](auto& s) {
                    remove_server(s, removed);
                })) {
                ui.message("Server removed",
                           {"Removed '" + removed + "' from ~/.arbiter/mcp_servers.json."});
            }
        }
    }
}

void run_tools_setup_wizard() {
    SetupTui ui;
    std::vector<mcp::ServerSpec> specs;
    try {
        specs = mcp::load_server_registry(get_config_dir() + "/mcp_servers.json");
    } catch (const std::exception& e) {
        ui.message("MCP registry error",
                   {"Could not parse ~/.arbiter/mcp_servers.json:",
                    e.what(),
                    "Fix or delete the file, then re-run --setup-tools."});
        return;
    }
    {
        const auto canon = canonicalize_registry(specs);
        if (canon.changed()) {
            if (persist_mcp_registry(ui, specs)) {
                std::vector<std::string> lines;
                if (canon.renamed) {
                    lines.push_back("Server names were lowercased so /browse and");
                    lines.push_back("/mcp call targets stay consistent.");
                }
                if (canon.dropped > 0) {
                    lines.push_back("Removed " + std::to_string(canon.dropped)
                        + " invalid or duplicate entr"
                        + (canon.dropped == 1 ? "y." : "ies."));
                }
                if (lines.empty()) {
                    lines.push_back("Registry cleaned up.");
                }
                ui.message("MCP registry normalized", lines);
            }
        }
    }

    ui.message("Agent tools",
               {"Configure web search, browse, and MCP servers.",
                "Changes write under ~/.arbiter/ and apply on next launch.",
                "Use arrow keys to move. Press Enter to select."});

    while (true) {
        const auto body = tools_status_body(get_search_api_key(), specs);
        const int pick = ui.menu("Agent tools",
                                 body,
                                 {
                                     "Web search (/search)",
                                     "Browse (/browse via Playwright)",
                                     "MCP servers",
                                     "Done",
                                 },
                                 0);
        if (pick < 0) {
            ui.message("Leaving tools setup",
                       {"Any changes already written under ~/.arbiter/ are kept.",
                        "Restart arbiter to load them into a running session."});
            return;
        }
        if (pick == 3) {
            ui.message("Tools setup complete",
                       {"Search key → ~/.arbiter/search_api_key (if set)",
                        "MCP registry → ~/.arbiter/mcp_servers.json",
                        "Restart arbiter to load changes into a running session."});
            return;
        }
        if (pick == 0) wizard_step_search(ui);
        else if (pick == 1) wizard_step_browse(ui, specs);
        else if (pick == 2) wizard_step_mcp(ui, specs);
    }
}

// Top-level first-run wizard.
std::map<std::string, std::string> run_key_setup_wizard() {
    std::map<std::string, std::string> keys;
    bool configure_tools = false;
    {
        SetupTui ui;
        ui.message("Welcome to index",
                   {"Let's get you set up — about a minute.",
                    "Use arrow keys to move. Press Enter to select."});
        wizard_step_keys(ui, keys);
        std::string master_model = wizard_step_default_model(ui, keys);
        wizard_step_starter_agents(ui, keys, master_model);

        const int tools = ui.menu("Configure agent tools?",
                                  {
                                      "Optional: enable /search, /browse, and MCP servers.",
                                      "You can also run this later with arbiter --setup-tools.",
                                  },
                                  {
                                      "Configure search / browse / MCP",
                                      "Skip for now",
                                  },
                                  0);
        if (tools == 0) {
            configure_tools = true;
        } else {
            ui.message("Setup complete",
                       {"Configuration is ready.",
                        "Run arbiter to launch the terminal client.",
                        "Later: arbiter --setup-tools for search / browse / MCP."});
        }
    }  // SetupTui restored the terminal before the tools wizard starts.

    if (configure_tools) {
        run_tools_setup_wizard();
        SetupTui done;
        done.message("Setup complete",
                     {"Configuration is ready.",
                      "Run arbiter to launch the terminal client."});
    }
    return keys;
}

}  // namespace

std::map<std::string, std::string> get_api_keys() {
    const std::string dir = get_config_dir();
    std::map<std::string, std::string> out;

    auto openrouter = load_key("OPENROUTER_API_KEY", dir + "/openrouter_api_key");
    if (!openrouter.empty()) out["openrouter"] = std::move(openrouter);

    if (out.empty()) {
        // First-run path: interactive wizard when stdin is a TTY.  Scripted
        // callers (pipes, CI, --send from a cronjob) still error-exit so
        // automation doesn't silently block on a prompt that nobody sees.
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            out = run_key_setup_wizard();
        } else {
            std::cerr << "ERR: No provider API keys configured. Set one of:\n"
                      << "  OPENROUTER_API_KEY (or write to ~/.arbiter/openrouter_api_key)\n";
            std::exit(1);
        }
    }
    return out;
}

std::string get_search_api_key() {
    if (const char* k = std::getenv("ARBITER_SEARCH_API_KEY"); k && *k) {
        return trim_ws(k);
    }
    if (const char* k = std::getenv("BRAVE_SEARCH_API_KEY"); k && *k) {
        return trim_ws(k);
    }
    return trim_ws(load_key_file(get_config_dir() + "/search_api_key"));
}

bool set_search_api_key(const std::string& key) {
    const std::string trimmed = trim_ws(key);
    if (trimmed.empty()) return false;
    return write_key_file("search_api_key", trimmed);
}

bool clear_search_api_key() {
    const std::string path = get_config_dir() + "/search_api_key";
    if (::unlink(path.c_str()) == 0) return true;
    return errno == ENOENT;
}

void cmd_setup_tools() {
    if (!(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))) {
        std::cerr << "ERR: --setup-tools requires an interactive terminal\n";
        std::exit(1);
    }
    run_tools_setup_wizard();
}

namespace {

int winsize_dim(int which) {
    struct winsize ws{};
    const int fds[] = {STDOUT_FILENO, STDIN_FILENO, STDERR_FILENO};
    for (int fd : fds) {
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
            const int v = (which == 0) ? ws.ws_col : ws.ws_row;
            if (v > 0) return v;
        }
    }
    return 0;
}

} // namespace

int term_cols() {
    const int cols = winsize_dim(0);
    return cols > 0 ? cols : 80;
}

int term_rows() {
    const int rows = winsize_dim(1);
    return rows > 0 ? rows : 24;
}

void drain_stdin_spurious(int max_wait_ms) {
    const int old_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    if (old_flags >= 0)
        ::fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);

    char buf[4096];
    auto drain_available = [&]() {
        while (true) {
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
        }
    };

    if (max_wait_ms <= 0) {
        drain_available();
    } else {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(max_wait_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            timeval tv = {0, 25000};
            const int r = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
            if (r > 0) drain_available();
        }
        drain_available();
    }

    if (old_flags >= 0)
        ::fcntl(STDIN_FILENO, F_SETFL, old_flags);
}

} // namespace arbiter
