// tests/test_schedule_parser.cpp — Unit tests for the natural-language
// schedule parser.  All tests pin a fixed `now` (Sat 2026-05-09 10:00 local)
// so the result is deterministic against the host's TZ.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "schedule_parser.h"

#include <ctime>
#include <limits>
#include <string>

using namespace arbiter;

namespace {

// Fixed reference: 2026-05-09 10:00:00 local.  May 9, 2026 is a Saturday.
int64_t kNow() {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon  = 5 - 1;       // May
    tm.tm_mday = 9;
    tm.tm_hour = 10;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm));
}

} // namespace

TEST_CASE("parse: 'in N <unit>' produces one-shot fire_at = now + N*unit") {
    const int64_t now = kNow();

    SUBCASE("minutes") {
        auto r = parse_schedule_phrase("in 30 minutes", now);
        CHECK(r.ok);
        CHECK(r.spec.kind == ScheduleSpec::Kind::Once);
        CHECK(r.spec.fire_at == now + 30 * 60);
        CHECK(r.spec.next_fire_at == r.spec.fire_at);
    }

    SUBCASE("hours") {
        auto r = parse_schedule_phrase("in 2 hours", now);
        CHECK(r.ok);
        CHECK(r.spec.fire_at == now + 2 * 3600);
    }

    SUBCASE("days") {
        auto r = parse_schedule_phrase("in 1 day", now);
        CHECK(r.ok);
        CHECK(r.spec.fire_at == now + 86400);
    }

    SUBCASE("short forms") {
        auto r = parse_schedule_phrase("in 5 min", now);
        CHECK(r.ok);
        CHECK(r.spec.fire_at == now + 5 * 60);

        r = parse_schedule_phrase("in 1 h", now);
        CHECK(r.ok);
        CHECK(r.spec.fire_at == now + 3600);
    }
}

TEST_CASE("parse: 'at HH:MM' fires today if future, tomorrow if past") {
    const int64_t now = kNow();

    SUBCASE("future today") {
        auto r = parse_schedule_phrase("at 17:30", now);
        CHECK(r.ok);
        CHECK(r.spec.kind == ScheduleSpec::Kind::Once);
        CHECK(r.spec.fire_at > now);
        CHECK(r.spec.fire_at - now < 86400);
    }

    SUBCASE("past today rolls to tomorrow") {
        auto r = parse_schedule_phrase("at 09:00", now);
        CHECK(r.ok);
        CHECK(r.spec.fire_at > now);
        // Should be ~23 hours out (10:00 → 09:00 next day).
        CHECK(r.spec.fire_at - now > 22 * 3600);
        CHECK(r.spec.fire_at - now < 24 * 3600);
    }
}

TEST_CASE("parse: 'tomorrow [at HH:MM]'") {
    const int64_t now = kNow();
    auto r = parse_schedule_phrase("tomorrow at 09:00", now);
    CHECK(r.ok);
    CHECK(r.spec.kind == ScheduleSpec::Kind::Once);
    CHECK(r.spec.fire_at - now == 23 * 3600);
}

