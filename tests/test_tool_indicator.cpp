#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "tui/tui.h"

using namespace arbiter;

TEST_CASE("ToolCallIndicator begin arms and bump counts Finished tools") {
    ToolCallIndicator ind(nullptr);
    CHECK(ind.total() == 0);
    CHECK(ind.failed() == 0);

    // bump before begin is a no-op (turn not armed).
    ind.bump("fetch", true);
    CHECK(ind.total() == 0);

    ind.begin();
    ind.bump("fetch", true);
    ind.bump("exec", false);
    ind.bump("help", true);
    CHECK(ind.total() == 3);
    CHECK(ind.failed() == 1);

    // begin() resets for the next turn.
    ind.begin();
    CHECK(ind.total() == 0);
    CHECK(ind.failed() == 0);
    ind.bump("write", true);
    CHECK(ind.total() == 1);
    CHECK(ind.failed() == 0);

    const std::string summary = ind.finalize();
    CHECK(summary.find("1 tool") != std::string::npos);
    // finalize disarms — further bumps ignored until begin.
    ind.bump("fetch", true);
    CHECK(ind.total() == 1);
}
