// arbiter/src/cli_helpers.cpp — see cli_helpers.h

#include "cli_helpers.h"
#include "commands.h"
#include "starters.h"
#include "theme.h"
#include "tui/opentui/engine.h"
#include "tui/tui_design.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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

// Resolve a provider API key.  Order of precedence:
//   1. env var  — short-circuits the file path entirely
//   2. file on disk — but ONLY if it's owned by the current user and has
//      0600-compatible permissions.  If the file exists under a different
//      uid, treat as tamper and exit (a hostile local user could swap in
//      a key that bills to their account).  If the perms are looser than
//      owner-only, warn and tighten in-place so the next startup is clean.
// Empty return = "no key found for this provider" — the caller falls back
// to whatever that means (first-run wizard for get_api_keys, or a clear
// "no API key configured" failure at the ApiClient wire layer).
std::string load_key(const char* env_var, const std::string& file_path) {
    const char* env = std::getenv(env_var);
    if (env && env[0]) return env;

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
        drain_stdin_spurious(100);
        engine_.shutdown_terminal();
        if (have_saved_stdin_) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_stdin_);
        }
        drain_stdin_spurious(300);
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

// Top-level first-run wizard.
std::map<std::string, std::string> run_key_setup_wizard() {
    SetupTui ui;
    std::map<std::string, std::string> keys;
    ui.message("Welcome to index",
               {"Let's get you set up — about a minute.",
                "Use arrow keys to move. Press Enter to select."});
    wizard_step_keys(ui, keys);
    std::string master_model = wizard_step_default_model(ui, keys);
    wizard_step_starter_agents(ui, keys, master_model);

    ui.message("Setup complete",
               {"Configuration is ready.",
                "Run arbiter to launch the terminal client."});
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
