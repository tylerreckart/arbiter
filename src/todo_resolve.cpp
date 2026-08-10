#include "todo_resolve.h"

#include <cctype>

namespace arbiter {

namespace {

std::string trim_ws(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_numeric_id(const std::string& s, int64_t& out) {
    std::string t = trim_ws(s);
    if (!t.empty() && t.front() == '#') t.erase(0, 1);
    if (t.empty()) return false;
    for (char c : t) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        out = std::stoll(t);
    } catch (...) {
        return false;
    }
    return out > 0;
}

template <typename GetId, typename GetSubject>
int64_t resolve_impl(const std::string& args,
                     size_t n,
                     GetId get_id,
                     GetSubject get_subject,
                     std::string& err_out) {
    err_out.clear();
    int64_t numeric = 0;
    if (parse_numeric_id(args, numeric)) return numeric;

    const std::string needle = trim_ws(args);
    if (needle.empty()) {
        err_out = "missing id or subject";
        return 0;
    }
    const std::string needle_l = lower_ascii(needle);

    int64_t exact = 0;
    int exact_hits = 0;
    int64_t ci_exact = 0;
    int ci_hits = 0;
    int64_t prefix = 0;
    int prefix_hits = 0;

    for (size_t i = 0; i < n; ++i) {
        const int64_t id = get_id(i);
        const std::string& subj = get_subject(i);
        if (subj == needle) {
            exact = id;
            ++exact_hits;
        }
        const std::string subj_l = lower_ascii(subj);
        if (subj_l == needle_l) {
            ci_exact = id;
            ++ci_hits;
        }
        if (subj_l.rfind(needle_l, 0) == 0) {
            prefix = id;
            ++prefix_hits;
        }
    }

    if (exact_hits == 1) return exact;
    if (exact_hits > 1) {
        err_out = "ambiguous subject — use the numeric id";
        return 0;
    }
    if (ci_hits == 1) return ci_exact;
    if (ci_hits > 1) {
        err_out = "ambiguous subject — use the numeric id";
        return 0;
    }
    if (prefix_hits == 1) return prefix;
    if (prefix_hits > 1) {
        err_out = "ambiguous subject — use the numeric id";
        return 0;
    }

    err_out = "no open todo matches that id or subject";
    return 0;
}

} // namespace

int64_t resolve_todo_target(const std::string& args,
                            const std::vector<TenantStore::Todo>& candidates,
                            std::string& err_out) {
    return resolve_impl(
        args, candidates.size(),
        [&](size_t i) { return candidates[i].id; },
        [&](size_t i) -> const std::string& { return candidates[i].subject; },
        err_out);
}

int resolve_todo_subject_ref(const std::string& args,
                             const std::vector<TodoSubjectRef>& candidates,
                             std::string& err_out) {
    const int64_t id = resolve_impl(
        args, candidates.size(),
        [&](size_t i) { return static_cast<int64_t>(candidates[i].id); },
        [&](size_t i) -> const std::string& { return candidates[i].subject; },
        err_out);
    return static_cast<int>(id);
}

} // namespace arbiter
