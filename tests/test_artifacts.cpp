// tests/test_artifacts.cpp — Unit tests for the per-tenant artifact store.
//
// Pins the contract that backs:
//   POST   /v1/conversations/:id/artifacts
//   GET    /v1/conversations/:id/artifacts[/:aid][/raw]
//   DELETE /v1/conversations/:id/artifacts/:aid
//   GET    /v1/artifacts (tenant-scoped discovery)
// and the agent-side /write --persist / /read / /list slash commands.
//
// Schema-level concerns covered: PUT-on-conflict semantics, three-tier
// quota enforcement, tenant + conversation scoping, cascade deletes,
// and the path sanitiser's reject/accept matrix.  HTTP-layer concerns
// (status code mapping, ETag handling) live in api_server tests.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tenant_store.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace index_ai;

namespace {

struct TempDb {
    fs::path path;
    TempDb() {
        const auto pid = static_cast<long long>(::getpid());
        const auto now = std::chrono::steady_clock::now()
                              .time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("arbiter_artifactstest_" + std::to_string(pid) + "_" +
                std::to_string(now) + ".db");
    }
    ~TempDb() {
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path.string() + "-wal", ec);
        fs::remove(path.string() + "-shm", ec);
    }
};

int64_t make_tenant(TenantStore& s, const std::string& name) {
    return s.create_tenant(name, 0).tenant.id;
}

int64_t make_conversation(TenantStore& s, int64_t tid, const std::string& title) {
    return s.create_conversation(tid, title, "index", "").id;
}

// Sanitize + assert success in one helper to keep test bodies tight.
std::string sane(const std::string& raw) {
    std::string err;
    auto r = sanitize_artifact_path(raw, err);
    REQUIRE_MESSAGE(r.has_value(), "expected '" << raw << "' to be accepted, got: " << err);
    return *r;
}

bool sanitize_rejects(const std::string& raw, const std::string& expected_substr = "") {
    std::string err;
    auto r = sanitize_artifact_path(raw, err);
    if (r.has_value()) return false;
    if (!expected_substr.empty() && err.find(expected_substr) == std::string::npos) {
        MESSAGE("path '" << raw << "' was rejected with unexpected reason: " << err);
        return false;
    }
    return true;
}

} // namespace

// ── 1. Path sanitizer ──────────────────────────────────────────────────

TEST_CASE("sanitize_artifact_path: simple paths round-trip canonical") {
    CHECK(sane("output/report.md")  == "output/report.md");
    CHECK(sane("notes.txt")         == "notes.txt");
    CHECK(sane("a/b/c/d/file.json") == "a/b/c/d/file.json");
}

TEST_CASE("sanitize_artifact_path: backslashes normalised to forward") {
    CHECK(sane("output\\report.md") == "output/report.md");
    CHECK(sane("a\\b\\c.txt")        == "a/b/c.txt");
}

TEST_CASE("sanitize_artifact_path: collapses repeated separators, drops trailing") {
    CHECK(sane("a//b///c.txt") == "a/b/c.txt");
    CHECK(sane("a/b/c.txt/")   == "a/b/c.txt");
}

TEST_CASE("sanitize_artifact_path: rejects empty and oversize") {
    CHECK(sanitize_rejects("", "empty"));
    CHECK(sanitize_rejects(std::string(257, 'a'), "256"));
}

TEST_CASE("sanitize_artifact_path: rejects absolute paths") {
    CHECK(sanitize_rejects("/etc/passwd",   "absolute"));
    CHECK(sanitize_rejects("\\Windows\\System32", "absolute"));
    CHECK(sanitize_rejects("C:\\Users\\foo", ":"));    // drive letter via colon check
}

TEST_CASE("sanitize_artifact_path: rejects traversal") {
    CHECK(sanitize_rejects("../etc/passwd",        "traversal"));
    CHECK(sanitize_rejects("output/../../etc",     "traversal"));
    CHECK(sanitize_rejects("./relative",           "traversal"));
    CHECK(sanitize_rejects("a/b/../c",             "traversal"));
    CHECK(sanitize_rejects("..",                    "traversal"));
}

TEST_CASE("sanitize_artifact_path: rejects hidden (dotfile) components") {
    CHECK(sanitize_rejects(".env",          "hidden"));
    CHECK(sanitize_rejects("output/.git",   "hidden"));
    CHECK(sanitize_rejects("a/.hidden/b",   "hidden"));
}

