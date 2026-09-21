// arbiter/src/event_routing.cpp

#include "event_routing.h"
#include "constitution.h"
#include "json.h"

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <fnmatch.h>
#include <sstream>
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
    if (!fs::is_directory(agents_dir)) return {};
    std::vector<std::pair<std::string, std::vector<std::string>>> agents;
    for (auto& entry : fs::directory_iterator(agents_dir)) {
        if (entry.path().extension() != ".json") continue;
        try {
            auto config = Constitution::from_file(entry.path().string());
            if (config.event_types.empty()) continue;
            std::string id = file_backed_agent_id(
                config, entry.path().stem().string());
            agents.emplace_back(std::move(id), config.event_types);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "WARN: route_event skip %s: %s\n",
                         entry.path().c_str(), e.what());
        }
    }
    std::string matched = route_event(agents, event_type);
    return matched;
}

std::string file_backed_agent_def_json(const std::string& agents_dir,
                                       const std::string& agent_id) {
    if (agent_id.empty() || !fs::is_directory(agents_dir)) return {};
    for (auto& entry : fs::directory_iterator(agents_dir)) {
        if (entry.path().extension() != ".json") continue;
        try {
            auto config = Constitution::from_file(entry.path().string());
            if (file_backed_agent_id(config, entry.path().stem().string())
                != agent_id) {
                continue;
            }
            std::ifstream in(entry.path());
            if (!in.is_open()) return {};
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "WARN: file_backed_agent_def skip %s: %s\n",
                         entry.path().c_str(), e.what());
        }
    }
    return {};
}

static std::string stamp_agent_def_id(const std::string& agent_def_json,
                                      const std::string& agent_id) {
    if (agent_def_json.empty() || agent_id.empty()) return {};
    try {
        auto def = json_parse(agent_def_json);
        if (!def || !def->is_object()) return {};
        def->as_object_mut()["id"] = jstr(agent_id);
        return json_serialize(*def);
    } catch (...) {
        return {};
    }
}

std::string event_ingest_agent_def_json(
    const std::string& agent_id,
    const std::string& agents_dir,
    const std::string& tenant_def_json,
    bool routed_from_file) {
    if (agent_id.empty() || agent_id == "index") return {};
    std::string blob;
    if (routed_from_file) {
        blob = file_backed_agent_def_json(agents_dir, agent_id);
    } else if (!tenant_def_json.empty()) {
        blob = tenant_def_json;
    } else {
        blob = file_backed_agent_def_json(agents_dir, agent_id);
    }
    return stamp_agent_def_id(blob, agent_id);
}

} // namespace arbiter
