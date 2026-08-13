// arbiter/src/reconcile.cpp — see include/reconcile.h

#include "reconcile.h"
#include "workspace_root.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace arbiter {

namespace {

constexpr std::size_t kMaxInvariants     = 32;
constexpr std::size_t kMaxTargetJson     = 64 * 1024;
constexpr std::size_t kMaxWalkFiles      = 2000;
constexpr std::size_t kMaxReadBytes      = 64 * 1024;
constexpr std::size_t kMaxVerifyLog      = 32 * 1024;
constexpr int         kVerifyTimeoutSec  = 120;
constexpr std::size_t kMaxFilesChanged   = 256;

const std::unordered_set<std::string> kSkipDirNames = {
    ".git", "node_modules", "target", "dist", "build", ".cache",
    ".arbiter-reconcile-snapshots", "__pycache__", ".venv", "venv",
};

struct NamedSpec {
    std::string name;
    std::string checker;  // file.exists | workspace.mentions
    std::string arg;
    std::vector<std::string> cues;  // extra mentions
};

const std::vector<NamedSpec>& catalog() {
    static const std::vector<NamedSpec> k = {
        {"require_two_factor_auth_prompt", "workspace.mentions",
         "two_factor", {"2fa", "totp", "mfa", "two-factor", "two_factor"}},
        {"require_authentication", "workspace.mentions",
         "auth", {"login", "password", "oauth", "session"}},
        {"require_readme", "file.exists", "README.md", {}},
        {"require_license", "file.exists", "LICENSE", {}},
    };
    return k;
}

const NamedSpec* find_named(const std::string& name) {
    for (const auto& n : catalog()) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

std::shared_ptr<JsonValue> parse_target(const std::string& json, std::string* err) {
    if (json.empty()) {
        if (err) *err = "target_state is required";
        return nullptr;
    }
    if (json.size() > kMaxTargetJson) {
        if (err) *err = "target_state exceeds 64 KiB";
        return nullptr;
    }
    try {
        auto v = json_parse(json);
        if (!v || !v->is_object()) {
            if (err) *err = "target_state must be a JSON object";
            return nullptr;
        }
        return v;
    } catch (const std::exception& e) {
        if (err) *err = std::string("target_state JSON: ") + e.what();
        return nullptr;
    }
}

bool json_number(const std::shared_ptr<JsonValue>& v, double* out) {
    if (!v) return false;
    if (v->is_number()) { *out = v->as_number(); return true; }
    if (v->is_string()) {
        try {
            size_t idx = 0;
            double d = std::stod(v->as_string(), &idx);
            if (idx == v->as_string().size()) { *out = d; return true; }
        } catch (...) {}
    }
    return false;
}

bool eval_expr(const ReconcileInvariant& inv,
               const std::shared_ptr<JsonValue>& target,
               std::string* detail) {
    if (!target || !target->is_object()) {
        if (detail) *detail = "no target_state";
        return false;
    }
    auto v = target->get(inv.field);
    if (!v) {
        if (detail) *detail = "missing field " + inv.field;
        return false;
    }
    auto fail = [&](const std::string& m) {
        if (detail) *detail = m;
        return false;
    };
    if (inv.op == "==" || inv.op == "!=") {
        bool eq = false;
        if (v->is_string()) {
            eq = v->as_string() == inv.value;
        } else if (v->is_bool()) {
            bool want = (inv.value == "true" || inv.value == "1");
            bool refuse = (inv.value == "false" || inv.value == "0");
            if (!want && !refuse) return fail("bool compare needs true/false");
            eq = v->as_bool() == want;
        } else {
            double lhs = 0, rhs = 0;
            if (!json_number(v, &lhs)) return fail("incomparable field");
            try { rhs = std::stod(inv.value); }
            catch (...) { return fail("incomparable literal"); }
            eq = std::fabs(lhs - rhs) < 1e-9;
        }
        bool ok = (inv.op == "==") ? eq : !eq;
        if (detail) *detail = ok ? "holds" : "does not hold";
        return ok;
    }
    double lhs = 0, rhs = 0;
    if (!json_number(v, &lhs)) return fail("numeric compare needs a number");
    try { rhs = std::stod(inv.value); }
    catch (...) { return fail("numeric compare needs a number literal"); }
    bool ok = false;
    if      (inv.op == "<=") ok = lhs <= rhs + 1e-12;
    else if (inv.op == ">=") ok = lhs >= rhs - 1e-12;
    else if (inv.op == "<")  ok = lhs <  rhs - 1e-12;
    else if (inv.op == ">")  ok = lhs >  rhs + 1e-12;
    else return fail("unknown operator");
    if (detail) *detail = ok ? "holds" : "does not hold";
    return ok;
}

bool skip_dir(const fs::path& p) {
    std::string name = p.filename().string();
    if (name.size() > 1 && name[0] == '.' && name != ".arbiter") return true;
    return kSkipDirNames.count(name) > 0;
}

// Case-insensitive file exists under root (exact relative path, or
// any filename match for basename-only args like README.md).
bool file_exists_under(const std::string& root, const std::string& rel) {
    std::error_code ec;
    fs::path want = fs::path(rel);
    fs::path direct = fs::path(root) / want;
    if (fs::exists(direct, ec) && fs::is_regular_file(direct, ec)) return true;
    std::string want_l = to_lower(want.filename().string());
    std::size_t n = 0;
    try {
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (it->is_directory(ec)) {
                if (skip_dir(it->path())) { it.disable_recursion_pending(); continue; }
            }
            if (!it->is_regular_file(ec)) continue;
            if (++n > kMaxWalkFiles) break;
            if (to_lower(it->path().filename().string()) == want_l) return true;
        }
    } catch (...) {}
    return false;
}

bool workspace_mentions(const std::string& root,
                        const std::vector<std::string>& cues) {
    if (cues.empty()) return false;
    std::vector<std::string> lc;
    lc.reserve(cues.size());
    for (const auto& c : cues) lc.push_back(to_lower(c));
    std::error_code ec;
    std::size_t n = 0;
    try {
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (it->is_directory(ec)) {
                if (skip_dir(it->path())) { it.disable_recursion_pending(); continue; }
            }
            if (!it->is_regular_file(ec)) continue;
            if (++n > kMaxWalkFiles) break;
            auto sz = it->file_size(ec);
            if (ec || sz == 0 || sz > kMaxReadBytes) continue;
            std::ifstream in(it->path(), std::ios::binary);
            if (!in) continue;
            std::string buf(static_cast<std::size_t>(sz), '\0');
            in.read(buf.data(), static_cast<std::streamsize>(sz));
            // Skip obvious binaries.
            if (buf.find('\0') != std::string::npos) continue;
            std::string low = to_lower(buf);
            for (const auto& cue : lc) {
                if (low.find(cue) != std::string::npos) return true;
            }
        }
    } catch (...) {}
    return false;
}

