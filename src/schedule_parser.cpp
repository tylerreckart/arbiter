// arbiter/src/schedule_parser.cpp
//
// Strict natural-language parser for the /schedule writ.  Local-time math
// uses the host's TZ; tests pin `now` so the result is deterministic.

#include "schedule_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <sstream>
#include <string>

namespace arbiter {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string collapse_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_ws = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!in_ws && !out.empty()) out.push_back(' ');
            in_ws = true;
        } else {
            out.push_back(c);
            in_ws = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// 0 = Sunday … 6 = Saturday, matching tm_wday.
int weekday_to_int(const std::string& w) {
    static const std::array<const char*, 7> names = {
        "sun", "mon", "tue", "wed", "thu", "fri", "sat"
    };
    static const std::array<const char*, 7> longs = {
        "sunday", "monday", "tuesday", "wednesday",
        "thursday", "friday", "saturday"
    };
    std::string lo = lower(w);
    for (int i = 0; i < 7; ++i) {
        if (lo == names[static_cast<size_t>(i)]) return i;
        if (lo == longs[static_cast<size_t>(i)]) return i;
    }
    return -1;
}

const char* weekday_short(int wd) {
    static const std::array<const char*, 7> names = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    if (wd < 0 || wd > 6) return "?";
    return names[static_cast<size_t>(wd)];
}

bool parse_hhmm(const std::string& s, int& hh, int& mm) {
    std::regex re(R"(^([0-9]{1,2}):([0-9]{2})$)");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return false;
    hh = std::stoi(m[1].str());
    mm = std::stoi(m[2].str());
    return hh >= 0 && hh < 24 && mm >= 0 && mm < 60;
}

bool parse_ymd(const std::string& s, int& y, int& mo, int& d) {
    std::regex re(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return false;
    y  = std::stoi(m[1].str());
    mo = std::stoi(m[2].str());
    d  = std::stoi(m[3].str());
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

int64_t make_local_epoch(int y, int mo, int d, int hh, int mm) {
    std::tm tm{};
    tm.tm_year  = y - 1900;
    tm.tm_mon   = mo - 1;
    tm.tm_mday  = d;
    tm.tm_hour  = hh;
    tm.tm_min   = mm;
    tm.tm_sec   = 0;
    tm.tm_isdst = -1;
    time_t t = std::mktime(&tm);
    return static_cast<int64_t>(t);
}

// Compute today's local date components for the reference now.
void local_date(int64_t now, int& y, int& mo, int& d, int& hh, int& mm, int& wday) {
    time_t t = static_cast<time_t>(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    y    = tm.tm_year + 1900;
    mo   = tm.tm_mon + 1;
    d    = tm.tm_mday;
    hh   = tm.tm_hour;
    mm   = tm.tm_min;
    wday = tm.tm_wday;
}

int64_t next_local_at(int64_t after, int hh, int mm) {
    int y, mo, d, h0, m0, w;
    local_date(after, y, mo, d, h0, m0, w);
    int64_t cand = make_local_epoch(y, mo, d, hh, mm);
    if (cand <= after) cand = make_local_epoch(y, mo, d + 1, hh, mm);
    return cand;
}

int64_t next_local_weekday_at(int64_t after, int target_wday, int hh, int mm) {
    int y, mo, d, h0, m0, w;
    local_date(after, y, mo, d, h0, m0, w);
    int delta = (target_wday - w + 7) % 7;
    int64_t cand = make_local_epoch(y, mo, d + delta, hh, mm);
    if (cand <= after) cand = make_local_epoch(y, mo, d + delta + 7, hh, mm);
    return cand;
}

std::string two(int n) {
    std::ostringstream oss;
    if (n < 10) oss << '0';
    oss << n;
    return oss.str();
}

std::string format_local(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << (tm.tm_year + 1900) << '-' << two(tm.tm_mon + 1) << '-' << two(tm.tm_mday)
        << ' ' << two(tm.tm_hour) << ':' << two(tm.tm_min);
    return oss.str();
}

// Compact JSON helpers (kept inline to avoid a json_writer dep here).
std::string js_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') { out += "\\n"; }
        else { out.push_back(c); }
    }
    out.push_back('"');
    return out;
}

std::string parse_json_str_field(const std::string& obj, const std::string& key) {
    // Tiny extractor — only used on JSON we wrote ourselves.
    std::string needle = "\"" + key + "\":\"";
    auto p = obj.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    auto q = obj.find('"', p);
    if (q == std::string::npos) return "";
    return obj.substr(p, q - p);
}

int64_t parse_json_int_field(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto p = obj.find(needle);
    if (p == std::string::npos) return 0;
    p += needle.size();
    while (p < obj.size() && std::isspace(static_cast<unsigned char>(obj[p]))) ++p;
    int64_t sign = 1;
    if (p < obj.size() && obj[p] == '-') { sign = -1; ++p; }
    int64_t v = 0;
    while (p < obj.size() && std::isdigit(static_cast<unsigned char>(obj[p]))) {
        v = v * 10 + (obj[p] - '0');
        ++p;
    }
    return v * sign;
}

} // namespace

ParseResult parse_schedule_phrase(const std::string& phrase_in, int64_t now) {
    ParseResult r;
    std::string phrase = collapse_ws(trim(lower(phrase_in)));
    if (phrase.empty()) {
        r.error.message = "empty schedule phrase";
        return r;
    }

    // ── "in N <unit>[s]" ────────────────────────────────────────────────
    {
        std::regex re(R"(^in\s+(\d+)\s+(minute|min|m|hour|h|day|d|week|w)s?$)");
        std::smatch m;
        if (std::regex_match(phrase, m, re)) {
            int64_t n = std::stoll(m[1].str());
            std::string u = m[2].str();
            int64_t mult = 60;
            std::string unit_word = "minutes";
            if (u == "hour" || u == "h") { mult = 3600; unit_word = "hours"; }
            else if (u == "day" || u == "d") { mult = 86400; unit_word = "days"; }
            else if (u == "week" || u == "w") { mult = 7 * 86400; unit_word = "weeks"; }
            r.ok                = true;
            r.spec.kind         = ScheduleSpec::Kind::Once;
            r.spec.fire_at      = now + n * mult;
            r.spec.next_fire_at = r.spec.fire_at;
            std::ostringstream oss;
            oss << "in " << n << " " << unit_word << " (" << format_local(r.spec.fire_at) << ")";
            r.spec.normalized = oss.str();
            return r;
        }
    }

    // ── "at HH:MM" (today; tomorrow if past) ────────────────────────────
    {
        std::regex re(R"(^at\s+(\d{1,2}:\d{2})$)");
        std::smatch m;
        if (std::regex_match(phrase, m, re)) {
            int hh, mm;
            if (parse_hhmm(m[1].str(), hh, mm)) {
                r.ok                = true;
                r.spec.kind         = ScheduleSpec::Kind::Once;
                r.spec.fire_at      = next_local_at(now, hh, mm);
                r.spec.next_fire_at = r.spec.fire_at;
                r.spec.normalized   = "at " + format_local(r.spec.fire_at);
                return r;
            }
        }
    }

    // ── "tomorrow [at HH:MM]" ───────────────────────────────────────────
    {
        std::regex re(R"(^tomorrow(?:\s+at\s+(\d{1,2}:\d{2}))?$)");
        std::smatch m;
        if (std::regex_match(phrase, m, re)) {
            int hh = 9, mm = 0;
            if (m[1].matched && !parse_hhmm(m[1].str(), hh, mm)) {
                r.error.message = "invalid HH:MM after 'tomorrow at'";
                return r;
            }
            int y, mo, d, h0, m0, w;
            local_date(now, y, mo, d, h0, m0, w);
            int64_t cand = make_local_epoch(y, mo, d + 1, hh, mm);
            r.ok                = true;
            r.spec.kind         = ScheduleSpec::Kind::Once;
            r.spec.fire_at      = cand;
            r.spec.next_fire_at = cand;
            r.spec.normalized   = "tomorrow at " + format_local(cand);
            return r;
        }
    }

    // ── "on YYYY-MM-DD [at HH:MM]" ──────────────────────────────────────
    {
        std::regex re(R"(^on\s+(\d{4}-\d{2}-\d{2})(?:\s+at\s+(\d{1,2}:\d{2}))?$)");
        std::smatch m;
        if (std::regex_match(phrase, m, re)) {
            int y, mo, d;
            if (!parse_ymd(m[1].str(), y, mo, d)) {
                r.error.message = "invalid YYYY-MM-DD";
                return r;
            }
            int hh = 9, mm = 0;
            if (m[2].matched && !parse_hhmm(m[2].str(), hh, mm)) {
                r.error.message = "invalid HH:MM after 'on … at'";
                return r;
            }
            int64_t cand = make_local_epoch(y, mo, d, hh, mm);
            if (cand <= now) {
                r.error.message = "scheduled time is in the past";
                return r;
            }
            r.ok                = true;
            r.spec.kind         = ScheduleSpec::Kind::Once;
            r.spec.fire_at      = cand;
            r.spec.next_fire_at = cand;
            r.spec.normalized   = "on " + format_local(cand);
            return r;
        }
    }

    // ── "every hour" / "hourly" ─────────────────────────────────────────
    if (phrase == "every hour" || phrase == "hourly") {
        r.ok                = true;
        r.spec.kind         = ScheduleSpec::Kind::Recurring;
        r.spec.recur_json   = R"({"every":"hour"})";
        r.spec.next_fire_at = now + 3600;
        r.spec.normalized   = "every hour";
        return r;
    }

    // ── "every N (minute|hour)[s]" ──────────────────────────────────────
    {
        std::regex re(R"(^every\s+(\d+)\s+(minute|min|m|hour|h)s?$)");
        std::smatch m;
        if (std::regex_match(phrase, m, re)) {
            int64_t n = std::stoll(m[1].str());
            if (n < 1) {
                r.error.message = "interval must be >= 1";
                return r;
            }
            std::string u = m[2].str();
            bool is_hour = (u == "hour" || u == "h");
            r.ok          = true;
            r.spec.kind   = ScheduleSpec::Kind::Recurring;
            std::ostringstream js;
            js << "{\"" << (is_hour ? "every_hours" : "every_minutes") << "\":" << n << "}";
            r.spec.recur_json   = js.str();
            r.spec.next_fire_at = now + n * (is_hour ? 3600 : 60);
            std::ostringstream oss;
            oss << "every " << n << " " << (is_hour ? "hours" : "minutes");
            r.spec.normalized = oss.str();
            return r;
        }
    }

    // ── "every (day|daily) [at HH:MM]" ──────────────────────────────────
    {
        std::regex re(R"(^(?:every\s+day|daily)(?:\s+at\s+(\d{1,2}:\d{2}))?$)");
        std::smatch m;
        if (std::regex_match(phrase, m, re)) {
            int hh = 9, mm = 0;
            if (m[1].matched && !parse_hhmm(m[1].str(), hh, mm)) {
                r.error.message = "invalid HH:MM";
                return r;
            }
            std::ostringstream js;
            js << "{\"every\":\"day\",\"at\":\"" << two(hh) << ":" << two(mm) << "\"}";
            r.ok                = true;
            r.spec.kind         = ScheduleSpec::Kind::Recurring;
            r.spec.recur_json   = js.str();
            r.spec.next_fire_at = next_local_at(now, hh, mm);
            std::ostringstream oss;
            oss << "every day at " << two(hh) << ":" << two(mm);
            r.spec.normalized = oss.str();
            return r;
        }
    }

    // ── "every (week|weekly) [on <weekday>] [at HH:MM]" ─────────────────
    // ── "every <weekday> [at HH:MM]" ────────────────────────────────────
    {
        std::regex re_weekly(
            R"(^(?:every\s+week|weekly)(?:\s+on\s+([a-z]+))?(?:\s+at\s+(\d{1,2}:\d{2}))?$)");
        std::regex re_wd(
            R"(^every\s+(sun|mon|tue|wed|thu|fri|sat|sunday|monday|tuesday|wednesday|thursday|friday|saturday)(?:\s+at\s+(\d{1,2}:\d{2}))?$)");
        std::smatch m;
        std::string wd_str;
        std::string time_str;
        if (std::regex_match(phrase, m, re_weekly)) {
            if (m[1].matched) wd_str = m[1].str();
            if (m[2].matched) time_str = m[2].str();
        } else if (std::regex_match(phrase, m, re_wd)) {
            wd_str = m[1].str();
            if (m[2].matched) time_str = m[2].str();
        } else {
            wd_str.clear();
        }
        if (!wd_str.empty() || phrase == "every week" || phrase == "weekly") {
            int wd = wd_str.empty() ? 1 /* Monday */ : weekday_to_int(wd_str);
            if (wd < 0) {
                r.error.message = "unrecognised weekday: " + wd_str;
                return r;
            }
            int hh = 9, mm = 0;
            if (!time_str.empty() && !parse_hhmm(time_str, hh, mm)) {
                r.error.message = "invalid HH:MM";
                return r;
            }
            std::ostringstream js;
            js << "{\"every\":\"week\",\"day\":\""
               << lower(std::string(weekday_short(wd))) << "\",\"at\":\""
               << two(hh) << ":" << two(mm) << "\"}";
            r.ok                = true;
            r.spec.kind         = ScheduleSpec::Kind::Recurring;
            r.spec.recur_json   = js.str();
            r.spec.next_fire_at = next_local_weekday_at(now, wd, hh, mm);
            std::ostringstream oss;
            oss << "every " << weekday_short(wd) << " at " << two(hh) << ":" << two(mm);
            r.spec.normalized = oss.str();
            return r;
        }
    }

    r.error.message = "unrecognised schedule phrase";
    return r;
}

int64_t next_fire_for_recur(const std::string& recur_json, int64_t after) {
    if (recur_json.empty()) return 0;

    int64_t ev_min = parse_json_int_field(recur_json, "every_minutes");
    if (ev_min > 0) return after + ev_min * 60;
    int64_t ev_hr = parse_json_int_field(recur_json, "every_hours");
    if (ev_hr > 0) return after + ev_hr * 3600;

    std::string every = parse_json_str_field(recur_json, "every");
    std::string at    = parse_json_str_field(recur_json, "at");
    int hh = 9, mm = 0;
    if (!at.empty() && !parse_hhmm(at, hh, mm)) return 0;

    if (every == "hour") return after + 3600;
    if (every == "day")  return next_local_at(after, hh, mm);
    if (every == "week") {
        std::string day = parse_json_str_field(recur_json, "day");
        int wd = weekday_to_int(day);
        if (wd < 0) return 0;
        return next_local_weekday_at(after, wd, hh, mm);
    }
    return 0;
}

std::string schedule_parser_help() {
    return
        "/schedule accepts:\n"
        "  in N (minute|hour|day|week)[s]\n"
        "  at HH:MM\n"
        "  tomorrow [at HH:MM]\n"
        "  on YYYY-MM-DD [at HH:MM]\n"
        "  every hour | hourly\n"
        "  every N (minute|hour)[s]\n"
        "  every day [at HH:MM] | daily\n"
        "  every week [on <weekday>] [at HH:MM] | weekly\n"
        "  every (Mon|Tue|...) [at HH:MM]";
}

} // namespace arbiter
