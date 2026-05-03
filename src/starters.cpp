// index/src/starters.cpp — see starters.h
//
// The full Constitution for each starter lives in agents/<id>.json — that
// directory is the single source of truth for example agents.  At build
// time, cmake/embed_starters.cmake reads every JSON file and emits the
// generated header below; we parse those embedded JSON strings via
// Constitution::from_json to build the starter registry at runtime.
//
// Blurbs (one-line descriptions shown in init/wizard menus) aren't part of
// the Constitution schema — they're metadata about the agent's purpose,
// not behaviour — so they stay in a small id→blurb table in this file.

#include "starters.h"
#include "starters_data.h"  // generated header (build/generated/)

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace index_ai {

namespace {

// Display order for the wizard / --init.  Matches what the C++ table
// historically presented; agents not in this list (someone dropping a new
// agents/foo.json) get appended at the end in lexicographic order so they
// still appear without code changes.
struct StarterMeta {
    std::string_view id;
    std::string_view blurb;
};

constexpr StarterMeta kCuratedOrder[] = {
    { "reviewer", "code review — terse, defect-focused"            },
    { "research", "research analyst — haiku + opus advisor combo"  },
    { "writer",   "essays, READMEs, docs, creative writing"        },
    { "devops",   "infrastructure engineer — shell, git, CI/CD"    },
    { "planner",  "task decomposition into phased execution plans" },
    { "backend",  "APIs, data modeling, distributed systems"       },
    { "frontend", "components, state, accessibility, performance"  },
    { "marketer", "strategy, positioning, campaign concepts"       },
    { "social",   "platform-native content, growth, engagement"    },
};

// A starter that's in the embedded JSON table but not in kCuratedOrder
// gets a fallback blurb derived from its `goal` field, truncated to fit a
// menu line.  Keeps new JSON files (added by users or future commits)
// usable without a starters.cpp change, which is the whole point of the
// move-to-JSON refactor.
std::string fallback_blurb(const Constitution& c) {
    std::string s = c.goal;
    if (s.empty()) s = c.role;
    if (s.empty()) s = c.name;
    constexpr size_t kMax = 60;
    if (s.size() > kMax) {
        s.resize(kMax - 1);
        s += "…";
    }
    return s;
}

}  // namespace

std::vector<StarterAgent> starter_agents() {
    const auto& table = starter_json_table();
    std::vector<StarterAgent> out;
    out.reserve(table.size());

    // Curated entries first, in declared order, so the menu stays stable
    // for users who've memorised "research is option 2".
    for (const auto& meta : kCuratedOrder) {
        std::string id(meta.id);
        auto it = table.find(id);
        if (it == table.end()) continue;  // JSON file removed — skip silently
        try {
            Constitution cfg = Constitution::from_json(it->second);
            out.push_back({ id, std::string(meta.blurb), std::move(cfg) });
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "WARN: starter '%s' embedded JSON failed to parse: %s\n",
                id.c_str(), e.what());
        }
    }

    // Anything in the JSON table not yet in the curated list — append in
    // lexicographic order so the resulting list is deterministic.
    std::vector<std::string> uncurated;
    for (const auto& [id, _] : table) {
        bool found = false;
        for (const auto& meta : kCuratedOrder) {
            if (meta.id == id) { found = true; break; }
        }
        if (!found) uncurated.push_back(id);
    }
    std::sort(uncurated.begin(), uncurated.end());
    for (const auto& id : uncurated) {
        auto it = table.find(id);
        if (it == table.end()) continue;
        try {
            Constitution cfg = Constitution::from_json(it->second);
            std::string blurb = fallback_blurb(cfg);
            out.push_back({ id, std::move(blurb), std::move(cfg) });
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "WARN: starter '%s' embedded JSON failed to parse: %s\n",
                id.c_str(), e.what());
        }
    }

    return out;
}

std::string starter_json(const std::string& id) {
    const auto& table = starter_json_table();
    auto it = table.find(id);
    if (it == table.end()) return {};
    return std::string(it->second);
}

}  // namespace index_ai
