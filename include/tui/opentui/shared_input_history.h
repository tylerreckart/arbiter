#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arbiter::opentui {

// Input history shared live across every pane's editor.  Panes used to get
// a copy of the history file at spawn, so a command typed in one pane never
// showed up in another's up-arrow history until the next run merged the
// copies back to disk.  One shared, mutex-guarded store makes history
// append-once/visible-everywhere; the editors index into it via the
// accessors below.
class SharedInputHistory {
public:
    void set_max(int n) {
        std::lock_guard<std::mutex> lk(mu_);
        max_ = n > 0 ? n : 1;
        trim_locked();
    }

    // Appends unless it duplicates the newest entry (readline behavior).
    void add(const std::string& line) {
        if (line.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (!items_.empty() && items_.back() == line) return;
        items_.push_back(line);
        trim_locked();
    }

    // Replace the whole list (initial load from the history file).
    void replace(std::vector<std::string> items) {
        std::lock_guard<std::mutex> lk(mu_);
        items_ = std::move(items);
        trim_locked();
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return items_;
    }

    [[nodiscard]] int size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<int>(items_.size());
    }

    // Bounds-checked element access (0 = oldest); "" when out of range.
    [[nodiscard]] std::string at(int idx) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (idx < 0 || idx >= static_cast<int>(items_.size())) return {};
        return items_[static_cast<size_t>(idx)];
    }

    // Newest match at or below `from_idx` whose entry contains `needle`
    // case-insensitively; -1 when nothing matches.  Drives Ctrl-R.
    [[nodiscard]] int rfind(const std::string& needle, int from_idx) const {
        if (needle.empty()) return -1;
        std::string n = lowered(needle);
        std::lock_guard<std::mutex> lk(mu_);
        int i = std::min(from_idx, static_cast<int>(items_.size()) - 1);
        for (; i >= 0; --i) {
            if (lowered(items_[static_cast<size_t>(i)]).find(n) != std::string::npos) {
                return i;
            }
        }
        return -1;
    }

private:
    static std::string lowered(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    void trim_locked() {
        while (static_cast<int>(items_.size()) > max_) items_.erase(items_.begin());
    }

    mutable std::mutex mu_;
    std::vector<std::string> items_;
    int max_ = 1000;
};

} // namespace arbiter::opentui
