// index/src/readline_wrapper.cpp — readline / libedit wrapper

#include "readline_wrapper.h"

#include <cstring>
#include <cstdlib>

#ifdef INDEX_AI_HAS_READLINE
#  ifdef INDEX_AI_USE_EDITLINE
#    include <editline/readline.h>
#  else
#    include <readline/readline.h>
#    include <readline/history.h>
#  endif
#else
#  include <iostream>
#endif

namespace index_ai {

// ─── Global state (readline uses C callbacks) ────────────────────────────────

#ifdef INDEX_AI_HAS_READLINE
static CompletionProvider g_provider;
static std::function<void(const std::string&)> g_line_callback;

static char* completion_generator(const char* text, int state) {
    static std::vector<std::string> matches;
    static size_t idx = 0;

    if (state == 0) {
        if (g_provider) {
            matches = g_provider(
                std::string(rl_line_buffer),
                std::string(text));
        } else {
            matches.clear();
        }
        idx = 0;
    }
    if (idx >= matches.size()) return nullptr;
    return ::strdup(matches[idx++].c_str());
}

static char** completion_callback(const char* text, int /*start*/, int /*end*/) {
    rl_attempted_completion_over = 1;  // suppress filename fallback
    if (!g_provider) return nullptr;
    return rl_completion_matches(text, completion_generator);
}

static void rl_line_handler(char* raw) {
    if (!raw) {
        // EOF / Ctrl-D
        if (g_line_callback) g_line_callback("");
        return;
    }
    std::string line(raw);
    if (!line.empty()) ::add_history(raw);
    ::free(raw);
    if (g_line_callback) g_line_callback(line);
}
#endif

// ─── ReadlineWrapper ─────────────────────────────────────────────────────────

ReadlineWrapper::ReadlineWrapper() {
#ifdef INDEX_AI_HAS_READLINE
    rl_attempted_completion_function = completion_callback;
    rl_completion_append_character   = ' ';
#endif
}

ReadlineWrapper::~ReadlineWrapper() {
    if (!history_path_.empty()) {
        save_history(history_path_);
    }
}

bool ReadlineWrapper::read_line(const std::string& prompt, std::string& out) {
#ifdef INDEX_AI_HAS_READLINE
    char* raw = ::readline(prompt.c_str());
    if (!raw) return false;  // EOF / Ctrl-D
    out = raw;
    if (!out.empty()) ::add_history(raw);
    ::free(raw);
    return true;
#else
    std::cout << prompt;
    std::cout.flush();
    return static_cast<bool>(std::getline(std::cin, out));
#endif
}

void ReadlineWrapper::set_completion_provider(CompletionProvider provider) {
#ifdef INDEX_AI_HAS_READLINE
    g_provider = std::move(provider);
#else
    (void)provider;
#endif
}

void ReadlineWrapper::load_history(const std::string& path) {
    history_path_ = path;
#ifdef INDEX_AI_HAS_READLINE
    ::read_history(path.c_str());  // silently ignored if file doesn't exist
#endif
}

void ReadlineWrapper::save_history(const std::string& path) {
#ifdef INDEX_AI_HAS_READLINE
    if (max_history_ > 0) ::stifle_history(max_history_);
    ::write_history(path.c_str());
#else
    (void)path;
#endif
}

void ReadlineWrapper::set_max_history(int n) {
    max_history_ = n;
#ifdef INDEX_AI_HAS_READLINE
    ::stifle_history(n);
#endif
}

void ReadlineWrapper::redisplay() {
#ifdef INDEX_AI_HAS_READLINE
    ::rl_on_new_line();
    ::rl_redisplay();
#endif
}

void ReadlineWrapper::on_new_line() {
#ifdef INDEX_AI_HAS_READLINE
    ::rl_on_new_line();
#endif
}

void ReadlineWrapper::initialize() {
#ifdef INDEX_AI_HAS_READLINE
    ::rl_initialize();
#endif
}

void ReadlineWrapper::install_callback(const std::string& prompt,
                                        std::function<void(const std::string&)> on_line) {
#ifdef INDEX_AI_HAS_READLINE
    g_line_callback = std::move(on_line);
    ::rl_callback_handler_install(prompt.c_str(), rl_line_handler);
#else
    (void)prompt;
    (void)on_line;
#endif
}

void ReadlineWrapper::process_char() {
#ifdef INDEX_AI_HAS_READLINE
    ::rl_callback_read_char();
#endif
}

void ReadlineWrapper::remove_callback() {
#ifdef INDEX_AI_HAS_READLINE
    ::rl_callback_handler_remove();
    g_line_callback = nullptr;
#endif
}

void ReadlineWrapper::stuff_char(int c) {
#ifdef INDEX_AI_HAS_READLINE
    ::rl_stuff_char(c);
#else
    (void)c;
#endif
}


void ReadlineWrapper::set_instream(FILE* f) {
#ifdef INDEX_AI_HAS_READLINE
    rl_instream = f;
#else
    (void)f;
#endif
}

void ReadlineWrapper::set_getc_function(int (*fn)(FILE *)) {
#ifdef INDEX_AI_HAS_READLINE
    // Capture the library default on first call so we can restore it.
    static int (*s_original)(FILE *) = rl_getc_function;
    rl_getc_function = fn ? fn : s_original;
#endif
}

std::string ReadlineWrapper::current_buffer() const {
#ifdef INDEX_AI_HAS_READLINE
    return rl_line_buffer ? std::string(rl_line_buffer) : "";
#else
    return "";
#endif
}

int ReadlineWrapper::current_display_rows(int term_cols) const {
    if (term_cols <= 0) return 1;
#ifdef INDEX_AI_HAS_READLINE
    // The prompt is "\001ESC...\002>\001ESC...\002 " — 2 visible chars ("> ").
    const int kPromptVisible = 2;
    int buf_len = rl_line_buffer ? static_cast<int>(strlen(rl_line_buffer)) : 0;
    int total = kPromptVisible + buf_len;
    return std::max(1, (total + term_cols - 1) / term_cols);
#else
    return 1;
#endif
}

} // namespace index_ai
