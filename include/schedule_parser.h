#pragma once
// arbiter/include/schedule_parser.h
//
// Natural-language → ScheduleSpec parser.  Turns phrases an agent emits with
// /schedule into the persisted-row shape (kind, fire_at | recur_json,
// next_fire_at).  Strict by design — covers the common phrasings without an
// LLM round-trip.  Unparseable phrases return ParseError with a help string
// the writ surfaces back to the agent.
//
// Recognised forms:
//   in N (minute|min|m|hour|h|day|d|week|w)[s]
//   at HH:MM                                  (today; tomorrow if past)
//   tomorrow [at HH:MM]
//   on YYYY-MM-DD [at HH:MM]
//   every (hour|hourly)
//   every (day|daily) [at HH:MM]
//   every (week|weekly) [on <weekday>] [at HH:MM]
//   every <weekday> [at HH:MM]                (e.g. "every Monday at 09:00")
//   every N (minute|hour)[s]
//
// Times are parsed in the host's local timezone.  The caller passes the
// reference epoch (`now`) so tests can be deterministic.

#include <cstdint>
#include <optional>
#include <string>

namespace arbiter {

struct ScheduleSpec {
    enum class Kind { Once, Recurring };
    Kind        kind         = Kind::Once;
    int64_t     fire_at      = 0;     // for Once
    std::string recur_json;            // for Recurring (compact JSON)
    int64_t     next_fire_at = 0;      // first fire (== fire_at for Once)
    std::string normalized;            // canonical render for tool-result text
};

struct ParseError {
    std::string message;
};

struct ParseResult {
    bool         ok = false;
    ScheduleSpec spec;
    ParseError   error;
};

// Parse `phrase` against the local clock at `now` (epoch seconds).
// Returns the spec on success or a ParseError on failure.  Phrase
// matching is case-insensitive and tolerates surrounding whitespace.
ParseResult parse_schedule_phrase(const std::string& phrase, int64_t now);

// Compute the next fire time for a recurring spec, given the last fire.
// `recur_json` shape:
//   {"every":"hour"}                      → +1h from after
//   {"every":"day","at":"09:00"}          → next 09:00 strictly after `after`
//   {"every":"week","day":"mon","at":"09:00"} → next Mon 09:00 strictly after `after`
//   {"every_minutes":N}                   → +Nm from after
//   {"every_hours":N}                     → +Nh from after
// Returns 0 on parse failure (caller should mark task failed).
int64_t next_fire_for_recur(const std::string& recur_json, int64_t after);

// Help text returned in the writ's ERR for unparseable phrases.  Listing
// accepted forms gives the agent enough surface to retry without a round
// trip to the human.
std::string schedule_parser_help();

} // namespace arbiter
