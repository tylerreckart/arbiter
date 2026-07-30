// arbiter/src/diff/apply.cpp — see include/diff/apply.h

#include "diff/apply.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace arbiter {

namespace {

std::string strip_path_prefix(std::string_view raw) {
    // Drop trailing CR and anything after a tab (git timestamps).
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' '))
        raw.remove_suffix(1);
    const auto tab = raw.find('\t');
    if (tab != std::string_view::npos) raw = raw.substr(0, tab);
    while (!raw.empty() && raw.back() == ' ') raw.remove_suffix(1);
    // Quoted paths: "foo/bar"
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    if (raw == "/dev/null") return {};
    // Keep other absolute paths intact so path_has_traversal rejects them
    // (do not collapse "/etc/passwd" into a relative "etc/passwd").
    if (!raw.empty() && raw.front() == '/') return std::string(raw);
    if (raw.size() >= 2 && raw[1] == '/' &&
        (raw[0] == 'a' || raw[0] == 'b')) {
        raw.remove_prefix(2);
    }
    // Drop leading "./" so a/.//etc/passwd still looks absolute below.
    while (raw.size() >= 2 && raw[0] == '.' && raw[1] == '/')
        raw.remove_prefix(2);
    // a//etc/passwd → "/etc/passwd" after a/ strip — keep absolute.
    if (!raw.empty() && raw.front() == '/') return std::string(raw);
    // Collapse "." segments and empty mid-path parts (LLM/git noise).
    // Still reject ".." via path_has_traversal.
    std::string out;
    std::size_t i = 0;
    while (i < raw.size()) {
        const auto slash = raw.find('/', i);
        const auto part = (slash == std::string_view::npos)
            ? raw.substr(i) : raw.substr(i, slash - i);
        if (!part.empty() && part != ".") {
            if (!out.empty()) out.push_back('/');
            out.append(part);
        }
        if (slash == std::string_view::npos) break;
        i = slash + 1;
    }
    return out;
}

bool path_has_traversal(std::string_view p) {
    if (p.empty()) return true;
    if (!p.empty() && p.front() == '/') return true;
    // Reject Windows drive / UNC styles early.
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) &&
        p[1] == ':')
        return true;
    std::size_t i = 0;
    while (i < p.size()) {
        const auto slash = p.find('/', i);
        const auto part = (slash == std::string_view::npos)
            ? p.substr(i) : p.substr(i, slash - i);
        // "." is normalized away in strip_path_prefix; reject ".." only.
        if (part == "..") return true;
        if (slash == std::string_view::npos) break;
        i = slash + 1;
    }
    return false;
}

int parse_hunk_count(std::string_view part, int fallback_start) {
    // part like "-12,3" or "+12" or "-0,0"
    if (part.empty()) return 0;
    if (part[0] == '-' || part[0] == '+') part.remove_prefix(1);
    const auto comma = part.find(',');
    std::string_view count = (comma == std::string_view::npos)
        ? std::string_view{} : part.substr(comma + 1);
    if (count.empty()) {
        // Unified diff omits count when it is 1, except for empty
        // (start 0).  Callers pass the start so we can decide.
        (void)fallback_start;
        return 1;
    }
    try {
        return std::stoi(std::string(count));
    } catch (...) {
        return -1;
    }
}

int parse_hunk_start(std::string_view part) {
    if (part.empty()) return -1;
    if (part[0] == '-' || part[0] == '+') part.remove_prefix(1);
    const auto comma = part.find(',');
    auto digits = (comma == std::string_view::npos) ? part : part.substr(0, comma);
    try {
        return std::stoi(std::string(digits));
    } catch (...) {
        return -1;
    }
}

std::vector<std::string> split_lines_keep_nl(const std::string& text) {
    // Split into lines WITHOUT trailing '\n'.  An empty file yields no
    // lines.  A file that ends with '\n' has N content lines for N
    // newline-terminated records; a file without a final newline still
    // yields its last partial line.
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            out.emplace_back(text.substr(pos));
            break;
        }
        out.emplace_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& lines, bool trailing_nl) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i];
        if (i + 1 < lines.size() || trailing_nl) oss << '\n';
    }
    return oss.str();
}

bool read_file_bytes(const fs::path& path, std::string& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        err = "cannot read " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    if (in.bad()) {
        err = "read failed: " + path.string();
        return false;
    }
    return true;
}