TEST_CASE("parse: 'on YYYY-MM-DD'") {
    const int64_t now = kNow();
    auto r = parse_schedule_phrase("on 2026-06-01 at 12:00", now);
    CHECK(r.ok);
    CHECK(r.spec.kind == ScheduleSpec::Kind::Once);
    CHECK(r.spec.fire_at > now);

    SUBCASE("rejects past dates") {
        auto past = parse_schedule_phrase("on 2020-01-01", now);
        CHECK(!past.ok);
    }

<<<<<<< HEAD
    SUBCASE("year 1 does not persist next_fire_at=-1") {
        // mktime fails or yields a pre-epoch value; either way we fail
        // closed instead of storing -1 (which list_due treats as always due).
        auto r = parse_schedule_phrase("on 0001-01-01 at 09:00", now);
        CHECK_FALSE(r.ok);
        CHECK(r.spec.next_fire_at == 0);
=======
    SUBCASE("rejects calendar-impossible dates instead of overflowing") {
        // mktime would turn Feb 31 into early March; /schedule must fail closed.
        auto feb31 = parse_schedule_phrase("on 2027-02-31 at 12:00", now);
        CHECK_FALSE(feb31.ok);
        CHECK(feb31.error.message.find("YYYY-MM-DD") != std::string::npos);

        auto jun31 = parse_schedule_phrase("on 2026-06-31", now);
        CHECK_FALSE(jun31.ok);

        auto non_leap = parse_schedule_phrase("on 2027-02-29 at 09:00", now);
        CHECK_FALSE(non_leap.ok);
    }

    SUBCASE("accepts leap-day in a leap year") {
        auto leap = parse_schedule_phrase("on 2028-02-29 at 12:00", now);
        CHECK(leap.ok);
        CHECK(leap.spec.kind == ScheduleSpec::Kind::Once);
        CHECK(leap.spec.fire_at > now);
>>>>>>> origin/main
    }
}

TEST_CASE("parse: recurring shapes") {
    const int64_t now = kNow();

    SUBCASE("hourly") {
        auto r = parse_schedule_phrase("every hour", now);
        CHECK(r.ok);
        CHECK(r.spec.kind == ScheduleSpec::Kind::Recurring);
        CHECK(r.spec.next_fire_at == now + 3600);
        CHECK(r.spec.recur_json.find("\"every\":\"hour\"") != std::string::npos);
    }

    SUBCASE("every N minutes") {
        auto r = parse_schedule_phrase("every 15 minutes", now);
        CHECK(r.ok);
        CHECK(r.spec.kind == ScheduleSpec::Kind::Recurring);
        CHECK(r.spec.next_fire_at == now + 15 * 60);
        CHECK(r.spec.recur_json.find("\"every_minutes\":15") != std::string::npos);
    }

    SUBCASE("daily at HH:MM") {
        auto r = parse_schedule_phrase("every day at 17:00", now);
        CHECK(r.ok);
        CHECK(r.spec.kind == ScheduleSpec::Kind::Recurring);
        CHECK(r.spec.next_fire_at > now);
        CHECK(r.spec.recur_json.find("\"every\":\"day\"") != std::string::npos);
        CHECK(r.spec.recur_json.find("\"at\":\"17:00\"") != std::string::npos);
    }

    SUBCASE("every Monday — picks next Monday after now (kNow is Saturday)") {
        auto r = parse_schedule_phrase("every monday at 09:00", now);
        CHECK(r.ok);
        CHECK(r.spec.kind == ScheduleSpec::Kind::Recurring);
        CHECK(r.spec.next_fire_at > now);
        // Sat → Mon = 2 days at 09:00, so ~47 hours out.
        const int64_t delta = r.spec.next_fire_at - now;
        CHECK(delta > 46 * 3600);
        CHECK(delta < 49 * 3600);
        CHECK(r.spec.recur_json.find("\"day\":\"mon\"") != std::string::npos);
    }
}

TEST_CASE("parse: rejects unrecognised phrases with a help-bearing error") {
    const int64_t now = kNow();
    auto r = parse_schedule_phrase("when the moon is full", now);
    CHECK(!r.ok);
    CHECK(!r.error.message.empty());

    auto h = schedule_parser_help();
    CHECK(h.find("/schedule accepts") != std::string::npos);
    CHECK(h.find("every") != std::string::npos);
}

TEST_CASE("next_fire_for_recur: advances daily, hourly, weekly correctly") {
    const int64_t now = kNow();

    SUBCASE("hourly: +3600") {
        int64_t n = next_fire_for_recur(R"({"every":"hour"})", now);
        CHECK(n == now + 3600);
    }

    SUBCASE("every_minutes:5") {
        int64_t n = next_fire_for_recur(R"({"every_minutes":5})", now);
        CHECK(n == now + 5 * 60);
    }

    SUBCASE("daily at 09:00 — kNow is 10:00 so next is tomorrow 09:00") {
        int64_t n = next_fire_for_recur(R"({"every":"day","at":"09:00"})", now);
        CHECK(n - now > 22 * 3600);
        CHECK(n - now < 24 * 3600);
    }

    SUBCASE("weekly mon@09:00 from Saturday") {
        int64_t n = next_fire_for_recur(
            R"({"every":"week","day":"mon","at":"09:00"})", now);
        CHECK(n - now > 46 * 3600);
        CHECK(n - now < 49 * 3600);
    }

    SUBCASE("malformed json returns 0") {
        int64_t n = next_fire_for_recur("{not json", now);
        CHECK(n == 0);
    }

    SUBCASE("overflowing every_minutes returns 0 rather than wrapping") {
        int64_t n = next_fire_for_recur(
            R"({"every_minutes":200000000000000000})", now);
        CHECK(n == 0);
    }

    SUBCASE("every hour near int64 max returns 0 rather than wrapping") {
        // `after + 3600` overflowed; same fail-closed contract as every_hours.
        const int64_t huge = std::numeric_limits<int64_t>::max() - 100;
        int64_t n = next_fire_for_recur(R"({"every":"hour"})", huge);
        CHECK(n == 0);
    }
}

TEST_CASE("parse: huge intervals fail closed without throwing") {
    const int64_t now = kNow();

    SUBCASE("digit run exceeds int64") {
        auto r = parse_schedule_phrase(
            "in 99999999999999999999 weeks", now);
        CHECK_FALSE(r.ok);
        CHECK(r.error.message.find("too large") != std::string::npos);
    }

    SUBCASE("product overflows int64") {
        auto r = parse_schedule_phrase("in 20000000000000 weeks", now);
        CHECK_FALSE(r.ok);
        CHECK(r.error.message.find("too large") != std::string::npos);
    }

    SUBCASE("every N hours overflow") {
        auto r = parse_schedule_phrase(
            "every 99999999999999999999 hours", now);
        CHECK_FALSE(r.ok);
        CHECK(r.error.message.find("too large") != std::string::npos);
    }

    SUBCASE("every hour near int64 max fails closed") {
        const int64_t huge = std::numeric_limits<int64_t>::max() - 100;
        auto r = parse_schedule_phrase("every hour", huge);
        CHECK_FALSE(r.ok);
        CHECK(r.error.message.find("too large") != std::string::npos);
        CHECK(r.spec.next_fire_at == 0);
    }

    SUBCASE("ordinary values still parse") {
        auto r = parse_schedule_phrase("in 2 hours", now);
        CHECK(r.ok);
        CHECK(r.spec.fire_at == now + 2 * 3600);
    }
}

TEST_CASE("/schedule pause/resume refuse terminal statuses") {
    CHECK(schedule_resume_block_reason("paused").empty());
    CHECK(schedule_pause_block_reason("paused").empty());
    CHECK(schedule_pause_block_reason("active").empty());
    CHECK(schedule_pause_block_reason("running").empty());

    CHECK(schedule_resume_block_reason("completed").find("completed")
          != std::string::npos);
    CHECK(schedule_pause_block_reason("completed").find("completed")
          != std::string::npos);
    CHECK(schedule_resume_block_reason("failed").find("failed")
          != std::string::npos);
    CHECK(schedule_pause_block_reason("failed").find("failed")
          != std::string::npos);
    CHECK(schedule_resume_block_reason("canceled").find("canceled")
          != std::string::npos);
    CHECK(schedule_pause_block_reason("canceled").find("canceled")
          != std::string::npos);
    CHECK(schedule_resume_block_reason("active").find("already active")
          != std::string::npos);
    CHECK(schedule_resume_block_reason("running").find("running")
          != std::string::npos);
    CHECK(schedule_resume_block_reason("bogus").find("cannot be resumed")
          != std::string::npos);
    CHECK(schedule_pause_block_reason("bogus").find("cannot be paused")
          != std::string::npos);
}