bool copy_tree(const fs::path& from, const fs::path& to, std::string* err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        if (err) *err = "cannot create snapshot dir: " + ec.message();
        return false;
    }
    try {
        for (auto it = fs::recursive_directory_iterator(
                 from, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            fs::path rel = fs::relative(it->path(), from, ec);
            if (ec) continue;
            if (rel.empty()) continue;
            if (rel.begin() != rel.end() &&
                rel.begin()->string() == ".arbiter-reconcile-snapshots") {
                it.disable_recursion_pending();
                continue;
            }
            fs::path dest = to / rel;
            if (it->is_directory(ec)) {
                fs::create_directories(dest, ec);
                if (ec) {
                    if (err) *err = "cannot create snapshot dir: " + ec.message();
                    return false;
                }
            } else if (it->is_regular_file(ec)) {
                fs::create_directories(dest.parent_path(), ec);
                if (ec) {
                    if (err) *err = "cannot create snapshot dir: " + ec.message();
                    return false;
                }
                fs::copy_file(it->path(), dest,
                              fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    if (err) *err = "snapshot copy failed: " + ec.message();
                    return false;
                }
            }
        }
        if (ec) {
            if (err) *err = "snapshot walk failed: " + ec.message();
            return false;
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    return true;
}

struct HostShellOutcome {
    int         exit_code = -1;
    std::string log;
    bool        timed_out = false;
    bool        canceled = false;
    bool        spawn_failed = false;
};

// Run `cmd` under /bin/sh with cwd=`dir`.  Avoids interpolating `dir`
// into a shell string (path metacharacters cannot inject commands).
// Parent enforces timeout and polls `cancel` to SIGKILL the child.
HostShellOutcome run_host_shell_in_dir(const std::string& dir,
                                       const std::string& cmd,
                                       std::size_t log_cap,
                                       int timeout_sec,
                                       std::atomic<bool>* cancel) {
    HostShellOutcome out;
    std::string shell_cmd = cmd;
    if (fs::exists("/usr/bin/timeout") && timeout_sec > 0) {
        shell_cmd = "/usr/bin/timeout --kill-after=5 " +
                    std::to_string(timeout_sec) + " " + cmd;
    }

    int pipe_fd[2] = {-1, -1};
    if (::pipe(pipe_fd) != 0) {
        out.spawn_failed = true;
        return out;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        out.spawn_failed = true;
        return out;
    }

    if (pid == 0) {
        while (::dup2(pipe_fd[1], STDOUT_FILENO) < 0 && errno == EINTR) {}
        while (::dup2(pipe_fd[1], STDERR_FILENO) < 0 && errno == EINTR) {}
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        if (::chdir(dir.c_str()) != 0) ::_exit(127);
        ::execl("/bin/sh", "sh", "-c", shell_cmd.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(pipe_fd[1]);
    int read_fd = pipe_fd[0];
    int flags = ::fcntl(read_fd, F_GETFL);
    if (flags >= 0) ::fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

    auto start = std::chrono::steady_clock::now();
    auto deadline = (timeout_sec > 0)
        ? start + std::chrono::seconds(timeout_sec)
        : std::chrono::steady_clock::time_point::max();

    char buf[4096];
    bool eof = false;
    while (!eof) {
        if (cancel && cancel->load()) {
            out.canceled = true;
            ::kill(pid, SIGKILL);
            break;
        }
        int poll_ms = -1;
        if (timeout_sec > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                out.timed_out = true;
                ::kill(pid, SIGKILL);
                break;
            }
            poll_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count());
            if (poll_ms < 0) poll_ms = 0;
            if (poll_ms > 250) poll_ms = 250;
        } else {
            poll_ms = 250;
        }

        pollfd pfd{read_fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, poll_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) {
            int status_check = 0;
            pid_t r = ::waitpid(pid, &status_check, WNOHANG);
            if (r == pid) {
                int fl = ::fcntl(read_fd, F_GETFL);
                if (fl >= 0) ::fcntl(read_fd, F_SETFL, fl & ~O_NONBLOCK);
                ssize_t k;
                while ((k = ::read(read_fd, buf, sizeof(buf))) > 0) {
                    if (out.log.size() < log_cap) {
                        std::size_t room = log_cap - out.log.size();
                        out.log.append(buf, std::min(room, static_cast<std::size_t>(k)));
                    }
                }
                ::close(read_fd);
                if (WIFEXITED(status_check)) out.exit_code = WEXITSTATUS(status_check);
                else if (WIFSIGNALED(status_check))
                    out.exit_code = 128 + WTERMSIG(status_check);
                else out.exit_code = status_check;
                return out;
            }
            continue;
        }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t k = ::read(read_fd, buf, sizeof(buf));
            if (k > 0) {
                if (out.log.size() < log_cap) {
                    std::size_t room = log_cap - out.log.size();
                    out.log.append(buf, std::min(room, static_cast<std::size_t>(k)));
                }
                continue;
            }
            if (k == 0) { eof = true; break; }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) break;
    }

    ::close(read_fd);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }
    if (out.canceled) {
        out.exit_code = -1;
        return out;
    }
    if (out.timed_out) {
        out.exit_code = 124;
        return out;
    }
#ifdef WIFEXITED
    if (WIFEXITED(status)) out.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) out.exit_code = 128 + WTERMSIG(status);
    else out.exit_code = status;