bool write_file_bytes(const fs::path& path, const std::string& bytes,
                      std::string& err) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            err = "cannot create directories: " + ec.message();
            return false;
        }
    }
    // Write via temp sibling then rename for a rough atomic replace.
    const fs::path tmp = path.string() + ".arbiter-diff.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            err = "cannot open for writing: " + tmp.string();
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            err = "write failed: " + tmp.string();
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Cross-device rename can fail; fall back to copy+remove.
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
        if (ec) {
            err = "cannot replace file: " + ec.message();
            return false;
        }
    }
    return true;
}

bool match_hunk_at(const std::vector<std::string>& file_lines,
                   std::size_t at,
                   const DiffHunk& hunk,
                   std::vector<std::string>& old_expected) {
    old_expected.clear();
    for (const auto& hl : hunk.lines) {
        if (hl.tag == DiffHunkLine::Tag::Add) continue;
        old_expected.push_back(hl.text);
    }
    if (at + old_expected.size() > file_lines.size()) return false;
    for (std::size_t i = 0; i < old_expected.size(); ++i) {
        if (file_lines[at + i] != old_expected[i]) return false;
    }
    return true;
}

std::optional<std::size_t> find_hunk(const std::vector<std::string>& file_lines,
                                     const DiffHunk& hunk) {
    std::vector<std::string> expected;
    for (const auto& hl : hunk.lines) {
        if (hl.tag != DiffHunkLine::Tag::Add) expected.push_back(hl.text);
    }

    if (expected.empty()) {
        // Pure insertion.  Unified-diff / GNU patch rule: with old_count==0,
        // old_start is the line *after which* to insert, so the 0-based
        // index equals old_start (NOT old_start-1).  Empirically:
        //   @@ -2,0 +3,1 @@ on "a\\nb\\nc\\n" inserts between b and c.
        //   @@ -3,0 +4,1 @@ appends after the last line.
        // @@ -0,0 @@ is the new-file / empty-file form only.
        if (hunk.old_count != 0) return std::nullopt;
        if (hunk.old_start == 0) {
            if (!file_lines.empty()) return std::nullopt;
            return 0;
        }
        const std::size_t at = static_cast<std::size_t>(hunk.old_start);
        if (at > file_lines.size()) return std::nullopt;
        return at;
    }

    // Prefer the hunk header offset when it matches.
    if (hunk.old_start > 0) {
        const std::size_t at = static_cast<std::size_t>(hunk.old_start - 1);
        if (match_hunk_at(file_lines, at, hunk, expected)) return at;
    }

    // LLM hunk headers are often off-by-N.  Fall back to a unique context
    // scan: apply only when exactly one match exists so duplicate blocks
    // never silently patch the wrong site.
    std::optional<std::size_t> unique;
    const std::size_t limit =
        (expected.size() > file_lines.size())
            ? 0
            : (file_lines.size() - expected.size() + 1);
    for (std::size_t at = 0; at < limit; ++at) {
        if (!match_hunk_at(file_lines, at, hunk, expected)) continue;
        if (unique) return std::nullopt;  // ambiguous
        unique = at;
    }
    return unique;
}

} // namespace

