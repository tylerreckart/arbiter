#pragma once
// arbiter/include/title_generator.h
//
// Turn-rule separator helper — pulled out of main.cpp so the REPL doesn't
// have to host every one-off formatting function.
//
// print_turn_rule: prints a colored ─── label ─────────── right ─ ruler
// straight to stdout.  Used between streamed agent responses.

#include <string>

namespace arbiter {

void print_turn_rule(const std::string& label,
                     const std::string& color,
                     const std::string& right_label,
                     int cols);

} // namespace arbiter
