#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "todo_resolve.h"

using namespace arbiter;

namespace {

TenantStore::Todo make_todo(int64_t id, std::string subject,
                            std::string status = "pending") {
    TenantStore::Todo t;
    t.id = id;
    t.subject = std::move(subject);
    t.status = std::move(status);
    return t;
}

} // namespace

TEST_CASE("resolve_todo_target accepts numeric ids") {
    const std::vector<TenantStore::Todo> open = {
        make_todo(14, "review deploy"),
        make_todo(15, "write postmortem"),
    };
    std::string err;
    CHECK(resolve_todo_target("14", open, err) == 14);
    CHECK(resolve_todo_target("#15", open, err) == 15);
    CHECK(err.empty());
}

TEST_CASE("resolve_todo_target matches unique subject / prefix") {
    const std::vector<TenantStore::Todo> open = {
        make_todo(14, "Analyze Nabonidus Chronicle, Cyrus Cylinder"),
        make_todo(15, "Write structured research brief"),
    };
    std::string err;
    CHECK(resolve_todo_target(
              "Analyze Nabonidus Chronicle, Cyrus Cylinder", open, err) == 14);
    CHECK(resolve_todo_target("analyze nabonidus", open, err) == 14);
    CHECK(resolve_todo_target("Write structured research brief", open, err) == 15);
    CHECK(err.empty());
}

TEST_CASE("resolve_todo_target rejects missing and ambiguous subjects") {
    const std::vector<TenantStore::Todo> open = {
        make_todo(1, "research Cyrus"),
        make_todo(2, "research Babylon"),
    };
    std::string err;
    CHECK(resolve_todo_target("no such todo", open, err) == 0);
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK(resolve_todo_target("research", open, err) == 0);
    CHECK(err.find("ambiguous") != std::string::npos);
}

TEST_CASE("resolve_todo_target rejects numeric id absent from open list") {
    const std::vector<TenantStore::Todo> open = {
        make_todo(14, "14 step checklist"),
        make_todo(15, "write postmortem"),
    };
    std::string err;
    CHECK(resolve_todo_target("99", open, err) == 0);
    CHECK(err.find("no open todo with that id") != std::string::npos);
    err.clear();
    // Must not fall through to subject prefix match on "14".
    CHECK(resolve_todo_target("14", std::vector<TenantStore::Todo>{}, err) == 0);
    CHECK(err.find("no open todo with that id") != std::string::npos);
}

TEST_CASE("sidebar subject start/done via record_tool") {
    // Covered more fully in unit_sidebar; keep resolve_todo_subject_ref here.
    const std::vector<TodoSubjectRef> refs = {
        {42, "Ship the landing page"},
        {43, "Polish the footer"},
    };
    std::string err;
    CHECK(resolve_todo_subject_ref("Ship the landing page", refs, err) == 42);
    CHECK(resolve_todo_subject_ref("43", refs, err) == 43);
}