ParsedUnifiedDiff parse_unified_diff(std::string_view patch) {
    ParsedUnifiedDiff out;
    if (patch.empty()) {
        out.error = "empty patch";
        return out;
    }

    DiffHunk current;
    bool in_hunk = false;
    bool saw_file = false;
    int file_headers = 0;

    auto flush_hunk = [&]() {
        if (in_hunk) {
            out.hunks.push_back(std::move(current));
            current = DiffHunk{};
            in_hunk = false;
        }
    };

    std::size_t pos = 0;
    while (pos <= patch.size()) {
        const std::size_t end = patch.find('\n', pos);
        std::string_view raw = (end == std::string_view::npos)
            ? patch.substr(pos) : patch.substr(pos, end - pos);
        while (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);

        if (raw.rfind("diff --git ", 0) == 0) {
            if (saw_file) {
                out.error = "multi-file patches are not supported yet; "
                            "emit one ```diff fence per file";
                out.hunks.clear();
                return out;
            }
        } else if (raw.rfind("Binary files ", 0) == 0 ||
                   raw.rfind("GIT binary patch", 0) == 0) {
            out.error = "binary patches are not supported";
            out.hunks.clear();
            return out;
        } else if (raw.rfind("rename from ", 0) == 0 ||
                   raw.rfind("rename to ", 0) == 0 ||
                   raw.rfind("copy from ", 0) == 0 ||
                   raw.rfind("copy to ", 0) == 0) {
            out.error = "rename/copy patches are not supported yet";
            out.hunks.clear();
            return out;
        } else if (raw.size() >= 4 && raw.substr(0, 4) == "--- ") {
            flush_hunk();
            ++file_headers;
            if (file_headers > 1) {
                out.error = "multi-file patches are not supported yet; "
                            "emit one ```diff fence per file";
                out.hunks.clear();
                return out;
            }
            out.old_path = strip_path_prefix(raw.substr(4));
            saw_file = true;
        } else if (raw.size() >= 4 && raw.substr(0, 4) == "+++ ") {
            out.new_path = strip_path_prefix(raw.substr(4));
            saw_file = true;
        } else if (raw.size() >= 2 && raw.substr(0, 2) == "@@") {
            flush_hunk();
            // @@ -old[,n] +new[,m] @@ optional junk
            const auto at2 = raw.find("@@", 2);
            std::string_view body = (at2 == std::string_view::npos)
                ? raw.substr(2) : raw.substr(2, at2 - 2);
            while (!body.empty() && body.front() == ' ') body.remove_prefix(1);
            while (!body.empty() && body.back() == ' ') body.remove_suffix(1);
            const auto sp = body.find(' ');
            if (sp == std::string_view::npos) {
                out.error = "malformed hunk header: " + std::string(raw);
                out.hunks.clear();
                return out;
            }
            auto old_part = body.substr(0, sp);
            auto new_part = body.substr(sp + 1);
            const auto sp2 = new_part.find(' ');
            if (sp2 != std::string_view::npos) new_part = new_part.substr(0, sp2);
            current.old_start = parse_hunk_start(old_part);
            current.new_start = parse_hunk_start(new_part);
            current.old_count = parse_hunk_count(old_part, current.old_start);
            current.new_count = parse_hunk_count(new_part, current.new_start);
            if (current.old_start < 0 || current.new_start < 0 ||
                current.old_count < 0 || current.new_count < 0) {
                out.error = "malformed hunk header: " + std::string(raw);
                out.hunks.clear();
                return out;
            }
            // When count omitted and start is 0, unified diff means empty.
            if (old_part.find(',') == std::string_view::npos &&
                current.old_start == 0)
                current.old_count = 0;
            if (new_part.find(',') == std::string_view::npos &&
                current.new_start == 0)
                current.new_count = 0;
            in_hunk = true;
        } else if (in_hunk) {
            if (raw.empty()) {
                // Bare blank lines between hunks / trailing EOF — ignore.
                // Real empty context is emitted as a single space marker.
            } else {
                const char marker = raw[0];
                if (marker == '\\') {
                    // "\ No newline at end of file" — ignore for apply;
                    // trailing newline policy comes from the file itself.
                } else if (marker == '+' || marker == '-' || marker == ' ') {
                    DiffHunkLine hl;
                    hl.tag = (marker == '+') ? DiffHunkLine::Tag::Add
                           : (marker == '-') ? DiffHunkLine::Tag::Remove
                                             : DiffHunkLine::Tag::Context;
                    hl.text = (raw.size() > 1) ? std::string(raw.substr(1))
                                               : std::string{};
                    current.lines.push_back(std::move(hl));
                } else {
                    // Models often omit the leading space on context lines.
                    // Treat unmarked hunk body as context so apply can match.
                    DiffHunkLine hl;
                    hl.tag = DiffHunkLine::Tag::Context;
                    hl.text = std::string(raw);
                    current.lines.push_back(std::move(hl));
                }
            }
        }

        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    flush_hunk();

    out.is_new_file = out.old_path.empty() && !out.new_path.empty();
    out.is_delete   = out.new_path.empty() && !out.old_path.empty();

    if (out.old_path.empty() && out.new_path.empty()) {
        out.error = "patch missing --- / +++ file headers";
        out.hunks.clear();
        return out;
    }
    if (out.hunks.empty()) {
        out.error = "patch has no hunks";
        return out;
    }

    const std::string& target =
        out.is_delete ? out.old_path : out.new_path;
    if (path_has_traversal(target)) {
        out.error = "path escapes workspace or is absolute: " + target;
        out.hunks.clear();
        return out;
    }
    if (!out.old_path.empty() && !out.new_path.empty() &&
        out.old_path != out.new_path) {
        out.error = "rename patches are not supported yet (" +
                    out.old_path + " → " + out.new_path + ")";
        out.hunks.clear();
        return out;
    }
    return out;
}

std::optional<std::string>
resolve_workspace_path(std::string_view rel_path,
                       std::string_view workspace_root,
                       std::string& err) {
    if (path_has_traversal(rel_path)) {
        err = "path escapes workspace or is absolute: " + std::string(rel_path);
        return std::nullopt;
    }
    std::error_code ec;
    fs::path root;
    if (workspace_root.empty()) {
        root = fs::current_path(ec);
        if (ec) {
            err = "cannot determine working directory";
            return std::nullopt;
        }
    } else {
        root = fs::path(std::string(workspace_root));
    }
    fs::path canon_root = fs::canonical(root, ec);
    if (ec) canon_root = fs::absolute(root, ec);
    if (ec) {
        err = "invalid workspace root: " + ec.message();
        return std::nullopt;
    }

    fs::path abs_target = canon_root / std::string(rel_path);
    fs::path existing = abs_target;
    fs::path tail;
    while (!existing.empty()) {
        if (fs::exists(existing, ec)) break;
        // Prefer concat over `filename() / empty` — the latter yields a
        // trailing slash ("new.txt/") which create_directories then
        // materialises as a directory.
        if (tail.empty()) tail = existing.filename();
        else              tail = existing.filename() / tail;
        if (!existing.has_parent_path() || existing.parent_path() == existing) {
            existing.clear();
            break;
        }
        existing = existing.parent_path();
    }
    fs::path resolved;
    if (!existing.empty()) {
        fs::path canon = fs::canonical(existing, ec);
        if (ec) {
            err = "invalid path: " + ec.message();
            return std::nullopt;
        }
        resolved = tail.empty() ? canon : (canon / tail);
    } else {
        resolved = fs::weakly_canonical(abs_target, ec);
        if (ec) {
            err = "invalid path: " + ec.message();
            return std::nullopt;
        }
    }
    resolved = resolved.lexically_normal();
    // Drop a trailing slash so "file/" cannot be mistaken for a directory
    // target on create.
    {
        auto s = resolved.string();
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        resolved = s;
    }
    const auto resolved_str = resolved.string();
    const auto root_str = canon_root.string();
    if (resolved_str.size() < root_str.size() ||
        resolved_str.compare(0, root_str.size(), root_str) != 0 ||
        (resolved_str.size() > root_str.size() &&
         resolved_str[root_str.size()] != '/')) {
        err = "path escapes project directory";
        return std::nullopt;
    }
    return resolved.string();
}

DiffApplyResult apply_unified_diff(std::string_view patch,
                                   std::string_view workspace_root) {
    DiffApplyResult result;
    auto parsed = parse_unified_diff(patch);
    if (!parsed.error.empty()) {
        result.error = parsed.error;
        return result;
    }

    const std::string rel =
        parsed.is_delete ? parsed.old_path : parsed.new_path;
    result.path = rel;

    std::string err;
    auto resolved = resolve_workspace_path(rel, workspace_root, err);
    if (!resolved) {
        result.error = err;
        return result;
    }
    result.resolved_path = *resolved;
    const fs::path path(*resolved);

    std::error_code ec;
    const bool exists = fs::exists(path, ec) && fs::is_regular_file(path, ec);
    if (exists && fs::is_directory(path, ec)) {
        result.error = "target is a directory: " + rel;
        return result;
    }

    std::string pre;
    if (exists) {
        if (!read_file_bytes(path, pre, err)) {
            result.error = err;
            return result;
        }
        result.had_file = true;
        result.pre_image = pre;
    } else if (parsed.is_delete) {
        result.error = "cannot delete missing file: " + rel;
        return result;
    } else {
        // File does not exist: create it from the patch's new side.
        // `/diff apply` is the user's grant — no write confirm, and we
        // do not require a /dev/null new-file header.
        //
        // Multi-hunk *edit* patches omit unchanged lines between hunks, so
        // concatenating new-side lines would drop those gaps.  Only create
        // from a single hunk, or from an explicit new-file patch (where
        // every hunk is a pure insert against /dev/null).
        if (parsed.hunks.size() > 1 && !parsed.is_new_file) {
            result.error =
                "cannot create missing file from multi-hunk edit patch: " + rel +
                " (gaps between hunks are unknown; use a single hunk or "
                "--- /dev/null new-file patch)";
            return result;
        }
        result.had_file = false;
        result.pre_image.clear();

        std::vector<DiffHunk> hunks = parsed.hunks;
        std::stable_sort(hunks.begin(), hunks.end(),
                         [](const DiffHunk& a, const DiffHunk& b) {
                             return a.new_start < b.new_start;
                         });
        std::vector<std::string> new_lines;
        for (const auto& hunk : hunks) {
            for (const auto& hl : hunk.lines) {
                if (hl.tag == DiffHunkLine::Tag::Remove) continue;
                new_lines.push_back(hl.text);
            }
        }
        const std::string post = join_lines(new_lines, /*trailing_nl=*/true);
        if (!write_file_bytes(path, post, err)) {
            result.error = err;
            return result;
        }
        result.post_image = post;
        result.ok = true;
        return result;
    }

    if (parsed.is_delete) {
        // Verify delete hunks match current content when present.
        auto lines = split_lines_keep_nl(pre);
        for (const auto& hunk : parsed.hunks) {
            auto at = find_hunk(lines, hunk);
            if (!at) {
                result.error = "stale patch: hunk context does not match " + rel;
                return result;
            }
        }
        fs::remove(path, ec);
        if (ec) {
            result.error = "cannot delete: " + ec.message();
            return result;
        }
        result.post_image.clear();
        result.ok = true;
        return result;
    }

    auto lines = split_lines_keep_nl(pre);
    // Preserve the pre-image EOF convention.  New files (empty pre) default
    // to a trailing newline — common text-file norm — without rewriting
    // existing no-final-newline files on edit.
    const bool trailing_nl = pre.empty() ? true : (pre.back() == '\n');

    // Apply hunks from bottom to top so earlier offsets stay valid.
    std::vector<DiffHunk> hunks = parsed.hunks;
    std::stable_sort(hunks.begin(), hunks.end(),
                     [](const DiffHunk& a, const DiffHunk& b) {
                         return a.old_start > b.old_start;
                     });

    for (const auto& hunk : hunks) {
        auto at = find_hunk(lines, hunk);
        if (!at) {
            // Distinguish "not found" from "ambiguous duplicate context".
            std::vector<std::string> expected;
            for (const auto& hl : hunk.lines) {
                if (hl.tag != DiffHunkLine::Tag::Add)
                    expected.push_back(hl.text);
            }
            int matches = 0;
            if (!expected.empty() && expected.size() <= lines.size()) {
                const std::size_t limit =
                    lines.size() - expected.size() + 1;
                for (std::size_t i = 0; i < limit; ++i) {
                    std::vector<std::string> tmp;
                    if (match_hunk_at(lines, i, hunk, tmp)) ++matches;
                }
            }
            if (matches > 1) {
                result.error =
                    "ambiguous patch: hunk context matches " +
                    std::to_string(matches) + " sites in " + rel +
                    " (regenerate with more unique context)";
            } else {
                result.error =
                    "stale patch: hunk context does not match " + rel +
                    " (re-read the file and emit a fresh ```diff)";
            }
            return result;
        }
        std::vector<std::string> old_expected;
        match_hunk_at(lines, *at, hunk, old_expected);
        std::vector<std::string> replacement;
        for (const auto& hl : hunk.lines) {
            if (hl.tag == DiffHunkLine::Tag::Remove) continue;
            replacement.push_back(hl.text);
        }
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(*at),
                    lines.begin() + static_cast<std::ptrdiff_t>(*at + old_expected.size()));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*at),
                     replacement.begin(), replacement.end());
    }

    const std::string post = join_lines(lines, trailing_nl && !lines.empty());
    if (!write_file_bytes(path, post, err)) {
        result.error = err;
        return result;
    }
    result.post_image = post;
    result.ok = true;
    return result;
}

