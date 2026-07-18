#include "tui/palette.h"

#include <algorithm>
#include <cctype>

namespace arbiter {

namespace {

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool subsequence(const std::string& needle, const std::string& hay) {
    size_t i = 0;
    for (char c : hay) {
        if (i < needle.size() && needle[i] == c) ++i;
    }
    return i == needle.size();
}

// Lower = better; -1 = no match.
int tier_for(const PaletteItem& item, const std::string& query_lc) {
    const std::string name_lc = lowered(item.name);
    // Commands are typed without the leading slash as often as with it.
    const std::string bare = !name_lc.empty() && name_lc[0] == '/'
        ? name_lc.substr(1) : name_lc;
    if (name_lc.compare(0, query_lc.size(), query_lc) == 0 ||
        bare.compare(0, query_lc.size(), query_lc) == 0) {
        return 0;
    }
    if (name_lc.find(query_lc) != std::string::npos) return 1;
    if (lowered(item.description).find(query_lc) != std::string::npos) return 2;
    if (subsequence(query_lc, name_lc)) return 3;
    return -1;
}

} // namespace

std::vector<PaletteItem>
palette_filter(const std::vector<PaletteItem>& items, const std::string& query) {
    if (query.empty()) return items;
    const std::string query_lc = lowered(query);

    struct Ranked {
        int tier;
        size_t pos;
        const PaletteItem* item;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const int t = tier_for(items[i], query_lc);
        if (t >= 0) ranked.push_back({t, i, &items[i]});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        return a.pos < b.pos;
    });

    std::vector<PaletteItem> out;
    out.reserve(ranked.size());
    for (const auto& r : ranked) out.push_back(*r.item);
    return out;
}

} // namespace arbiter
