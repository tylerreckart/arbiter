// index/src/cli_helpers.cpp — see cli_helpers.h

#include "cli_helpers.h"
#include "commands.h"
#include "starters.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

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
    const char* name;       // "anthropic"
    const char* display;    // "Anthropic (Claude models)"
    const char* key_file;   // "api_key"
    const char* env_var;    // "ANTHROPIC_API_KEY"
    const char* hint;       // key-format hint shown to the user
    const char* console_url;// where to grab a key
};

constexpr ProviderSetup kProviderSetups[] = {
    { "anthropic",
      "Anthropic (Claude models)",
      "api_key",
      "ANTHROPIC_API_KEY",
      "starts with sk-ant-",
      "https://console.anthropic.com/settings/keys" },
    { "openai",
      "OpenAI (GPT / o-series models)",
      "openai_api_key",
      "OPENAI_API_KEY",
      "starts with sk-",
      "https://platform.openai.com/api-keys" },
};
constexpr size_t kNumProviders = sizeof(kProviderSetups) / sizeof(kProviderSetups[0]);

// Recommended models shown on the "pick a default" step.  The wizard filters
// these by which providers the user configured.  The first entry of a given
// provider acts as its recommended default if the user presses Enter.
struct ModelOption {
    const char* provider;   // "anthropic" | "openai"
    const char* id;         // model id passed to the API router
    const char* blurb;      // one-liner shown to the user
};