#else
    out.exit_code = status;
#endif
    return out;
}

void clear_dir_contents(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> kids;
    for (auto it = fs::directory_iterator(root, ec);
         it != fs::directory_iterator(); ++it) {
        if (it->path().filename() == ".arbiter-reconcile-snapshots") continue;
        kids.push_back(it->path());
    }
    for (const auto& p : kids) fs::remove_all(p, ec);
}

std::vector<std::string> list_rel_files(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    std::size_t n = 0;
    try {
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (it->is_directory(ec)) {
                if (skip_dir(it->path()) ||
                    it->path().filename() == ".arbiter-reconcile-snapshots") {
                    it.disable_recursion_pending();
                    continue;
                }
            }
            if (!it->is_regular_file(ec)) continue;
            if (++n > kMaxWalkFiles) break;
            fs::path rel = fs::relative(it->path(), root, ec);
            if (!ec) out.push_back(rel.generic_string());
        }
    } catch (...) {}
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> diff_files(const std::vector<std::string>& before,
                                    const std::vector<std::string>& after) {
    std::unordered_set<std::string> b(before.begin(), before.end());
    std::vector<std::string> out;
    for (const auto& p : after) {
        if (!b.count(p)) {
            out.push_back(p);
            if (out.size() >= kMaxFilesChanged) break;
        }
    }
    return out;
}

