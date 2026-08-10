#include "tui/menu.h"

#include "model_catalog.h"

#include <algorithm>
#include <cctype>

namespace arbiter {

namespace {

bool is_selectable(const MenuItem& it) {
    return !it.section && !it.disabled;
}

} // namespace

void MenuState::open(MenuPurpose purpose,
                     std::string title,
                     std::string hint,
                     std::vector<MenuItem> items,
                     std::string_view select_id,
                     std::string context,
                     int max_width) {
    std::lock_guard<std::mutex> lk(mu_);
    purpose_ = purpose;
    title_ = std::move(title);
    hint_ = std::move(hint);
    context_ = std::move(context);
    items_ = std::move(items);
    max_width_ = std::max(24, max_width);
    selected_ = first_selectable_locked();
    scroll_offset_ = 0;
    if (!select_id.empty()) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].id == select_id && is_selectable(items_[i])) {
                selected_ = static_cast<int>(i);
                break;
            }
        }
    }
    active_ = false;
    for (const auto& it : items_) {
        if (is_selectable(it)) {
            active_ = true;
            break;
        }
    }
    // Allow section-only / empty browse menus to still open when there is at
    // least one row (read-only catalogues with headers).
    if (!active_ && !items_.empty()) {
        active_ = true;
        selected_ = 0;
    }
}

void MenuState::close() {
    std::lock_guard<std::mutex> lk(mu_);
    active_ = false;
    purpose_ = MenuPurpose::None;
    title_.clear();
    hint_.clear();
    context_.clear();
    items_.clear();
    selected_ = 0;
    scroll_offset_ = 0;
    max_width_ = 48;
}

bool MenuState::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

MenuPurpose MenuState::purpose() const {
    std::lock_guard<std::mutex> lk(mu_);
    return purpose_;
}

std::string MenuState::context() const {
    std::lock_guard<std::mutex> lk(mu_);
    return context_;
}

int MenuState::first_selectable_locked() const {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (is_selectable(items_[i])) return static_cast<int>(i);
    }
    return 0;
}

int MenuState::next_selectable_locked(int from, int delta) const {
    const int n = static_cast<int>(items_.size());
    if (n == 0 || delta == 0) return from;
    const int step = delta > 0 ? 1 : -1;
    int steps = std::abs(delta);
    int idx = from;
    while (steps-- > 0) {
        int guard = n;
        do {
            idx = (idx + step) % n;
            if (idx < 0) idx += n;
            --guard;
        } while (guard > 0 && !is_selectable(items_[static_cast<size_t>(idx)]));
    }
    return idx;
}

void MenuState::clamp_scroll_locked(int visible_rows) {
    const int n = static_cast<int>(items_.size());
    if (visible_rows <= 0 || n <= 0) {
        scroll_offset_ = 0;
        return;
    }
    if (selected_ < scroll_offset_) scroll_offset_ = selected_;
    if (selected_ >= scroll_offset_ + visible_rows) {
        scroll_offset_ = selected_ - visible_rows + 1;
    }
    const int max_off = std::max(0, n - visible_rows);
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_off);
}

void MenuState::move_selection(int delta, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_ || items_.empty()) return;
    selected_ = next_selectable_locked(selected_, delta);
    clamp_scroll_locked(visible_rows);
}

void MenuState::page_selection(int direction, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    const int n = static_cast<int>(items_.size());
    if (!active_ || n == 0 || visible_rows <= 0) return;
    const int step = std::max(1, visible_rows - 1);
    const int dir = direction < 0 ? -1 : 1;
    int idx = selected_;
    int moved = 0;
    while (moved < step) {
        const int next = idx + dir;
        if (next < 0 || next >= n) break;
        idx = next;
        if (is_selectable(items_[static_cast<size_t>(idx)])) ++moved;
    }
    // Land on a selectable row (walk back toward the start of the page).
    while (idx >= 0 && idx < n && !is_selectable(items_[static_cast<size_t>(idx)])) {
        idx -= dir;
    }
    if (idx >= 0 && idx < n && is_selectable(items_[static_cast<size_t>(idx)])) {
        selected_ = idx;
    }
    clamp_scroll_locked(visible_rows);
}

bool MenuState::select_shortcut(char key, int visible_rows) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_ || key == 0) return false;
    const char want = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        if (!is_selectable(it) || it.shortcut.empty()) continue;
        const char s = static_cast<char>(
            std::tolower(static_cast<unsigned char>(it.shortcut[0])));
        if (s == want) {
            selected_ = static_cast<int>(i);
            clamp_scroll_locked(visible_rows);
            return true;
        }
    }
    return false;
}

MenuItem MenuState::selected_item() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_ || items_.empty()) return {};
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return {};
    return items_[static_cast<size_t>(selected_)];
}

int MenuState::selected_index() const {
    std::lock_guard<std::mutex> lk(mu_);
    return selected_;
}

MenuSnapshot MenuState::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    MenuSnapshot s;
    s.active = active_;
    s.purpose = purpose_;
    s.title = title_;
    s.hint = hint_;
    s.context = context_;
    s.selected = selected_;
    s.scroll_offset = scroll_offset_;
    s.max_width = max_width_;
    s.items = items_;
    return s;
}