constexpr ModelOption kModelOptions[] = {
    { "anthropic", "claude-sonnet-4-6",   "Sonnet  — balanced quality/speed (recommended)" },
    { "anthropic", "claude-haiku-4-5",    "Haiku   — fast and cheap" },
    { "anthropic", "claude-opus-4-7",     "Opus    — highest quality, slowest" },
    { "openai",    "openai/gpt-4.1",      "GPT-4.1 — balanced (recommended for OpenAI)" },
    { "openai",    "openai/gpt-4o",       "GPT-4o  — flagship chat model" },
    { "openai",    "openai/gpt-4o-mini",  "GPT-4o-mini — fast and cheap" },
    { "openai",    "openai/gpt-5.4",      "GPT-5.4 — newest frontier model" },
    { "openai",    "openai/o4-mini",      "o4-mini — reasoning model" },
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

// Write the key to ~/.index/<filename> with mode 0600.  Opens with the
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

// Write the chosen master-model id to ~/.index/master_model.  Orchestrator
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
void wizard_step_keys(std::map<std::string, std::string>& keys) {
    std::cout << "[1/3] Provider API keys\n"
              << "───────────────────────\n"
              << "Set up at least one provider.  Keys are saved to ~/.index/\n"
              << "with mode 0600; environment variables override the files.\n\n";

    while (true) {
        std::cout << "Providers:\n";
        for (size_t i = 0; i < kNumProviders; ++i) {
            const auto& ps = kProviderSetups[i];
            std::cout << "  " << (i + 1) << ") " << ps.display;
            if (keys.count(ps.name)) std::cout << "  [configured]";
            std::cout << "\n";
        }
        std::cout << "  d) done\n\n"
                  << "Choice [1-" << kNumProviders << ", d]: " << std::flush;

        std::string choice = read_line_or_exit();

        if (choice == "d" || choice == "D" || choice == "done" || choice.empty()) {
            if (keys.empty()) {
                std::cout << "\nNeed at least one provider.  Press Ctrl-C to abort.\n\n";
                continue;
            }
            std::cout << "\n";
            return;
        }

        size_t idx = 0;
        try { idx = std::stoul(choice); } catch (...) { idx = 0; }
        if (idx < 1 || idx > kNumProviders) {
            std::cout << "\nInvalid choice.\n\n";
            continue;
        }
        const auto& ps = kProviderSetups[idx - 1];

        std::cout << "\n" << ps.display << "\n"
                  << "  Get a key: " << ps.console_url << "\n"
                  << "  Saves to ~/.index/" << ps.key_file << " (mode 0600).\n"
                  << "  " << ps.env_var << " overrides the file if set later.\n"
                  << "  Paste key (" << ps.hint
                  << ", or blank to cancel): " << std::flush;

        const std::string key = read_secret_line();
        if (key.empty()) {
            std::cout << "(cancelled)\n\n";
            continue;
        }
        if (!write_key_file(ps.key_file, key)) {
            std::cerr << "ERR: could not write "
                      << get_config_dir() << "/" << ps.key_file << "\n\n";
            continue;
        }
        keys[ps.name] = key;
        std::cout << "Saved.\n\n";
    }
}

// Step 2: pick the default model for the master "index" agent.  Shown even
// when only one model is available so the user knows what they're getting.
// Enter accepts the first (recommended) option.  The choice is persisted to
// ~/.index/master_model; the orchestrator applies it at startup.
// Returns the chosen model id so Step 3 can use it as a fallback when a
// starter's default model refers to a provider the user didn't configure.
std::string wizard_step_default_model(
    const std::map<std::string, std::string>& keys) {
    std::vector<const ModelOption*> available;
    for (const auto& m : kModelOptions) {
        if (keys.count(m.provider)) available.push_back(&m);
    }
    if (available.empty()) return {};  // shouldn't happen — step 1 guarantees a key

    std::cout << "[2/3] Default model\n"
              << "───────────────────\n"
              << "The \"index\" master agent will use this model for routing\n"
              << "and orchestration.  You can change it any time with\n"
              << "/model index <model-id>.\n\n";

    for (size_t i = 0; i < available.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << available[i]->blurb << "\n";
    }
    std::cout << "\nChoice [1-" << available.size() << ", Enter for 1]: " << std::flush;

    size_t pick = 1;
    while (true) {
        std::string choice = read_line_or_exit();
        if (choice.empty()) { pick = 1; break; }
        try { pick = std::stoul(choice); } catch (...) { pick = 0; }
        if (pick >= 1 && pick <= available.size()) break;
        std::cout << "Invalid choice.  Choice [1-" << available.size()
                  << ", Enter for 1]: " << std::flush;
    }

    const auto* chosen = available[pick - 1];
    if (!write_master_model(chosen->id)) {
        // Non-fatal: orchestrator still falls back to a sensible default
        // based on which keys are configured.
        std::cerr << "WARN: could not write ~/.index/master_model — "
                     "will use default each session.\n";
    }
    std::cout << "Default model set to " << chosen->id << ".\n\n";
    return chosen->id;
}

// Check that a model string can route to a configured provider.  Used when
// computing per-starter defaults in Step 3 — if a starter's default model
// refers to a provider the user didn't set up, we swap in the master model
// so the starter works on first message instead of erroring.
bool model_usable(const std::string& model,
                  const std::map<std::string, std::string>& keys) {
    if (model.empty()) return true;
    if (model.rfind("openai/", 0) == 0) return keys.count("openai") > 0;
    if (model.rfind("ollama/", 0) == 0) return true;   // ollama is keyless
    // Bare ids default to Anthropic in the provider registry.
    return keys.count("anthropic") > 0;
}

// Step 3: multi-select starter agents; let the user customize each one's
// model and advisor before saving to ~/.index/agents/<id>.json.
void wizard_step_starter_agents(
    const std::map<std::string, std::string>& keys,
    const std::string& master_model) {
    auto starters = starter_agents();
    if (starters.empty()) return;

    std::cout << "[3/3] Starter agents\n"
              << "────────────────────\n"
              << "Pick example agents to install.  You can customize each\n"
              << "one's model and advisor below, or accept the defaults.\n"
              << "Skip the whole step by pressing Enter with no numbers.\n\n";

    size_t max_id_len = 0;
    for (auto& s : starters) max_id_len = std::max(max_id_len, s.id.size());
    for (size_t i = 0; i < starters.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << starters[i].id
                  << std::string(max_id_len - starters[i].id.size() + 2, ' ')
                  << starters[i].blurb << "\n";
    }
    std::cout << "\nPick numbers (e.g. '1 3 5', 'all', Enter to skip): "
              << std::flush;

    std::string choice = read_line_or_exit();
    if (choice.empty()) {
        std::cout << "Skipped starter agents.  Run `index --init` later to\n"
                  << "create them, or make your own in ~/.index/agents/.\n\n";
        return;
    }

    std::set<size_t> picked;
    std::string lower = choice;
    for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
    if (lower == "all") {
        for (size_t i = 0; i < starters.size(); ++i) picked.insert(i);
    } else {
        std::string buf;
        auto flush_num = [&]() {
            if (buf.empty()) return;
            try {
                size_t n = std::stoul(buf);
                if (n >= 1 && n <= starters.size()) picked.insert(n - 1);
            } catch (...) { /* ignore non-numeric tokens */ }
            buf.clear();
        };
        for (char c : choice) {
            if (std::isdigit(static_cast<unsigned char>(c))) buf += c;
            else flush_num();
        }
        flush_num();
    }

    if (picked.empty()) {
        std::cout << "No valid selections.  Skipping.\n\n";
        return;
    }

    const std::string agents_dir = get_config_dir() + "/agents";
    fs::create_directories(agents_dir);

    std::cout << "\n";
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

        std::cout << "configure '" << s.id << "'\n";
        std::cout << "  model     [" << s.config.model << "]: " << std::flush;
        std::string mdl = read_line_or_exit();
        if (!mdl.empty()) s.config.model = mdl;

        const std::string adv_display = s.config.advisor_model.empty()
            ? "unset"
            : s.config.advisor_model;
        std::cout << "  advisor   [" << adv_display
                  << "]  (Enter = keep, 'none' = unset): " << std::flush;
        std::string adv = read_line_or_exit();
        std::string adv_lower = adv;
        for (auto& c : adv_lower) c = static_cast<char>(std::tolower((unsigned char)c));
        if (adv_lower == "none") s.config.advisor_model.clear();
        else if (!adv.empty())   s.config.advisor_model = adv;

        s.config.save(agents_dir + "/" + s.id + ".json");
        ++saved;
        std::cout << "\n";
    }

    std::cout << "Saved " << saved << " starter agent"
              << (saved == 1 ? "" : "s") << " to " << agents_dir << "/\n\n";
}