TEST_CASE("sanitize_artifact_path: rejects null bytes and control chars") {
    std::string with_null = "foo\0bar.txt";
    with_null.resize(11);   // include the embedded NUL
    CHECK(sanitize_rejects(with_null, "null"));
    CHECK(sanitize_rejects(std::string("foo\x01" "bar"), "control"));
    CHECK(sanitize_rejects(std::string("foo\x7f" "bar"), "control"));
}

TEST_CASE("sanitize_artifact_path: rejects Windows-reserved names") {
    CHECK(sanitize_rejects("CON",       "Windows"));
    CHECK(sanitize_rejects("con.txt",   "Windows"));      // case-insensitive + extension
    CHECK(sanitize_rejects("output/PRN.log", "Windows"));
    CHECK(sanitize_rejects("COM3",      "Windows"));
    CHECK(sanitize_rejects("LPT9.dat",  "Windows"));
}

TEST_CASE("sanitize_artifact_path: rejects oversize components") {
    std::string long_comp(129, 'x');
    CHECK(sanitize_rejects(long_comp, "128"));
    CHECK(sanitize_rejects("output/" + long_comp, "128"));
}

// ── 2. Artifact CRUD round-trip ────────────────────────────────────────

TEST_CASE("artifact round-trip: create / get / update / delete") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "research");

    auto put = s.put_artifact(tid, cid, "output/draft.md",
                                "# Hello world\n", "text/markdown");
    REQUIRE(put.status == PutArtifactResult::Status::Created);
    REQUIRE(put.record.has_value());
    CHECK(put.record->path == "output/draft.md");
    CHECK(put.record->size == 14);
    CHECK(put.record->mime_type == "text/markdown");
    CHECK(put.record->sha256.size() == 64);
    CHECK(put.tenant_used_bytes       == 14);
    CHECK(put.conversation_used_bytes == 14);

    auto meta = s.get_artifact_meta(tid, put.record->id);
    REQUIRE(meta.has_value());
    CHECK(meta->path == "output/draft.md");

    auto content = s.get_artifact_content(tid, put.record->id);
    REQUIRE(content.has_value());
    CHECK(*content == "# Hello world\n");

    auto by_path = s.get_artifact_meta_by_path(tid, cid, "output/draft.md");
    REQUIRE(by_path.has_value());
    CHECK(by_path->id == put.record->id);

    // PUT-on-conflict overwrites.
    auto upd = s.put_artifact(tid, cid, "output/draft.md",
                                "# Hello world v2\n", "text/markdown");
    CHECK(upd.status == PutArtifactResult::Status::Updated);
    REQUIRE(upd.record.has_value());
    CHECK(upd.record->id == put.record->id);              // same row
    CHECK(upd.record->size == 17);
    CHECK(upd.record->updated_at >= put.record->updated_at);

    auto v2 = s.get_artifact_content(tid, put.record->id);
    REQUIRE(v2.has_value());
    CHECK(*v2 == "# Hello world v2\n");

    REQUIRE(s.delete_artifact(tid, put.record->id));
    CHECK_FALSE(s.get_artifact_meta(tid, put.record->id).has_value());
    CHECK(s.bytes_used_conversation(tid, cid) == 0);
}

TEST_CASE("listing: newest-updated first, conversation- and tenant-scoped") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t c1  = make_conversation(s, tid, "thread-1");
    const int64_t c2  = make_conversation(s, tid, "thread-2");

    REQUIRE(s.put_artifact(tid, c1, "a.txt", "a", "text/plain").status
            == PutArtifactResult::Status::Created);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(s.put_artifact(tid, c1, "b.txt", "bb", "text/plain").status
            == PutArtifactResult::Status::Created);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(s.put_artifact(tid, c2, "c.txt", "ccc", "text/plain").status
            == PutArtifactResult::Status::Created);

    // Conversation-scoped lists isolate by conversation.
    auto lc1 = s.list_artifacts_conversation(tid, c1, 50);
    REQUIRE(lc1.size() == 2);
    CHECK(lc1[0].path == "b.txt");      // newest first
    CHECK(lc1[1].path == "a.txt");
    auto lc2 = s.list_artifacts_conversation(tid, c2, 50);
    REQUIRE(lc2.size() == 1);
    CHECK(lc2[0].path == "c.txt");

    // Tenant-scoped list includes both conversations.
    auto lt = s.list_artifacts_tenant(tid, 50);
    REQUIRE(lt.size() == 3);
    CHECK(lt[0].path == "c.txt");        // c.txt is newest across the tenant
}