std::shared_ptr<JsonValue> clause_to_json(const StateClause& c) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"] = jstr(c.id);
    m["checker"] = jstr(c.checker);
    if (!c.arg.empty()) m["arg"] = jstr(c.arg);
    if (!c.agent.empty()) m["agent"] = jstr(c.agent);
    return o;
}

std::shared_ptr<JsonValue> clause_result_to_json(const ClauseResult& r) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"] = jstr(r.id);
    m["checker"] = jstr(r.checker);
    m["satisfied"] = jbool(r.satisfied);
    if (!r.detail.empty()) m["detail"] = jstr(r.detail);
    return o;
}

}  // namespace

bool named_invariant_known(const std::string& name) {
    return find_named(name) != nullptr;
}

const std::vector<std::string>& named_invariant_catalog() {
    static std::vector<std::string> names;
    if (names.empty()) {
        for (const auto& n : catalog()) names.push_back(n.name);
    }
    return names;
}

std::optional<ReconcileInvariant>
parse_invariant(const std::string& raw, std::string* err) {
    std::string s = trim(raw);
    if (s.empty()) {
        if (err) *err = "empty invariant";
        return std::nullopt;
    }
    // Named: [a-z][a-z0-9_]* with at least one underscore (capability style)
    // or a known catalog name.  Expr contains an operator.
    static const char* ops[] = {"<=", ">=", "==", "!=", "<", ">"};
    const char* found_op = nullptr;
    size_t op_pos = std::string::npos;
    for (const char* op : ops) {
        size_t p = s.find(op);
        if (p != std::string::npos && (op_pos == std::string::npos || p < op_pos)) {
            op_pos = p;
            found_op = op;
        }
    }
    if (!found_op) {
        for (char c : s) {
            if (!is_ident(c)) {
                if (err) *err = "malformed invariant: " + s;
                return std::nullopt;
            }
        }
        ReconcileInvariant inv;
        inv.tier = ReconcileInvariant::Tier::Named;
        inv.raw = s;
        return inv;
    }
    ReconcileInvariant inv;
    inv.tier = ReconcileInvariant::Tier::Expr;
    inv.raw = s;
    inv.op = found_op;
    inv.field = trim(s.substr(0, op_pos));
    inv.value = trim(s.substr(op_pos + std::strlen(found_op)));
    if (inv.field.empty() || inv.value.empty()) {
        if (err) *err = "malformed expression: " + s;
        return std::nullopt;
    }
    for (char c : inv.field) {
        if (!is_ident(c)) {
            if (err) *err = "invalid field in expression: " + inv.field;
            return std::nullopt;
        }
    }
    if (inv.value.size() >= 2 &&
        ((inv.value.front() == '"' && inv.value.back() == '"') ||
         (inv.value.front() == '\'' && inv.value.back() == '\''))) {
        inv.value = inv.value.substr(1, inv.value.size() - 2);
        if (inv.op != "==" && inv.op != "!=") {
            if (err) *err = "string literals only valid with == or !=";
            return std::nullopt;
        }
        return inv;
    }
    // Numeric (or bool/ident for ==/!=).
    bool numeric = !inv.value.empty();
    size_t i = 0;
    if (inv.value[0] == '-' || inv.value[0] == '+') ++i;
    bool saw_digit = false, saw_dot = false;
    for (; i < inv.value.size(); ++i) {
        char c = inv.value[i];
        if (std::isdigit(static_cast<unsigned char>(c))) { saw_digit = true; continue; }
        if (c == '.' && !saw_dot) { saw_dot = true; continue; }
        numeric = false;
        break;
    }
    if (numeric && saw_digit && i == inv.value.size()) return inv;
    if (inv.op == "==" || inv.op == "!=") {
        for (char c : inv.value) {
            if (!is_ident(c)) {
                if (err) *err = "malformed expression value: " + inv.value;
                return std::nullopt;
            }
        }
        return inv;
    }
    if (err) *err = "malformed expression value: " + inv.value;
    return std::nullopt;
}