// Top-level first-run wizard.
std::map<std::string, std::string> run_key_setup_wizard() {
    std::cout << "\n"
              << "Welcome to index.\n"
              << "Let's get you set up — about a minute.\n\n";

    std::map<std::string, std::string> keys;
    wizard_step_keys(keys);
    std::string master_model = wizard_step_default_model(keys);
    wizard_step_starter_agents(keys, master_model);

    std::cout << "Setup complete.\n\n";
    return keys;
}

}  // namespace

std::map<std::string, std::string> get_api_keys() {
    const std::string dir = get_config_dir();
    std::map<std::string, std::string> out;

    auto anthropic = load_key("ANTHROPIC_API_KEY", dir + "/api_key");
    if (!anthropic.empty()) out["anthropic"] = std::move(anthropic);

    auto openai = load_key("OPENAI_API_KEY", dir + "/openai_api_key");
    if (!openai.empty()) out["openai"] = std::move(openai);

    if (out.empty()) {
        // First-run path: interactive wizard when stdin is a TTY.  Scripted
        // callers (pipes, CI, --send from a cronjob) still error-exit so
        // automation doesn't silently block on a prompt that nobody sees.
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            out = run_key_setup_wizard();
        } else {
            std::cerr << "ERR: No provider API keys configured. Set one of:\n"
                      << "  ANTHROPIC_API_KEY  (or write to ~/.index/api_key)\n"
                      << "  OPENAI_API_KEY     (or write to ~/.index/openai_api_key)\n";
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

} // namespace index_ai
