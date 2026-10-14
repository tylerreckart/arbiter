#pragma once
// State glyphs for the TUI, transcript chrome, and text lists.
//
// Unicode is the shape language: filled = happening now, hollow = not yet,
// dotted = waiting, a mark = finished. ASCII is the single-byte fallback
// for dumb terminals, non-UTF-8 locales, and ARBITER_GLYPHS=ascii.
//
// Every unicode mark is one cell (no emoji presentation). ASCII tokens for
// a few events are two or three bytes because those states still have to
// stay distinct; callers pad with display_width, not a hard-coded +1.

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace arbiter {

enum class GlyphMode : std::uint8_t { Unicode, Ascii };

// Shared run-state. Fleet nodes, todos, tool rows, and activity badges
// map into this (or ActivityBadgeKind) and then into a glyph + tone.
enum class RunState : std::uint8_t {
    Pending,
    Running,
    Waiting,
    Succeeded,
    Failed,
    Cancelled,
};

// Unfocused-pane badge. Pulse is attention, not the running disc, so it
// does not share a glyph with RunState::Running.
enum class ActivityBadgeKind : std::uint8_t {
    None,
    Pulse,
    Succeeded,
    Failed,
};

enum class GlyphTone : std::uint8_t {
    Muted,
    Primary,
    Warning,
    Success,
    Error,
    Info,
};

struct GlyphSet {
    const char* pending;
    const char* running;
    const char* waiting;
    const char* succeeded;
    const char* failed;
    const char* cancelled;
    const char* advisor_halt;
    const char* advisor_redirect;
    const char* advisor_consult;
    const char* intent_applied;
    const char* intent_hint;
    const char* presence;
    const char* jit;
    const char* activity;
    const char* scheduled;
    const char* paused;
    const char* loop;
    const char* folder_open;
    const char* folder_closed;
    const char* task_todo;
    const char* task_done;
    const char* separator;
    const char* handoff;
    const char* tree_vert;
    const char* tree_branch;
    const char* tree_last;
};

inline const GlyphSet& unicode_glyphs() {
    static const GlyphSet g{
        "\u25CB",  // ○ pending
        "\u25CF",  // ● running
        "\u25CC",  // ◌ waiting
        "\u2713",  // ✓ done
        "\u2717",  // ✗ failed
        "\u2298",  // ⊘ cancelled
        "\u25A0",  // ■ advisor halt
        "\u21BA",  // ↺ advisor redirect
        "\u25C6",  // ◆ advisor consult
        "\u2192",  // → intent applied
        "\u00B7",  // · intent hint
        "\u21B3",  // ↳ presence note
        "\u25C7",  // ◇ JIT agent
        "\u2022",  // • unread activity
        "\u25F7",  // ◷ scheduled
        "\u2016",  // ‖ paused
        "\u21BB",  // ↻ loop
        "\u25BE",  // ▾ folder open
        "\u25B8",  // ▸ folder closed
        "\u25A1",  // □ markdown task open
        "\u25A0",  // ■ markdown task done
        "\u00B7",  // · separator
        "\u2192",  // → handoff / delegating
        "\u2502",  // │
        "\u251C",  // ├
        "\u2514",  // └
    };
    return g;
}

inline const GlyphSet& ascii_glyphs() {
    static const GlyphSet g{
        "-",   // pending
        "*",   // running
        "~",   // waiting
        "+",   // done
        "x",   // failed
        "/",   // cancelled
        "!!",  // advisor halt
        "<>",  // advisor redirect
        "?",   // advisor consult
        "->",  // intent applied
        ".",   // intent hint
        ">>",  // presence note
        "^",   // JIT agent
        "*",   // unread activity
        "@",   // scheduled
        "||",  // paused
        "@@",  // loop
        "[-]", // folder open
        "[+]", // folder closed
        "[ ]", // markdown task open
        "[x]", // markdown task done
        ".",   // separator
        "->",  // handoff
        "|",
        "+",
        "L",
    };
    return g;
}

inline bool glyph_token_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        unsigned char ca = static_cast<unsigned char>(*a);
        unsigned char cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

inline bool env_claims_utf8(const char* v) {
    if (!v || !*v) return false;
    // Locale names are short. Fold case into a stack buffer and search.
    char buf[96];
    std::size_t n = 0;
    for (; v[n] && n + 1 < sizeof(buf); ++n) {
        unsigned char c = static_cast<unsigned char>(v[n]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        buf[n] = static_cast<char>(c);
    }
    buf[n] = 0;
    return std::strstr(buf, "utf-8") != nullptr || std::strstr(buf, "utf8") != nullptr;
}

inline bool locale_is_utf8() {
    const char* lc_all = std::getenv("LC_ALL");
    if (lc_all && *lc_all) return env_claims_utf8(lc_all);
    const char* lc_ctype = std::getenv("LC_CTYPE");
    if (lc_ctype && *lc_ctype) return env_claims_utf8(lc_ctype);
    const char* lang = std::getenv("LANG");
    if (lang && *lang) return env_claims_utf8(lang);
    // No locale advertised. Modern terminals are UTF-8; only an explicit
    // C/POSIX locale or TERM=dumb opts out.
    return true;
}

inline bool term_forces_ascii() {
    const char* term = std::getenv("TERM");
    if (!term || !*term) return false;
    return glyph_token_eq(term, "dumb") || glyph_token_eq(term, "unknown");
}

// ARBITER_GLYPHS=unicode|ascii|auto. Unset and auto follow the locale and
// TERM. An explicit mode wins over both.
inline GlyphMode glyph_mode() {
    const char* e = std::getenv("ARBITER_GLYPHS");
    if (e && *e) {
        if (glyph_token_eq(e, "ascii")) return GlyphMode::Ascii;
        if (glyph_token_eq(e, "unicode") || glyph_token_eq(e, "utf-8") ||
            glyph_token_eq(e, "utf8") || glyph_token_eq(e, "shape")) {
            return GlyphMode::Unicode;
        }
        // auto, empty-ish, or unknown → detection below.
    }
    if (term_forces_ascii()) return GlyphMode::Ascii;
    if (!locale_is_utf8()) return GlyphMode::Ascii;
    return GlyphMode::Unicode;
}

inline const GlyphSet& glyphs() {
    return glyph_mode() == GlyphMode::Ascii ? ascii_glyphs() : unicode_glyphs();
}

inline const char* glyph_for(RunState state) {
    const GlyphSet& g = glyphs();
    switch (state) {
    case RunState::Pending:   return g.pending;
    case RunState::Running:   return g.running;
    case RunState::Waiting:   return g.waiting;
    case RunState::Succeeded: return g.succeeded;
    case RunState::Failed:    return g.failed;
    case RunState::Cancelled: return g.cancelled;
    }
    return g.pending;
}

inline const char* glyph_for(ActivityBadgeKind kind) {
    const GlyphSet& g = glyphs();
    switch (kind) {
    case ActivityBadgeKind::None:      return "";
    case ActivityBadgeKind::Pulse:     return g.activity;
    case ActivityBadgeKind::Succeeded: return g.succeeded;
    case ActivityBadgeKind::Failed:    return g.failed;
    }
    return "";
}

inline GlyphTone tone_for(RunState state) {
    switch (state) {
    case RunState::Pending:   return GlyphTone::Muted;
    case RunState::Running:   return GlyphTone::Primary;
    case RunState::Waiting:   return GlyphTone::Warning;
    case RunState::Succeeded: return GlyphTone::Success;
    case RunState::Failed:    return GlyphTone::Error;
    case RunState::Cancelled: return GlyphTone::Muted;
    }
    return GlyphTone::Muted;
}

inline GlyphTone tone_for(ActivityBadgeKind kind) {
    switch (kind) {
    case ActivityBadgeKind::None:      return GlyphTone::Muted;
    case ActivityBadgeKind::Pulse:     return GlyphTone::Warning;
    case ActivityBadgeKind::Succeeded: return GlyphTone::Success;
    case ActivityBadgeKind::Failed:    return GlyphTone::Error;
    }
    return GlyphTone::Muted;
}

inline RunState run_state_from_todo(const char* status) {
    if (!status) return RunState::Pending;
    if (std::strcmp(status, "completed") == 0) return RunState::Succeeded;
    if (std::strcmp(status, "in_progress") == 0) return RunState::Running;
    if (std::strcmp(status, "canceled") == 0 ||
        std::strcmp(status, "cancelled") == 0) {
        return RunState::Cancelled;
    }
    return RunState::Pending;
}

inline RunState run_state_from_tool(bool finished, bool ok) {
    if (!finished) return RunState::Running;
    return ok ? RunState::Succeeded : RunState::Failed;
}

}  // namespace arbiter
