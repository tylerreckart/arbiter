#pragma once
// Decodes kitty keyboard protocol "CSI ... u" key reports.
//
// Terminals that speak the kitty protocol (kitty, ghostty, WezTerm, foot, ...)
// re-encode any keystroke that would otherwise be ambiguous under the legacy
// VT100 encoding as `CSI <codepoint>[:...][;<mods>[:<event-type>]][;<text>] u`
// once the "disambiguate escape codes" flag is pushed. Ctrl+letter (which
// legacy terminals send as a single C0 control byte) and the Escape key
// (which legacy terminals send as a bare 0x1B, indistinguishable from the
// start of another escape sequence) both fall in that ambiguous set.
//
// Rather than relying on suppressing the terminal's use of this protocol
// (racy: the "supports kitty" capability reply, and therefore the terminal's
// decision to start using it, is asynchronous and can arrive at any time —
// see Engine::render()), arbiter's input layer decodes these reports
// directly back into the legacy control bytes its key-dispatch switches
// already understand. This makes ctrl-key bindings and Esc-cancel correct
// regardless of whether the terminal ever turns the protocol on, keeps it on
// past our attempts to turn it off, or races us on startup.
//
// When "report all keys as escape codes" is active (or a terminal reports
// plain letters as CSI-u anyway), unmodified printable codepoints are also
// mapped back to their ASCII bytes so text entry (sidebar rename, etc.)
// keeps working. An optional third ";text-as-codepoints" field (flag 16)
// must be ignored by the mods/event parser — treating it as part of the
// event-type used to reject the whole report and silently drop typed keys.

#include <optional>
#include <string_view>

namespace arbiter::opentui {

namespace detail {

inline bool parse_uint_field(std::string_view s, int& out) {
    if (s.empty()) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// Split `s` on the first ';'. `first` is the prefix; `rest` is after the
// separator (empty if no separator).
inline void split_semi(std::string_view s,
                       std::string_view& first,
                       std::string_view& rest) {
    const auto semi = s.find(';');
    if (semi == std::string_view::npos) {
        first = s;
        rest = {};
        return;
    }
    first = s.substr(0, semi);
    rest = s.substr(semi + 1);
}

}  // namespace detail

// `params` is the CSI parameter string preceding the final 'u' byte, e.g.
// "112;5" for Ctrl-P or "27" for a disambiguated Esc. Returns the legacy
// single-byte equivalent (a C0 control code or plain ASCII byte) when one
// exists, or std::nullopt for key-release events, bare modifier keypresses,
// or codepoints with no legacy representation.
inline std::optional<int> decode_kitty_csi_u(std::string_view params) {
    if (params.empty() || params[0] == '?') return std::nullopt;

    // Fields: code[:shifted[:base]] [; mods[:event] [; text-as-codepoints]]
    std::string_view code_field;
    std::string_view after_code;
    detail::split_semi(params, code_field, after_code);

    int codepoint = 0;
    int shifted = -1;
    {
        const auto c1 = code_field.find(':');
        std::string_view primary = code_field;
        if (c1 != std::string_view::npos) {
            primary = code_field.substr(0, c1);
            std::string_view rest = code_field.substr(c1 + 1);
            const auto c2 = rest.find(':');
            std::string_view shifted_field =
                (c2 == std::string_view::npos) ? rest : rest.substr(0, c2);
            if (!shifted_field.empty()) {
                int s = 0;
                if (detail::parse_uint_field(shifted_field, s)) shifted = s;
            }
        }
        if (!detail::parse_uint_field(primary, codepoint) || codepoint <= 0) {
            return std::nullopt;
        }
    }

    int modifiers = 1;   // 1 == no modifiers (the spec biases the field by +1)
    int event_type = 1;  // 1 == press, 2 == repeat, 3 == release
    if (!after_code.empty()) {
        std::string_view mod_field;
        std::string_view text_field;
        detail::split_semi(after_code, mod_field, text_field);
        (void)text_field;  // associated text — unused for legacy mapping

        const auto mod_colon = mod_field.find(':');
        std::string_view mod_num =
            (mod_colon == std::string_view::npos) ? mod_field
                                                  : mod_field.substr(0, mod_colon);
        if (!mod_num.empty()) {
            if (!detail::parse_uint_field(mod_num, modifiers)) return std::nullopt;
        }
        if (mod_colon != std::string_view::npos) {
            std::string_view ev = mod_field.substr(mod_colon + 1);
            // Defend against a missing second ';' where text was glued on.
            const auto ev_semi = ev.find(';');
            if (ev_semi != std::string_view::npos) ev = ev.substr(0, ev_semi);
            if (!ev.empty()) {
                if (!detail::parse_uint_field(ev, event_type)) return std::nullopt;
            }
        }
    }
    // A bare release has no legacy equivalent; acting on it would double-fire
    // whatever the matching press already triggered.
    if (event_type == 3) return std::nullopt;

    const int mods = modifiers - 1;  // strip the "always +1" bias
    const bool ctrl = (mods & 0x04) != 0;
    const bool alt = (mods & 0x02) != 0;
    const bool shift = (mods & 0x01) != 0;
    const bool other = (mods & ~0x07) != 0;  // super/hyper/meta/caps/num

    if (ctrl && !alt && !shift && !other) {
        if (codepoint >= 'a' && codepoint <= 'z') return codepoint - 'a' + 1;
        if (codepoint >= 'A' && codepoint <= 'Z') return codepoint - 'A' + 1;
        // Ctrl+[ \ ] ^ _ and Ctrl+Space/? also land in the C0 control range.
        if (codepoint >= '[' && codepoint <= '_') return codepoint - '[' + 0x1B;
        if (codepoint == ' ') return 0;
        if (codepoint == '?') return 0x7F;
    }

    if (!ctrl && !alt && !other) {
        if (!shift) {
            if (codepoint == 27) return 0x1B;   // disambiguated Esc
            if (codepoint == 13) return '\r';   // disambiguated Enter
            if (codepoint == 9) return '\t';    // disambiguated Tab
            if (codepoint == 127) return 0x7F;  // disambiguated Backspace
            // Report-all-keys (or terminals that CSI-u encode plain text):
            // map unmodified printable codepoints back to ASCII.
            if (codepoint >= 0x20 && codepoint < 0x7F) return codepoint;
            return std::nullopt;
        }

        // Shift alone — prefer the shifted alternate from "report alternate
        // keys", else the primary codepoint when it is already the shifted
        // glyph, else uppercase for ASCII letters.
        int ch = codepoint;
        if (shifted >= 0x20 && shifted < 0x7F) ch = shifted;
        else if (codepoint >= 'a' && codepoint <= 'z') {
            ch = codepoint - 'a' + 'A';
        }
        if (ch >= 0x20 && ch < 0x7F) return ch;
    }

    return std::nullopt;
}

}  // namespace arbiter::opentui