std::optional<AdmitError> admit_reconcile(const ReconcileSpec& spec) {
    AdmitError e;
    if (spec.mode != "observe" && spec.mode != "ensure") {
        e.code = "bad_mode";
        e.message = "mode must be observe or ensure";
        return e;
    }
    if (spec.workspace.root.empty()) {
        e.code = "bad_workspace";
        e.message = "workspace root is required";
        return e;
    }
    if (spec.workspace.kind != "sandbox" && spec.workspace.kind != "path") {
        e.code = "bad_workspace";
        e.message = "workspace.kind must be sandbox or path";
        return e;
    }
    std::string werr;
    if (canonical_workspace_root(spec.workspace.root, &werr).empty()) {
        e.code = "bad_workspace";
        e.message = werr.empty() ? "workspace root is not a directory" : werr;
        return e;
    }
    if (spec.invariants.size() > kMaxInvariants) {
        e.code = "too_many_invariants";
        e.message = "at most 32 invariants";
        return e;
    }
    std::string jerr;
    auto target = parse_target(spec.target_state_json, &jerr);
    if (!target) {
        e.code = "bad_target_state";
        e.message = jerr;
        return e;
    }
    for (const auto& raw : spec.invariants) {
        std::string perr;
        auto inv = parse_invariant(raw, &perr);
        if (!inv) {
            e.code = "bad_invariant";
            e.message = perr;
            return e;
        }
        if (inv->tier == ReconcileInvariant::Tier::Named) {
            if (!named_invariant_known(inv->raw)) {
                e.code = "unknown_invariant";
                e.message = "unknown named invariant: " + inv->raw;
                return e;
            }
        } else {
            std::string detail;
            if (!eval_expr(*inv, target, &detail) && detail != "holds") {
                // Contradictory desired state (e.g. amountUSD <= 10 but
                // target_state.amountUSD is 99).  Missing field also fails.
                e.code = "contradictory_invariant";
                e.message = inv->raw + " does not hold on target_state (" +
                            detail + ")";
                return e;
            }
        }
    }
    if (!spec.verification.command.empty() &&
        spec.verification.command != "auto" &&
        !verification_command_is_safe(spec.verification.command)) {
        e.code = "unsafe_verification";
        e.message = "verification.command contains disallowed shell characters";
        return e;
    }
    return std::nullopt;
}

StateContract compile_intent_contract(const ReconcileSpec& spec,
                                      const std::string& contract_id) {
    StateContract c;
    c.id = contract_id.empty() ? "intent" : contract_id;
    c.max_waves = spec.max_waves;
    c.max_wall_ms = spec.max_wall_ms;

    std::string jerr;
    auto target = parse_target(spec.target_state_json, &jerr);

    if (target) {
        std::string system = target->get_string("system", "");
        if (!system.empty()) {
            StateClause cl;
            cl.id = "system";
            cl.checker = "workspace.mentions";
            cl.arg = system;
            cl.agent = "backend";
            c.clauses.push_back(std::move(cl));
        }
        std::string status = target->get_string("status", "");
        if (!status.empty()) {
            StateClause cl;
            cl.id = "status";
            cl.checker = "workspace.mentions";
            cl.arg = status;
            cl.agent = "backend";
            c.clauses.push_back(std::move(cl));
        }
    }

    int n = 0;
    for (const auto& raw : spec.invariants) {
        std::string perr;
        auto inv = parse_invariant(raw, &perr);
        if (!inv) continue;
        StateClause cl;
        cl.id = "inv-" + std::to_string(++n);
        if (inv->tier == ReconcileInvariant::Tier::Expr) {
            cl.checker = "expr.holds";
            cl.arg = inv->raw;
        } else if (const NamedSpec* ns = find_named(inv->raw)) {
            cl.checker = ns->checker;
            cl.arg = ns->arg;
            cl.agent = "backend";
        }
        c.clauses.push_back(std::move(cl));
    }

    if (spec.verification.require_tests) {
        StateClause cl;
        cl.id = "tests-pass";
        cl.checker = "verification.pass";
        cl.arg = spec.verification.command.empty() ? "auto"
                                                   : spec.verification.command;
        cl.agent = "devops";
        c.clauses.push_back(std::move(cl));
    }
    return c;
}

