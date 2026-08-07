// arbiter/src/event_routing.cpp

#include "event_routing.h"
#include "constitution.h"

#include <cstdio>
#include <filesystem>
#include <fnmatch.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace arbiter {

std::string route_event(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& agents,
    const std::string& event_type) {
    for (const auto& [id, patterns] : agents) {
        for (const auto& pattern : patterns) {
            if (fnmatch(pattern.c_str(), event_type.c_str(), 0) == 0) {
                return id;
            }
        }
    }
    return {};
}

std::string route_event(const std::string& agents_dir,
                        const std::string& event_type) {
    if (!fs::is_directory(agents_dir)) return "index";
    std::vector<std::pair<std::string, std::vector<std::string>>> agents;
    for (auto& entry : fs::directory_iterator(agents_dir)) {
        if (entry.path().extension() != ".json") continue;
        try {
            auto config = Constitution::from_file(entry.path().string());
            if (config.event_types.empty()) continue;
            std::string id = config.name.empty()
                ? entry.path().stem().string()
                : config.name;
            agents.emplace_back(std::move(id), config.event_types);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "WARN: route_event skip %s: %s\n",
                         entry.path().c_str(), e.what());
        }
    }
    std::string matched = route_event(agents, event_type);
    return matched.empty() ? std::string("index") : matched;
}

} // namespace arbiter
