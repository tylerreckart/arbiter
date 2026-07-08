#pragma once
// Fuzzy filter for the Ctrl-P command palette.  Pure ranking logic, kept
// free of OpenTUI so it unit-tests without a terminal.

#include <string>
#include <vector>

namespace arbiter {

struct PaletteItem {
    std::string name;          // what Enter inserts, e.g. "/chat search"
    std::string description;   // one-line hint shown next to the name
};

// Rank `items` against `query`, best match first.  Case-insensitive, in
// descending match quality: name prefix, name substring, description
// substring, then name subsequence (characters in order with gaps).
// Ties keep the input order.  An empty query returns everything as-is.
[[nodiscard]] std::vector<PaletteItem>
palette_filter(const std::vector<PaletteItem>& items, const std::string& query);

} // namespace arbiter
