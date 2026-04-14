#pragma once
// index/include/readline_wrapper.h — Line editing abstraction (readline / libedit)

#include <string>
#include <vector>
#include <functional>

namespace index_ai {

// Completion provider: given current buffer and token being completed,
// return a list of candidate strings.
using CompletionProvider =
    std::function<std::vector<std::string>(
        const std::string& buffer,
        const std::string& token)>;

class ReadlineWrapper {
public:
    ReadlineWrapper();
    ~ReadlineWrapper();

    // Read one line with the given prompt.
    // prompt may contain ANSI escapes wrapped in \001/\002 markers for readline.
    // Returns false on EOF (Ctrl-D).
    bool read_line(const std::string& prompt, std::string& out);

    // Register a tab-completion provider.
    void set_completion_provider(CompletionProvider provider);

    // Load history from file (silently ignored if file doesn't exist).
    // Also stores the path for auto-save on destruction.
    void load_history(const std::string& path);

    // Save history to file.
    void save_history(const std::string& path);

    // Max history entries kept in memory and file.
    void set_max_history(int n);

    // Tell readline the cursor is now on a fresh line and redraw the prompt.
    // Only safe to call from the readline thread (main thread).
    void redisplay();

    // Tell readline the cursor is on a fresh line (no redraw).
    void on_new_line();

    // Push a character into readline's pending-input queue.
    // The next process_char() call will consume it before reading from stdin.
    // Used to replay a consumed byte (e.g. an ESC that turned out to be part
    // of an escape sequence rather than a standalone interrupt key).
    void stuff_char(int c);


    // Override readline's character-source function.
    void set_getc_function(int (*fn)(FILE *));

    // Replace the FILE* readline reads characters from.
    // Call before install_callback().  Used to interpose a filter pipe so that
    // bytes written to the write end are what readline sees, while the real
    // stdin is read by our own loop (for mouse-event interception, etc.).
    void set_instream(FILE* f);

    // Force readline/libedit to (re)initialize now, picking up the current
    // rl_instream.  Normally libedit lazily initializes on first readline()
    // call — but read_history(), add_history(), and similar can trigger init
    // earlier, capturing whatever rl_instream happened to be at that moment.
    // Call this after set_instream() to pin libedit to the intended FILE*.
    void initialize();

    // Return the current contents of readline's line buffer (rl_line_buffer).
    // Safe to call from inside a getc-function or between process_char() calls.
    std::string current_buffer() const;

    // Return how many terminal rows the current readline prompt + buffer
    // occupies given a terminal of `term_cols` columns.  Returns 1 when there
    // is no active buffer.  Used by the REPL to drive dynamic input-area sizing.
    int current_display_rows(int term_cols) const;

    // Non-blocking readline: install handler, draw prompt, return immediately.
    void install_callback(const std::string& prompt,
                          std::function<void(const std::string&)> on_line);

    // Process one character from stdin (call when stdin is ready).
    void process_char();

    // Remove the installed callback handler.
    void remove_callback();

private:
    std::string history_path_;
    int         max_history_ = 1000;
};

} // namespace index_ai
