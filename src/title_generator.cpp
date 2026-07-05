// arbiter/src/title_generator.cpp — see title_generator.h

#include "title_generator.h"
#include "theme.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace arbiter {

void print_turn_rule(const std::string& label,
                     const std::string& color,
                     const std::string& right_label,
                     int cols) {
    const std::string& dim = theme().text_dimmer;
    const std::string& rst = theme().reset;

    int prefix = 4;  // "─── "
    int suffix = right_label.empty() ? 0 : (int)right_label.size() + 2;
    int label_w = (int)label.size() + 2;
    int fill = std::max(0, cols - prefix - label_w - suffix);

    std::string line;
    line += dim;
    line += "───";
    line += color;
    line += " ";
    line += label;
    line += " ";
    line += dim;
    for (int i = 0; i < fill; ++i) line += "─";
    if (!right_label.empty()) {
        line += theme().prompt_color;
        line += " ";
        line += right_label;
        line += dim;
        line += " ─";
    }
    line += rst;
    line += "\n";
    std::cout << line;
}

} // namespace arbiter
