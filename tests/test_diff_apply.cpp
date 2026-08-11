// tests/test_diff_apply.cpp — Unified-diff parse / apply / undo.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "diff/apply.h"
#include "repl/diff_proposals.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace arbiter;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        const auto pid = static_cast<long long>(::getpid());
        const auto now = std::chrono::steady_clock::now()
                              .time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_diff_" + std::to_string(pid) + "_" +
                std::to_string(now));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& p, const std::string& body) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << body;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

TEST_CASE("parse_unified_diff: simple edit") {
    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto p = parse_unified_diff(patch);
    CHECK(p.error.empty());
    CHECK(p.old_path == "foo.txt");
    CHECK(p.new_path == "foo.txt");
    CHECK_FALSE(p.is_new_file);
    REQUIRE(p.hunks.size() == 1);
    CHECK(p.hunks[0].old_start == 1);
    CHECK(p.hunks[0].lines.size() == 4);
}

TEST_CASE("parse_unified_diff: rejects absolute / traversal / multi-file") {
    CHECK_FALSE(parse_unified_diff(
        "--- a/../etc/passwd\n+++ b/../etc/passwd\n@@ -1 +1 @@\n-x\n+y\n")
                    .error.empty());
    CHECK_FALSE(parse_unified_diff(
        "--- a/foo\n+++ b/foo\n@@ -1 +1 @@\n-x\n+y\n"
        "--- a/bar\n+++ b/bar\n@@ -1 +1 @@\n-a\n+b\n")
                    .error.empty());
    CHECK_FALSE(parse_unified_diff(
        "--- /etc/passwd\n+++ /etc/passwd\n@@ -1 +1 @@\n-x\n+y\n")
                    .error.empty());
    // a// and a/.// must not collapse into a relative cwd write.
    CHECK_FALSE(parse_unified_diff(
        "--- a//etc/passwd\n+++ b//etc/passwd\n@@ -1 +1 @@\n-x\n+y\n")
                    .error.empty());
    CHECK_FALSE(parse_unified_diff(
        "--- a/.//etc/passwd\n+++ b/.//etc/passwd\n@@ -1 +1 @@\n-x\n+y\n")
                    .error.empty());
    // Backslash-separated .. must not bypass traversal checks on Windows.
    CHECK_FALSE(parse_unified_diff(
        "--- a/foo\\..\\etc\\passwd\n+++ b/foo\\..\\etc\\passwd\n"
        "@@ -1 +1 @@\n-x\n+y\n")
                    .error.empty());
    CHECK_FALSE(parse_unified_diff(
        "--- \\etc\\passwd\n+++ \\etc\\passwd\n@@ -1 +1 @@\n-x\n+y\n")
                    .error.empty());
}

TEST_CASE("parse_unified_diff: collapses ./ path segments") {
    auto p = parse_unified_diff(
        "--- a/./src/foo.cpp\n"
        "+++ b/./src/foo.cpp\n"
        "@@ -1 +1 @@\n"
        "-x\n"
        "+y\n");
    CHECK(p.error.empty());
    CHECK(p.old_path == "src/foo.cpp");
    CHECK(p.new_path == "src/foo.cpp");
}

TEST_CASE("apply_unified_diff: edit + undo") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "a\nb\nc\n");
    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    REQUIRE(r.ok);
    CHECK(r.path == "foo.txt");
    CHECK(read_text(dir.path / "foo.txt") == "a\nB\nc\n");
    CHECK(r.had_file);
    CHECK(r.pre_image == "a\nb\nc\n");

    DiffUndoSnapshot snap;
    snap.resolved_path = r.resolved_path;
    snap.had_file = r.had_file;
    snap.pre_image = r.pre_image;
    snap.post_image = r.post_image;
    auto u = undo_unified_diff(snap);
    REQUIRE(u.ok);
    CHECK(read_text(dir.path / "foo.txt") == "a\nb\nc\n");
}

TEST_CASE("apply_unified_diff: create new file + undo deletes it") {
    TempDir dir;
    const char* patch =
        "--- /dev/null\n"
        "+++ b/new.txt\n"
        "@@ -0,0 +1,2 @@\n"
        "+hello\n"
        "+world\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    REQUIRE(r.ok);
    CHECK(fs::exists(dir.path / "new.txt"));
    CHECK(read_text(dir.path / "new.txt") == "hello\nworld\n");
    CHECK_FALSE(r.had_file);

    DiffUndoSnapshot snap{r.resolved_path, r.had_file, r.pre_image, r.post_image};
    auto u = undo_unified_diff(snap);
    REQUIRE(u.ok);
    CHECK_FALSE(fs::exists(dir.path / "new.txt"));
}

