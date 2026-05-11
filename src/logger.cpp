// arbiter/src/logger.cpp

#include "logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

namespace arbiter {

namespace {

std::string rfc3339_now_utc() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms));
    return buf;
}

std::string short_clock_now() {
    auto t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// Escape a string per JSON: backslash, double quote, control bytes.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

} // namespace

Logger& Logger::global() {
    static Logger instance;
    return instance;
}

void Logger::init_from_env() {
    std::lock_guard<std::mutex> lk(mu_);
    if (initialized_) return;
    initialized_ = true;
    const char* e = std::getenv("ARBITER_LOG_FORMAT");
    if (!e || !*e) return;
    if (std::strcmp(e, "json") == 0 || std::strcmp(e, "JSON") == 0) {
        fmt_ = LogFormat::Json;
    } else {
        fmt_ = LogFormat::Human;
    }
}

void Logger::set_format(LogFormat fmt) {
    std::lock_guard<std::mutex> lk(mu_);
    fmt_ = fmt;
    initialized_ = true;
}

LogFormat Logger::format() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_;
}

void Logger::info (const std::string& event, Fields fields) {
    log_impl("info",  event, {fields.begin(), fields.end()});
}
void Logger::warn (const std::string& event, Fields fields) {
    log_impl("warn",  event, {fields.begin(), fields.end()});
}
void Logger::error(const std::string& event, Fields fields) {
    log_impl("error", event, {fields.begin(), fields.end()});
}

void Logger::log_impl(const char* level, const std::string& event,
                       const std::vector<Field>& fields) {
    LogFormat fmt;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fmt = fmt_;
    }
    std::ostringstream line;
    if (fmt == LogFormat::Json) {
        line << "{\"ts\":\""  << rfc3339_now_utc() << "\","
             << "\"level\":\"" << level << "\","
             << "\"event\":\"" << json_escape(event) << "\"";
        for (const auto& [k, v] : fields) {
            line << ",\"" << json_escape(k) << "\":\""
                 << json_escape(v) << "\"";
        }
        line << "}\n";
    } else {
        line << "[" << short_clock_now() << "] [" << level << "] " << event;
        for (const auto& [k, v] : fields) {
            line << ' ' << k << '=' << v;
        }
        line << '\n';
    }
    // Single fprintf so the line lands atomically on stderr; we hold
    // mu_ only briefly above so concurrent emitters don't interleave
    // partial writes.
    std::string s = line.str();
    std::lock_guard<std::mutex> lk(mu_);
    std::fwrite(s.data(), 1, s.size(), stderr);
    std::fflush(stderr);
}

} // namespace arbiter