DeltaS observe_contract(const StateContract& contract,
                        const ReconcileSpec& spec) {
    DeltaS d;
    std::string jerr;
    auto target = parse_target(spec.target_state_json, &jerr);
    const std::string& root = spec.workspace.root;

    for (const auto& cl : contract.clauses) {
        ClauseResult r;
        r.id = cl.id;
        r.checker = cl.checker;
        if (cl.checker == "expr.holds") {
            std::string perr;
            auto inv = parse_invariant(cl.arg, &perr);
            if (!inv || inv->tier != ReconcileInvariant::Tier::Expr) {
                r.satisfied = false;
                r.detail = perr.empty() ? "bad expr" : perr;
            } else {
                r.satisfied = eval_expr(*inv, target, &r.detail);
            }
        } else if (cl.checker == "file.exists") {
            r.satisfied = file_exists_under(root, cl.arg);
            r.detail = r.satisfied ? ("found " + cl.arg) : ("missing " + cl.arg);
        } else if (cl.checker == "workspace.mentions") {
            std::vector<std::string> cues{cl.arg};
            if (const NamedSpec* ns = find_named(cl.arg)) {
                cues = ns->cues;
                if (cues.empty()) cues.push_back(ns->arg);
            } else if (cl.id == "inv-" + cl.arg) {
                // not used
            }
            // Named clauses store the catalog arg (e.g. "two_factor");
            // system/status store the target_state value.  Also fold in
            // catalog cues when the clause came from a named invariant.
            for (const auto& ns : catalog()) {
                if (ns.arg == cl.arg || ns.name == cl.arg) {
                    cues.insert(cues.end(), ns.cues.begin(), ns.cues.end());
                }
            }
            r.satisfied = workspace_mentions(root, cues);
            r.detail = r.satisfied ? ("mentions " + cl.arg)
                                   : ("no mention of " + cl.arg);
        } else if (cl.checker == "verification.pass") {
            // Observe does not run tests — the runner stamps this later.
            r.satisfied = false;
            r.detail = "pending verification";
        } else if (cl.checker == "named.capability") {
            r.satisfied = workspace_mentions(root, {cl.arg});
            r.detail = r.satisfied ? "capability present" : "capability missing";
        } else {
            r.satisfied = false;
            r.detail = "unknown checker";
        }
        if (r.satisfied) d.held.push_back(std::move(r));
        else             d.residual.push_back(std::move(r));
    }
    return d;
}

std::string detect_test_command(const std::string& workspace_root) {
    std::error_code ec;
    auto has = [&](const std::string& name) {
        return fs::exists(fs::path(workspace_root) / name, ec);
    };
    if (has("package.json")) return "npm test";
    if (has("pytest.ini") || has("pyproject.toml") || has("setup.cfg"))
        return "python -m pytest";
    if (has("Cargo.toml")) return "cargo test";
    if (has("go.mod")) return "go test ./...";
    if (has("CMakeLists.txt")) return "ctest --output-on-failure";
    if (has("Makefile") || has("makefile")) return "make test";
    // Heuristic: a tests/ dir with python files.
    fs::path tests = fs::path(workspace_root) / "tests";
    if (fs::is_directory(tests, ec)) {
        try {
            for (auto it = fs::directory_iterator(tests, ec);
                 it != fs::directory_iterator(); ++it) {
                auto ext = it->path().extension().string();
                if (ext == ".py") return "python -m pytest";
                if (ext == ".js" || ext == ".mjs" || ext == ".ts")
                    return "npm test";
            }
        } catch (...) {}
    }
    return {};
}

bool verification_command_is_safe(const std::string& command) {
    if (command.empty() || command.size() > 512) return false;
    for (unsigned char c : command) {
        if (c < 32) return false;
        switch (c) {
            case '$': case '`': case ';': case '|': case '&':
            case '>': case '<': case '(': case ')':
            case '{': case '}': case '\\': case '\n': case '\r':
                return false;
            default: break;
        }
    }
    return true;
}

VerificationEvidence run_verification(const ReconcileSpec& spec,
                                      const ReconcileHooks& hooks) {
    VerificationEvidence ev;
    std::atomic<bool>* cancel = hooks.cancel;
    if (!spec.verification.require_tests) {
        ev.reason = "skipped";
        ev.passed = true;
        return ev;
    }
    std::string cmd = spec.verification.command;
    if (cmd.empty() || cmd == "auto") {
        cmd = detect_test_command(spec.workspace.root);
        if (cmd.empty()) {
            ev.reason = "undetectable";
            ev.passed = false;
            return ev;
        }
    }
    if (!verification_command_is_safe(cmd)) {
        ev.reason = "unsafe";
        ev.command = cmd;
        return ev;
    }
    ev.command = cmd;
    if (cancel && cancel->load()) {
        ev.reason = "canceled";
        return ev;
    }

    if (spec.workspace.kind == "sandbox") {
        if (!hooks.verify_exec) {
            ev.reason = "sandbox_unavailable";
            return ev;
        }
        ev.ran = true;
        VerificationEvidence sandbox_ev = hooks.verify_exec(cmd, cancel);
        if (sandbox_ev.command.empty()) sandbox_ev.command = cmd;
        return sandbox_ev;
    }

    ev.ran = true;
    HostShellOutcome run = run_host_shell_in_dir(
        spec.workspace.root, cmd, kMaxVerifyLog, kVerifyTimeoutSec, cancel);
    ev.log = std::move(run.log);
    if (run.spawn_failed) {
        ev.exit_code = -1;
        ev.reason = "spawn_failed";
        return ev;
    }
    if (run.canceled || (cancel && cancel->load())) {
        ev.exit_code = -1;
        ev.reason = "canceled";
        return ev;
    }
    ev.exit_code = run.exit_code;
    if (run.timed_out || ev.exit_code == 124) {
        ev.passed = false;
        ev.reason = "timeout";
        return ev;
    }
    ev.passed = (ev.exit_code == 0);
    ev.reason = ev.passed ? "passed" : "failed";
    return ev;
}

