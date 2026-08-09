#include "workspace_root.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace arbiter {

std::string canonical_workspace_root(std::string_view root, std::string* err) {
    auto fail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        return std::string();
    };
    std::error_code ec;
    fs::path raw;
    if (root.empty()) {
        raw = fs::current_path(ec);
        if (ec) return fail("cannot determine working directory");
    } else {
        // Migration leftovers from per-cwd session hashes are not real paths.
        if (root.rfind("session:", 0) == 0) {
            return fail("conversation workspace unknown (legacy session placeholder)");
        }
        raw = fs::path(std::string(root));
        if (!fs::exists(raw, ec) || ec) {
            return fail("conversation workspace missing: " + std::string(root));
        }
        if (!fs::is_directory(raw, ec) || ec) {
            return fail("conversation workspace is not a directory: " +
                        std::string(root));
        }
    }
    fs::path canon = fs::canonical(raw, ec);
    if (ec) {
        if (!root.empty()) {
            return fail("conversation workspace unavailable: " + std::string(root) +
                        " (" + ec.message() + ")");
        }
        canon = raw;  // rare: cwd itself failed to canonicalize
    }
    if (!fs::is_directory(canon, ec) || ec) {
        return fail(root.empty()
                        ? "working directory is not a directory"
                        : ("conversation workspace is not a directory: " +
                           std::string(root)));
    }
    if (err) err->clear();
    return canon.string();
}

} // namespace arbiter