// ── 3. Quota enforcement ───────────────────────────────────────────────

TEST_CASE("per-file quota rejects oversize without consuming budget") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "research");

    std::string oversize(kArtifactPerFileMaxBytes + 1, 'x');
    auto r = s.put_artifact(tid, cid, "huge.bin", oversize, "application/octet-stream");
    CHECK(r.status == PutArtifactResult::Status::QuotaExceeded);
    CHECK(r.error_msg.find("per-file") != std::string::npos);
    CHECK(r.tenant_used_bytes == 0);     // nothing was written
    CHECK(s.list_artifacts_conversation(tid, cid, 50).empty());
}

TEST_CASE("PUT overwrite subtracts existing size before quota check") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "research");

    // Write a file at half the per-file cap.  Overwriting it with a
    // same-or-smaller file MUST always succeed regardless of total quota
    // (the in-place delta is ≤ 0).
    std::string half(kArtifactPerFileMaxBytes / 2, 'a');
    auto first = s.put_artifact(tid, cid, "data.bin", half, "application/octet-stream");
    REQUIRE(first.status == PutArtifactResult::Status::Created);

    std::string smaller(kArtifactPerFileMaxBytes / 4, 'b');
    auto over = s.put_artifact(tid, cid, "data.bin", smaller, "application/octet-stream");
    CHECK(over.status == PutArtifactResult::Status::Updated);
}

// ── 4. Tenant isolation + cascade ──────────────────────────────────────

TEST_CASE("artifacts are tenant-isolated") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t a = make_tenant(s, "tenant-a");
    const int64_t b = make_tenant(s, "tenant-b");
    const int64_t a_conv = make_conversation(s, a, "thread-a");
    const int64_t b_conv = make_conversation(s, b, "thread-b");

    auto pa = s.put_artifact(a, a_conv, "shared.txt", "from A", "text/plain");
    auto pb = s.put_artifact(b, b_conv, "shared.txt", "from B", "text/plain");
    REQUIRE(pa.record.has_value());
    REQUIRE(pb.record.has_value());
    CHECK(pa.record->id != pb.record->id);

    // Cross-tenant read returns nullopt — no leakage.
    CHECK_FALSE(s.get_artifact_meta(b, pa.record->id).has_value());
    CHECK_FALSE(s.get_artifact_content(a, pb.record->id).has_value());

    // Cross-tenant delete is a no-op.
    CHECK_FALSE(s.delete_artifact(b, pa.record->id));
    auto still = s.get_artifact_meta(a, pa.record->id);
    REQUIRE(still.has_value());

    // bytes_used scopes to the right tenant.
    CHECK(s.bytes_used_tenant(a) == 6);
    CHECK(s.bytes_used_tenant(b) == 6);
}

TEST_CASE("deleting a conversation cascades to its artifacts") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "doomed");

    REQUIRE(s.put_artifact(tid, cid, "a.txt", "hello", "text/plain").status
            == PutArtifactResult::Status::Created);
    REQUIRE(s.put_artifact(tid, cid, "b.txt", "world", "text/plain").status
            == PutArtifactResult::Status::Created);
    REQUIRE(s.bytes_used_conversation(tid, cid) == 10);

    REQUIRE(s.delete_conversation(tid, cid));
    // Both artifacts gone via FK CASCADE.
    CHECK(s.list_artifacts_conversation(tid, cid, 50).empty());
    CHECK(s.bytes_used_conversation(tid, cid) == 0);
    CHECK(s.bytes_used_tenant(tid) == 0);
}

TEST_CASE("default mime_type when unspecified") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "thread");

    auto r = s.put_artifact(tid, cid, "blob.bin", "abc", "");
    REQUIRE(r.record.has_value());
    CHECK(r.record->mime_type == "application/octet-stream");
}

// ── 5. Memory → Artifact linkage ───────────────────────────────────────