bool snapshot_workspace(const std::string& root,
                        const std::string& dest,
                        std::string* err) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (err) *err = "snapshot source is not a directory";
        return false;
    }
    fs::remove_all(dest, ec);
    return copy_tree(root, dest, err);
}

bool restore_workspace(const std::string& snapshot,
                       const std::string& root,
                       std::string* err) {
    std::error_code ec;
    if (!fs::is_directory(snapshot, ec)) {
        if (err) *err = "snapshot is missing";
        return false;
    }
    if (!fs::is_directory(root, ec)) {
        if (err) *err = "workspace root is missing";
        return false;
    }
    clear_dir_contents(root);
    return copy_tree(snapshot, root, err);
}

std::string format_reconcile_brief(const StateContract& c,
                                   const ReconcileSpec& spec) {
    std::ostringstream os;
    os << "Reconcile the workspace so target_state is true, invariants hold, "
          "and tests pass. Do not claim success without green verification.\n";
    os << "target_state: " << spec.target_state_json << "\n";
    if (!spec.invariants.empty()) {
        os << "invariants:\n";
        for (const auto& i : spec.invariants) os << "  - " << i << "\n";
    }
    os << "clauses:\n";
    for (const auto& cl : c.clauses) {
        os << "  - " << cl.id << " [" << cl.checker << "]";
        if (!cl.arg.empty()) os << " " << cl.arg;
        os << "\n";
    }
    os << "Write application code and tests in-tree. Prefer /write and /diff.";
    return os.str();
}

std::shared_ptr<JsonValue> contract_to_json(const StateContract& c) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["id"] = jstr(c.id);
    m["version"] = jnum(c.version);
    auto arr = jarr();
    for (const auto& cl : c.clauses)
        arr->as_array_mut().push_back(clause_to_json(cl));
    m["clauses"] = arr;
    auto b = jobj();
    b->as_object_mut()["max_waves"] = jnum(c.max_waves);
    b->as_object_mut()["max_wall_ms"] = jnum(static_cast<double>(c.max_wall_ms));
    m["budgets"] = b;
    return o;
}

std::shared_ptr<JsonValue> delta_to_json(const DeltaS& d) {
    auto o = jobj();
    auto res = jarr();
    for (const auto& r : d.residual)
        res->as_array_mut().push_back(clause_result_to_json(r));
    auto held = jarr();
    for (const auto& r : d.held)
        held->as_array_mut().push_back(clause_result_to_json(r));
    o->as_object_mut()["residual"] = res;
    o->as_object_mut()["held"] = held;
    o->as_object_mut()["empty"] = jbool(d.empty());
    return o;
}

std::shared_ptr<JsonValue> result_to_json(const ReconcileResult& r) {
    auto o = jobj();
    auto& m = o->as_object_mut();
    m["status"] = jstr(r.status);
    if (!r.reason.empty()) m["reason"] = jstr(r.reason);
    m["contract"] = contract_to_json(r.contract);
    m["delta"] = delta_to_json(r.delta);
    auto ev = jobj();
    auto& em = ev->as_object_mut();
    em["ran"] = jbool(r.verification.ran);
    em["passed"] = jbool(r.verification.passed);
    if (!r.verification.command.empty()) em["command"] = jstr(r.verification.command);
    em["exit_code"] = jnum(r.verification.exit_code);
    if (!r.verification.reason.empty()) em["reason"] = jstr(r.verification.reason);
    if (!r.verification.log.empty()) em["log"] = jstr(r.verification.log);
    m["verification"] = ev;
    auto files = jarr();
    for (const auto& f : r.files_changed) files->as_array_mut().push_back(jstr(f));
    m["files_changed"] = files;
    m["rolled_back"] = jbool(r.rolled_back);
    if (!r.brief.empty()) m["brief"] = jstr(r.brief);
    if (!r.snapshot_path.empty()) m["snapshot_path"] = jstr(r.snapshot_path);
    return o;
}