TEST_CASE("apply_unified_diff: missing file created from edit-style hunks") {
    // Agent emitted an edit hunk but the file is absent — create from new side.
    TempDir dir;
    const char* patch =
        "--- a/subdir/missing.txt\n"
        "+++ b/subdir/missing.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " keep\n"
        "-old\n"
        "+new\n"
        " tail\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    REQUIRE(r.ok);
    CHECK(r.path == "subdir/missing.txt");
    CHECK_FALSE(r.had_file);
    CHECK(read_text(dir.path / "subdir" / "missing.txt") == "keep\nnew\ntail\n");

    DiffUndoSnapshot snap{r.resolved_path, r.had_file, r.pre_image, r.post_image};
    auto u = undo_unified_diff(snap);
    REQUIRE(u.ok);
    CHECK_FALSE(fs::exists(dir.path / "subdir" / "missing.txt"));
}

TEST_CASE("apply_unified_diff: delete refuses missing file") {
    TempDir dir;
    const char* patch =
        "--- a/gone.txt\n"
        "+++ /dev/null\n"
        "@@ -1 +0,0 @@\n"
        "-x\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("missing") != std::string::npos);
}

TEST_CASE("apply_unified_diff: multi-hunk edit refuses create-when-missing") {
    TempDir dir;
    const char* patch =
        "--- a/multi.txt\n"
        "+++ b/multi.txt\n"
        "@@ -1,2 +1,2 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        "@@ -10,2 +10,2 @@\n"
        " i\n"
        "-j\n"
        "+J\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("multi-hunk") != std::string::npos);
    CHECK_FALSE(fs::exists(dir.path / "multi.txt"));
}

TEST_CASE("apply_unified_diff: stale context fails") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "a\nCHANGED\nc\n");
    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("stale") != std::string::npos);
    CHECK(read_text(dir.path / "foo.txt") == "a\nCHANGED\nc\n");
}

TEST_CASE("undo refuses when file changed after apply") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "a\nb\nc\n");
    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    REQUIRE(r.ok);
    write_text(dir.path / "foo.txt", "tampered\n");
    DiffUndoSnapshot snap{r.resolved_path, r.had_file, r.pre_image, r.post_image};
    auto u = undo_unified_diff(snap);
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("changed since apply") != std::string::npos);
}

TEST_CASE("apply_unified_diff: unique context fallback when offset is stale") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "x\na\nb\nc\n");
    // Header claims line 9, but unique context is at lines 2-4.
    const char* stale_header =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -9,3 +9,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(stale_header, dir.path.string());
    REQUIRE(r.ok);
    CHECK(read_text(dir.path / "foo.txt") == "x\na\nB\nc\n");
}

TEST_CASE("apply_unified_diff: ambiguous duplicate context refuses apply") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "a\nb\nc\nx\na\nb\nc\n");
    // Wrong header + context that matches twice → refuse (not silent pick).
    const char* ambiguous =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -99,3 +99,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(ambiguous, dir.path.string());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("ambiguous") != std::string::npos);
    CHECK(read_text(dir.path / "foo.txt") == "a\nb\nc\nx\na\nb\nc\n");
}

TEST_CASE("apply_unified_diff: exact header still preferred when it matches") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "x\na\nb\nc\na\nb\nc\n");
    const char* first_block =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -2,3 +2,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto ok_first = apply_unified_diff(first_block, dir.path.string());
    REQUIRE(ok_first.ok);
    CHECK(read_text(dir.path / "foo.txt") == "x\na\nB\nc\na\nb\nc\n");
}

TEST_CASE("apply_unified_diff: accepts a/./path and unmarked context lines") {
    TempDir dir;
    fs::create_directories(dir.path / "sub");
    write_text(dir.path / "sub" / "foo.txt", "a\nb\nc\n");
    const char* patch =
        "--- a/./sub/foo.txt\n"
        "+++ b/./sub/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        "a\n"   // missing leading space (LLM footgun)
        "-b\n"
        "+B\n"
        "c\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    REQUIRE(r.ok);
    CHECK(read_text(dir.path / "sub" / "foo.txt") == "a\nB\nc\n");
}