TEST_CASE("memory entry can carry an artifact_id; round-trips through storage") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "research");

    auto art = s.put_artifact(tid, cid, "report.md", "# findings\n", "text/markdown");
    REQUIRE(art.record.has_value());
    const int64_t aid = art.record->id;

    auto entry = s.create_entry(tid, "reference", "Findings report",
                                  "Source: agent /write --persist", "agent",
                                  "[]", "accepted", aid);
    CHECK(entry.artifact_id == aid);

    auto reload = s.get_entry(tid, entry.id);
    REQUIRE(reload.has_value());
    CHECK(reload->artifact_id == aid);
}

TEST_CASE("update_entry can set and clear artifact_id") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "thread");

    auto art = s.put_artifact(tid, cid, "draft.md", "draft", "text/markdown");
    REQUIRE(art.record.has_value());
    const int64_t aid = art.record->id;

    // Create without artifact_id, then patch it on.
    auto entry = s.create_entry(tid, "project", "Draft tracker", "", "", "[]");
    CHECK(entry.artifact_id == 0);

    REQUIRE(s.update_entry(tid, entry.id, std::nullopt, std::nullopt,
                            std::nullopt, std::nullopt, std::nullopt,
                            /*artifact_id=*/std::optional<int64_t>(aid)));
    auto with_link = s.get_entry(tid, entry.id);
    REQUIRE(with_link.has_value());
    CHECK(with_link->artifact_id == aid);

    // Clear the link with optional<int64_t>(0).
    REQUIRE(s.update_entry(tid, entry.id, std::nullopt, std::nullopt,
                            std::nullopt, std::nullopt, std::nullopt,
                            /*artifact_id=*/std::optional<int64_t>(0)));
    auto cleared = s.get_entry(tid, entry.id);
    REQUIRE(cleared.has_value());
    CHECK(cleared->artifact_id == 0);
}

TEST_CASE("deleting an artifact nullifies referencing memory entries (direct delete)") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "thread");

    auto art = s.put_artifact(tid, cid, "doomed.md", "soon-gone", "text/markdown");
    REQUIRE(art.record.has_value());
    auto entry = s.create_entry(tid, "reference", "Refers", "", "", "[]",
                                  "accepted", art.record->id);
    REQUIRE(entry.artifact_id == art.record->id);

    REQUIRE(s.delete_artifact(tid, art.record->id));

    auto stale = s.get_entry(tid, entry.id);
    REQUIRE(stale.has_value());
    CHECK(stale->artifact_id == 0);    // link cleared, entry preserved
}

TEST_CASE("conversation cascade-delete also nullifies referencing memory entries") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t tid = make_tenant(s, "acme");
    const int64_t cid = make_conversation(s, tid, "doomed");

    auto art = s.put_artifact(tid, cid, "ephemeral.md", "by", "text/markdown");
    REQUIRE(art.record.has_value());
    auto entry = s.create_entry(tid, "reference", "Refers", "", "", "[]",
                                  "accepted", art.record->id);

    // Conversation delete cascades to artifacts via FK; the trigger
    // fires on each cascaded artifact delete and clears the memory link.
    REQUIRE(s.delete_conversation(tid, cid));

    auto stale = s.get_entry(tid, entry.id);
    REQUIRE(stale.has_value());                  // memory entry survives
    CHECK(stale->artifact_id == 0);              // link cleared
}

TEST_CASE("artifact link is not affected by another tenant's delete") {
    TempDb db; TenantStore s; s.open(db.path.string());
    const int64_t a = make_tenant(s, "tenant-a");
    const int64_t b = make_tenant(s, "tenant-b");
    const int64_t a_conv = make_conversation(s, a, "a-thread");
    const int64_t b_conv = make_conversation(s, b, "b-thread");

    auto a_art = s.put_artifact(a, a_conv, "a.md", "A", "text/markdown");
    auto b_art = s.put_artifact(b, b_conv, "b.md", "B", "text/markdown");
    auto a_ent = s.create_entry(a, "reference", "A entry", "", "", "[]",
                                  "accepted", a_art.record->id);

    // Tenant B trying to delete tenant A's artifact does nothing.
    CHECK_FALSE(s.delete_artifact(b, a_art.record->id));
    auto still = s.get_entry(a, a_ent.id);
    REQUIRE(still.has_value());
    CHECK(still->artifact_id == a_art.record->id);
}