DiffApplyResult undo_unified_diff(const DiffUndoSnapshot& snap) {
    DiffApplyResult result;
    result.resolved_path = snap.resolved_path;
    if (snap.resolved_path.empty()) {
        result.error = "empty undo snapshot";
        return result;
    }
    const fs::path path(snap.resolved_path);
    std::error_code ec;

    // Derive relative display path from filename.
    result.path = path.filename().string();

    const bool exists = fs::exists(path, ec) && fs::is_regular_file(path, ec);
    std::string current;
    if (exists) {
        std::string err;
        if (!read_file_bytes(path, current, err)) {
            result.error = err;
            return result;
        }
    }

    if (current != snap.post_image) {
        result.error =
            "file changed since apply; refuse to undo (resolve manually)";
        return result;
    }

    if (!snap.had_file) {
        // Apply created the file — delete it.
        if (exists) {
            fs::remove(path, ec);
            if (ec) {
                result.error = "cannot remove created file: " + ec.message();
                return result;
            }
        }
        result.ok = true;
        result.post_image = snap.pre_image;
        return result;
    }

    // Restore pre-image.
    std::string err;
    if (!write_file_bytes(path, snap.pre_image, err)) {
        result.error = err;
        return result;
    }
    result.had_file = true;
    result.pre_image = snap.post_image;
    result.post_image = snap.pre_image;
    result.ok = true;
    return result;
}

} // namespace arbiter
