// arbiter/src/logger.cpp

#include "logger.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

TuiStderrRedirect::TuiStderrRedirect(const std::string& config_dir) {
    if (config_dir.empty()) return;
    path_ = config_dir + "/tui.log";
    // get_config_dir() already creates this; mkdir covers tests and a
    // race where the directory disappeared between startup and the TUI.
    if (::mkdir(config_dir.c_str(), 0700) != 0 && errno != EEXIST) {
        // Still try the open — the directory may already be usable.
    } else {
        ::chmod(config_dir.c_str(), 0700);
    }
    std::fflush(stderr);
    std::fflush(stdout);

    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    struct stat st{};
    if (::stat(path_.c_str(), &st) == 0 && st.st_size > (1024 * 1024)) {
        flags |= O_TRUNC;
    } else {
        flags |= O_APPEND;
    }
    const int log_fd = ::open(path_.c_str(), flags, 0600);
    if (log_fd >= 0) ::fchmod(log_fd, 0600);
    if (log_fd < 0) return;
    saved_fd_ = ::dup(STDERR_FILENO);
    if (saved_fd_ < 0) {
        ::close(log_fd);
        return;
    }
    if (::dup2(log_fd, STDERR_FILENO) < 0) {
        ::close(saved_fd_);
        saved_fd_ = -1;
        ::close(log_fd);
        return;
    }
    if (log_fd != STDERR_FILENO) ::close(log_fd);
    // Drop any stdio buffer that still points at the old device so a
    // later fprintf cannot flush a line onto the tty.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    active_ = true;
}

TuiStderrRedirect::~TuiStderrRedirect() {
    if (!active_) return;
    std::fflush(stderr);
    if (saved_fd_ >= 0) {
        ::dup2(saved_fd_, STDERR_FILENO);
        ::close(saved_fd_);
        saved_fd_ = -1;
    }
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    active_ = false;
}

void emit_tui_log_probe_if_enabled() {
    const char* v = std::getenv("ARBITER_TUI_LOG_PROBE");
    if (!v || !*v || std::strcmp(v, "0") == 0) return;
    // Same events a live turn writes: intent_route / intent_shadow go
    // through Logger, openrouter_response is the historical fprintf.
    Logger::global().info("intent_route", {
        {"tier", "probe"},
        {"source", "probe"},
        {"choice", "index"},
    });
    Logger::global().info("intent_shadow", {
        {"backend", "probe"},
        {"ok", "true"},
    });
    std::fprintf(stderr,
                 "[info] openrouter_response requested_model=%s served_model=%s cost_usd=%s\n",
                 "anthropic/claude-haiku-4-5",
                 "anthropic/claude-haiku-4.5",
                 "0.001197");
    std::fflush(stderr);
}

} // namespace arbiter
