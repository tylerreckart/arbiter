#pragma once
// index/include/cli_helpers.h — Shared helpers for CLI entry points and the REPL.
//
// Covers:
//   • BANNER — ASCII startup banner
//   • agent_color — stable per-agent ANSI color
//   • Config path helpers (~/.index for config, $PWD/.index/memory for memory,
//     API key resolution)
//   • Thin wrappers around index_ai::cmd_mem_* and cmd_fetch so the REPL
//     doesn't have to thread memory_dir through every call site
//   • term_cols / term_rows — terminal dimensions via TIOCGWINSZ

#include <string>

namespace index_ai {

extern const char* BANNER;

// Stable ANSI color for a given agent id (hash-mapped into a fixed palette).
// "index" always maps to orange.
std::string agent_color(const std::string& agent_id);

// ~/.index (created if missing).
std::string get_config_dir();

// $PWD/.index/memory — cwd-scoped so context never bleeds between projects.
// Not auto-created on resolve; writers create lazily when notes are first saved.
std::string get_memory_dir();

// ANTHROPIC_API_KEY env var, else ~/.index/api_key.
// Prints to stderr and exits(1) if neither is set.
std::string get_api_key();

// Thin wrappers: commands.cpp's cmd_mem_*/cmd_fetch but with memory_dir
// supplied automatically.
std::string write_memory(const std::string& agent_id, const std::string& text);
std::string read_memory (const std::string& agent_id);
std::string fetch_url   (const std::string& url);

// Terminal dimensions (via ioctl on stdout).  Default to 80x24 on failure.
int term_cols();
int term_rows();

} // namespace index_ai