ReconcileResult run_reconcile(const ReconcileSpec& spec,
                              const ReconcileHooks& hooks) {
    ReconcileResult out;
    auto canceled = [&]() {
        return hooks.cancel && hooks.cancel->load();
    };

    if (auto adm = admit_reconcile(spec)) {
        out.status = "failed";
        out.reason = adm->code;
        out.brief = adm->message;
        return out;
    }

    ReconcileSpec bound = spec;
    std::string werr;
    bound.workspace.root = canonical_workspace_root(spec.workspace.root, &werr);
    if (bound.workspace.root.empty()) {
        out.status = "failed";
        out.reason = "bad_workspace";
        out.brief = werr;
        return out;
    }

    out.contract = compile_intent_contract(bound, "reconcile");
    out.brief = format_reconcile_brief(out.contract, bound);

    auto before = list_rel_files(bound.workspace.root);

    if (bound.rollback_on_failure) {
        fs::path snap = bound.snapshot_dir.empty()
            ? fs::path(bound.workspace.root) / ".arbiter-reconcile-snapshots" / "pre"
            : fs::path(bound.snapshot_dir);
        std::string serr;
        if (!snapshot_workspace(bound.workspace.root, snap.string(), &serr)) {
            out.status = "failed";
            out.reason = "snapshot_failed";
            out.brief = serr;
            return out;
        }
        out.snapshot_path = snap.string();
    }

    auto apply_verify_to_delta = [&](DeltaS& d, const VerificationEvidence& ev) {
        for (auto it = d.residual.begin(); it != d.residual.end(); ) {
            if (it->checker == "verification.pass") {
                it->satisfied = ev.passed;
                it->detail = ev.reason.empty() ? (ev.passed ? "passed" : "failed")
                                               : ev.reason;
                if (it->satisfied) {
                    d.held.push_back(*it);
                    it = d.residual.erase(it);
                    continue;
                }
            }
            ++it;
        }
    };

    auto implementation_residual = [](const DeltaS& d) {
        for (const auto& r : d.residual)
            if (r.checker != "verification.pass") return true;
        return false;
    };

    auto finish_fail = [&](const std::string& reason) {
        if (canceled()) {
            out.status = "canceled";
            out.reason = "canceled";
        } else {
            out.status = "failed";
            out.reason = reason;
        }
        if (bound.rollback_on_failure && !out.snapshot_path.empty()) {
            std::string rerr;
            if (restore_workspace(out.snapshot_path, bound.workspace.root, &rerr)) {
                out.rolled_back = true;
                if (out.status == "failed") out.status = "rolled_back";
            }
        }
        out.files_changed = diff_files(before, list_rel_files(bound.workspace.root));
        return out;
    };

    if (canceled()) return finish_fail("canceled");

    out.delta = observe_contract(out.contract, bound);

    if (implementation_residual(out.delta)) {
        if (bound.mode == "ensure" && hooks.implement) {
            try {
                hooks.implement(out.contract, bound.workspace.root, hooks.cancel);
            } catch (const std::exception& e) {
                out.delta = observe_contract(out.contract, bound);
                return finish_fail(std::string("implement_failed: ") + e.what());
            }
            if (canceled()) return finish_fail("canceled");
            out.delta = observe_contract(out.contract, bound);
        } else if (bound.mode == "ensure" && !hooks.implement) {
            return finish_fail("implement_required");
        } else {
            // observe: residual is a failed (not rolled back unless
            // we mutated — we didn't).
            out.verification.reason = "not_run";
            apply_verify_to_delta(out.delta, out.verification);
            out.status = "failed";
            out.reason = "delta_unresolved";
            out.files_changed = diff_files(before, list_rel_files(bound.workspace.root));
            return out;
        }
    }

    if (canceled()) return finish_fail("canceled");

    if (implementation_residual(out.delta) && bound.mode == "ensure") {
        return finish_fail("delta_unresolved");
    }

    out.verification = run_verification(bound, hooks);
    apply_verify_to_delta(out.delta, out.verification);

    if (canceled()) return finish_fail("canceled");

    if (bound.verification.require_tests && !out.verification.passed) {
        std::string why = out.verification.reason.empty()
            ? "verification_failed" : out.verification.reason;
        if (why == "undetectable") why = "verification_missing";
        return finish_fail(why);
    }

    if (!out.delta.empty()) return finish_fail("delta_unresolved");

    out.status = "satisfied";
    out.reason = "ok";
    out.files_changed = diff_files(before, list_rel_files(bound.workspace.root));
    return out;
}

}  // namespace arbiter
