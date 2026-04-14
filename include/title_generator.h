#pragma once
// index/include/title_generator.h
//
// Session-title generation and the turn-rule separator helper — both pulled
// out of main.cpp so the REPL doesn't have to host every one-off formatting
// function.
//
// print_turn_rule: prints a colored ─── label ─────────── right ─ ruler
// straight to stdout.  Used between streamed agent responses.
//
// generate_title_async: fires a detached thread that asks Claude Haiku to
// write a 5-7 word title for the session, then delivers the result via the
// on_title callback.  The callback is invoked on the worker thread, so the
// implementer must handle synchronization (TUI::set_title is mutex-protected
// internally; arbitrary sinks need their own locking).

#include "api_client.h"

#include <functional>
#include <string>

namespace index_ai {

void print_turn_rule(const std::string& label,
                     const std::string& color,
                     const std::string& right_label,
                     int cols);

void generate_title_async(ApiClient& client,
                          const std::string& user_msg,
                          const std::string& assistant_snippet,
                          std::function<void(const std::string&)> on_title);

} // namespace index_ai