TEST_CASE("apply_unified_diff: truly stale context still fails") {
    TempDir dir;
    write_text(dir.path / "bar.txt", "a\nb\nc\nx\na\nb\nc\n");
    const char* bad_offset =
        "--- a/bar.txt\n"
        "+++ b/bar.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " x\n"
        "-y\n"
        "+Y\n"
        " z\n";
    auto stale = apply_unified_diff(bad_offset, dir.path.string());
    CHECK_FALSE(stale.ok);
    CHECK(stale.error.find("stale") != std::string::npos);
    CHECK(read_text(dir.path / "bar.txt") == "a\nb\nc\nx\na\nb\nc\n");
}

TEST_CASE("apply_unified_diff: preserves missing final newline") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "a\nb\nc");  // no trailing \n
    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    REQUIRE(r.ok);
    CHECK(read_text(dir.path / "foo.txt") == "a\nB\nc");
}

TEST_CASE("apply_unified_diff: refuses -0,0 insert on non-empty file") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "keep\nme\n");
    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -0,0 +1,1 @@\n"
        "+injected\n";
    auto r = apply_unified_diff(patch, dir.path.string());
    CHECK_FALSE(r.ok);
    CHECK(read_text(dir.path / "foo.txt") == "keep\nme\n");
}

TEST_CASE("apply_unified_diff: pure-insert uses after-line offset (GNU patch)") {
    TempDir dir;
    write_text(dir.path / "foo.txt", "a\nb\nc\n");
    // @@ -2,0 @@ → insert after line 2 (between b and c), matching GNU patch.
    const char* mid =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -2,0 +3,1 @@\n"
        "+INSERTED\n";
    auto r = apply_unified_diff(mid, dir.path.string());
    REQUIRE(r.ok);
    CHECK(read_text(dir.path / "foo.txt") == "a\nb\nINSERTED\nc\n");

    write_text(dir.path / "bar.txt", "a\nb\nc\n");
    const char* append =
        "--- a/bar.txt\n"
        "+++ b/bar.txt\n"
        "@@ -3,0 +4,1 @@\n"
        "+APPEND\n";
    auto a = apply_unified_diff(append, dir.path.string());
    REQUIRE(a.ok);
    CHECK(read_text(dir.path / "bar.txt") == "a\nb\nc\nAPPEND\n");
}

TEST_CASE("apply_unified_diff: uses workspace_root not process cwd") {
    TempDir proj;
    TempDir decoy;
    write_text(proj.path / "foo.txt", "a\nb\nc\n");

    const fs::path prev = fs::current_path();
    fs::current_path(decoy.path);

    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n";
    auto r = apply_unified_diff(patch, proj.path.string());
    REQUIRE(r.ok);
    CHECK(read_text(proj.path / "foo.txt") == "a\nB\nc\n");
    CHECK_FALSE(fs::exists(decoy.path / "foo.txt"));

    fs::current_path(prev);
}

TEST_CASE("apply_unified_diff: missing workspace_root does not fall back") {
    TempDir decoy;
    const fs::path missing = decoy.path / "does_not_exist";
    const fs::path prev = fs::current_path();
    fs::current_path(decoy.path);

    const char* patch =
        "--- a/foo.txt\n"
        "+++ b/foo.txt\n"
        "@@ -0,0 +1,1 @@\n"
        "+hello\n";
    auto r = apply_unified_diff(patch, missing.string());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("missing") != std::string::npos);
    CHECK_FALSE(fs::exists(decoy.path / "foo.txt"));

    fs::current_path(prev);
}

TEST_CASE("DiffProposalStore: add / apply / reject / undo lifecycle") {
    DiffProposalStore store;
    const char* patch =
        "--- a/x.txt\n"
        "+++ b/x.txt\n"
        "@@ -1 +1 @@\n"
        "-old\n"
        "+new\n";
    auto a = store.add_patch(patch);
    REQUIRE(a);
    CHECK(a->id == 1);
    CHECK(a->path == "x.txt");
    CHECK(a->status == DiffProposalStatus::Pending);

    auto b = store.add_patch(patch);
    REQUIRE(b);
    CHECK(b->id == 2);
    CHECK(store.latest_pending()->id == 2);

    CHECK(store.mark_rejected(1));
    CHECK(store.get(1)->status == DiffProposalStatus::Rejected);

    DiffUndoSnapshot snap;
    snap.resolved_path = "/tmp/x";
    snap.had_file = true;
    snap.pre_image = "old\n";
    snap.post_image = "new\n";
    CHECK(store.mark_applied(2, snap));
    CHECK(store.latest_applied()->id == 2);
    CHECK(store.clear_undo_after_revert(2));
    CHECK(store.get(2)->status == DiffProposalStatus::Pending);
}
