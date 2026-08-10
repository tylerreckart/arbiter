#include "workspace_map.h"
#include "workspace_root.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace arbiter {
namespace {

bool is_ignored_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return true;
    // Hidden entries except a few that agents commonly need.
    if (name.front() == '.') {
        return !(name == ".github" || name == ".gitlab" ||
                 name == ".circleci" || name == ".vscode" ||
                 name == ".cursor");
    }
    static constexpr const char* kDirs[] = {
        "node_modules", "build", "dist", "out", "target",
        "__pycache__", "venv", ".venv", "Pods", "DerivedData",
        "coverage", ".next", ".turbo", ".gradle", "vendor",
        "CMakeFiles", ".cache",
    };
    for (const char* d : kDirs) {
        if (name == d) return true;
    }
    return false;
}

bool path_has_traversal(std::string_view p) {
    if (p.empty()) return false;
    if (p.front() == '/' || p.front() == '\\') return true;
    // Reject Windows drive letters and absolute-ish forms.
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) &&
        p[1] == ':') {
        return true;
    }
    std::string cur;
    for (char c : p) {
        if (c == '/' || c == '\\') {
            if (cur == "..") return true;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return cur == "..";
}

struct Entry {
    std::string name;
    bool is_dir = false;
};

std::vector<Entry> list_children(const fs::path& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto name = it->path().filename().string();
        if (is_ignored_name(name)) continue;
        std::error_code sec;
        const bool is_dir = it->is_directory(sec);
        if (sec) continue;
        // Skip non-regular files (sockets, fifos, etc.); keep dirs + files.
        if (!is_dir && !it->is_regular_file(sec)) continue;
        out.push_back(Entry{name, is_dir});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir && !b.is_dir;
        return a.name < b.name;
    });
    return out;
}

struct WalkState {
    int entries = 0;
    std::size_t bytes = 0;
    bool truncated_entries = false;
    bool truncated_bytes = false;
    bool truncated_depth = false;
};

bool append_line(std::ostringstream& out, WalkState& st,
                 const std::string& line, const WorkspaceMapOptions& opts) {
    const std::string with_nl = line + "\n";
    if (opts.max_bytes > 0 &&
        st.bytes + with_nl.size() > opts.max_bytes) {
        st.truncated_bytes = true;
        return false;
    }
    if (opts.max_entries > 0 && st.entries >= opts.max_entries) {
        st.truncated_entries = true;
        return false;
    }
    out << with_nl;
    st.bytes += with_nl.size();
    ++st.entries;
    return true;
}

bool under_workspace_root(const std::string& root_str, const fs::path& path,
                          fs::path* canon_out = nullptr) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(path, ec);
    if (ec) return false;
    canon = canon.lexically_normal();
    const auto cand = canon.string();
    if (!path_within_canonical_root(root_str, cand)) return false;
    if (canon_out) *canon_out = std::move(canon);
    return true;
}

bool walk(const fs::path& dir, const std::string& root_str, int depth,
          const std::string& prefix, std::ostringstream& out, WalkState& st,
          const WorkspaceMapOptions& opts) {
    if (opts.max_depth >= 0 && depth > opts.max_depth) {
        st.truncated_depth = true;
        return true;  // skip this subtree, continue siblings
    }
    auto children = list_children(dir);
    for (size_t i = 0; i < children.size(); ++i) {
        const auto& child = children[i];
        const bool last = (i + 1 == children.size());
        const std::string branch = last ? "└── " : "├── ";
        const std::string label =
            child.is_dir ? (child.name + "/") : child.name;
        if (!append_line(out, st, prefix + branch + label, opts)) {
            return false;
        }
        if (child.is_dir) {
            if (opts.max_depth >= 0 && depth >= opts.max_depth) {
                st.truncated_depth = true;
                continue;
            }
            fs::path canon_child;
            if (!under_workspace_root(root_str, dir / child.name, &canon_child)) {
                continue;
            }
            std::error_code ec;
            if (!fs::is_directory(canon_child, ec) || ec) continue;
            const std::string child_prefix =
                prefix + (last ? "    " : "│   ");
            if (!walk(canon_child, root_str, depth + 1, child_prefix, out, st,
                      opts)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

std::string cmd_map(std::string_view workspace_root,
                    std::string_view rel_path,
                    const WorkspaceMapOptions& opts) {
    std::string root_err;
    const std::string root_str =
        canonical_workspace_root(workspace_root, &root_err);
    if (root_str.empty()) {
        return "ERR: " + (root_err.empty() ? "conversation workspace unavailable"
                                           : root_err);
    }

    fs::path map_root(root_str);
    std::string display = ".";
    if (!rel_path.empty()) {
        while (!rel_path.empty() &&
               (rel_path.front() == '/' || rel_path.front() == '\\' ||
                rel_path.front() == ' ' || rel_path.front() == '\t')) {
            rel_path.remove_prefix(1);
        }
        while (!rel_path.empty() &&
               (rel_path.back() == ' ' || rel_path.back() == '\t' ||
                rel_path.back() == '/' || rel_path.back() == '\\')) {
            rel_path.remove_suffix(1);
        }
    }
    if (!rel_path.empty()) {
        if (path_has_traversal(rel_path)) {
            return "ERR: path escapes project directory";
        }
        std::error_code ec;
        fs::path candidate = fs::weakly_canonical(map_root / std::string(rel_path), ec);
        if (ec) {
            return "ERR: invalid path: " + ec.message();
        }
        candidate = candidate.lexically_normal();
        const auto cand = candidate.string();
        if (!path_within_canonical_root(root_str, cand)) {
            return "ERR: path escapes project directory";
        }
        if (!fs::exists(candidate, ec) || ec) {
            return "ERR: path not found: " + std::string(rel_path);
        }
        if (!fs::is_directory(candidate, ec) || ec) {
            return "ERR: not a directory: " + std::string(rel_path);
        }
        map_root = candidate;
        display = std::string(rel_path);
    }

    std::ostringstream out;
    out << "workspace: " << root_str << "\n";
    if (display != ".") out << "path: " << display << "\n";
    out << display << "/\n";

    WalkState st;
    // Count the root label toward the budget loosely via bytes already written.
    st.bytes = out.str().size();
    walk(map_root, root_str, /*depth=*/1, /*prefix=*/"", out, st, opts);

    if (st.truncated_entries) {
        out << "... [truncated — entry cap reached]\n";
    } else if (st.truncated_bytes) {
        if (opts.max_bytes >= 1024) {
            out << "... [truncated at " << (opts.max_bytes / 1024) << " KB]\n";
        } else {
            out << "... [truncated at " << opts.max_bytes << " bytes]\n";
        }
    } else if (st.truncated_depth) {
        out << "... [truncated — depth cap reached; /map <subdir> to go deeper]\n";
    }
    if (st.entries == 0 && !st.truncated_entries && !st.truncated_bytes) {
        out << "(empty)\n";
    }
    return out.str();
}

} // namespace arbiter