std::vector<MenuItem>
menu_items_themes(const std::vector<std::string>& themes,
                  std::string_view active_name) {
    std::vector<MenuItem> out;
    out.reserve(themes.size());
    for (const auto& name : themes) {
        MenuItem it;
        it.id = name;
        it.label = name;
        it.current = (!active_name.empty() && name == active_name);
        out.push_back(std::move(it));
    }
    return out;
}

std::vector<MenuItem> menu_items_help() {
    // Structured twin of the /help scrollback dump — section headers plus
    // command rows.  `id` is the first token after `/` (help topic / insert).
    struct Row {
        const char* section;  // non-null starts a section
        const char* id;
        const char* label;
        const char* detail;
    };
    static constexpr Row kRows[] = {
        {"Conversation", "send", "/send <agent> <msg>", "send to a specific agent"},
        {nullptr, "ask", "/ask <query>", "ask the index master"},
        {nullptr, "use", "/use <agent>", "switch the focused pane's agent"},
        {"Agents", "agents", "/agents", "list loaded agents"},
        {nullptr, "status", "/status", "system status"},
        {nullptr, "tokens", "/tokens", "token + cost breakdown"},
        {nullptr, "create", "/create <id>", "create agent with default config"},
        {nullptr, "remove", "/remove <id>", "remove agent"},
        {nullptr, "reset", "/reset [id]", "clear agent history"},
        {nullptr, "compact", "/compact [id]", "summarize older turns"},
        {nullptr, "model", "/model", "browse / change agent model"},
        {"Panes", "pane", "/pane <agent> <msg>", "spawn a parallel pane"},
        {"Background loops", "loop", "/loop <agent> <prompt>", "run agent in a background loop"},
        {nullptr, "loops", "/loops", "list running / suspended loops"},
        {nullptr, "log", "/log <loop-id>", "show buffered loop output"},
        {nullptr, "watch", "/watch <loop-id>", "tail loop output live"},
        {nullptr, "kill", "/kill <loop-id>", "stop a loop"},
        {"Fetch + memory", "fetch", "/fetch <url>", "fetch URL into the agent"},
        {nullptr, "mem", "/mem …", "scratchpad + memory graph"},
        {"Tools", "search", "/search <query>", "web search"},
        {nullptr, "browse", "/browse <url>", "JS-rendered fetch"},
        {nullptr, "todo", "/todo …", "conversation task list"},
        {nullptr, "schedule", "/schedule …", "recurring / one-shot tasks"},
        {nullptr, "exec", "/exec <cmd>", "host shell or sandbox"},
        {nullptr, "diff", "/diff …", "review / apply streamed diffs"},
        {nullptr, "write", "/write <path>", "write file / artifact"},
        {nullptr, "read", "/read <path>", "read conversation artifact"},
        {nullptr, "list", "/list", "list conversation artifacts"},
        {nullptr, "map", "/map [path]", "workspace tree"},
        {nullptr, "mcp", "/mcp tools|call", "MCP server registry"},
        {nullptr, "a2a", "/a2a list|call", "remote A2A agents"},
        {nullptr, "lesson", "/lesson list|add", "agent-scoped lessons"},
        {"Plans", "plan", "/plan execute <path>", "execute a planner plan file"},
        {"Session", "theme", "/theme", "browse TUI color themes"},
        {nullptr, "verbose", "/verbose [on|off]", "toggle raw /cmd streaming"},
        {nullptr, "chat", "/chat …", "conversations + folders"},
        {nullptr, "find", "/find <text>", "search focused scrollback"},
        {nullptr, "help", "/help [topic]", "this menu / topic detail"},
        {nullptr, "quit", "/quit", "exit"},
    };

    std::vector<MenuItem> out;
    out.reserve(sizeof(kRows) / sizeof(kRows[0]) + 8);
    for (const auto& r : kRows) {
        if (r.section) {
            MenuItem sec;
            sec.section = true;
            sec.label = r.section;
            out.push_back(std::move(sec));
        }
        MenuItem it;
        it.id = r.id;
        it.label = r.label;
        it.detail = r.detail;
        out.push_back(std::move(it));
    }
    return out;
}

std::vector<MenuItem>
menu_items_models(std::string_view current_model_id) {
    std::size_t n = 0;
    const auto* models = model_catalog(n);
    std::vector<MenuItem> out;
    out.reserve(n);
    std::string last_provider;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = models[i];
        if (last_provider != m.provider) {
            MenuItem sec;
            sec.section = true;
            sec.label = m.provider;
            out.push_back(std::move(sec));
            last_provider = m.provider;
        }
        MenuItem it;
        it.id = m.id;
        it.label = m.id;
        it.detail = "ctx=" + format_context_window(m.context_window);
        if (m.blurb && m.blurb[0] != '\0') {
            it.detail += "  ";
            it.detail += m.blurb;
        }
        it.current = (!current_model_id.empty() && current_model_id == m.id);
        out.push_back(std::move(it));
    }
    return out;
}

} // namespace arbiter
