#pragma once
// Decodes kitty keyboard protocol "CSI ... u" key reports.
//
// Terminals that speak the kitty protocol (kitty, ghostty, WezTerm, foot, ...)
// re-encode any keystroke that would otherwise be ambiguous under the legacy
// VT100 encoding as `CSI <codepoint>[:...][;<mods>[:<event-type>]] u` once the
// "disambiguate escape codes" flag is pushed. Ctrl+letter (which legacy
// terminals send as a single C0 control byte) and the Escape key (which
// legacy terminals send as a bare 0x1B, indistinguishable from the start of
// another escape sequence) both fall in that ambiguous set.
//
// Rather than relying on suppressing the terminal's use of this protocol
// (racy: the "supports kitty" capability reply, and therefore the terminal's
// decision to start using it, is asynchronous and can arrive at any time —
// see Engine::render()), arbiter's input layer decodes these reports
// directly back into the legacy control bytes its key-dispatch switches
// already understand. This makes ctrl-key bindings and Esc-cancel correct
// regardless of whether the terminal ever turns the protocol on, keeps it on
// past our attempts to turn it off, or races us on startup.

#include <optional>
#include <string_view>

namespace arbiter::opentui {

// `params` is the CSI parameter string preceding the final 'u' byte, e.g.
// "112;5" for Ctrl-P or "27" for a disambiguated Esc. Returns the legacy
// single-byte equivalent (a C0 control code or plain ASCII byte) when one
// exists, or std::nullopt for key-release events, bare modifier keypresses,
// or codepoints with no legacy representation.
inline std::optional<int> decode_kitty_csi_u(std::string_view params) {
    if (params.empty()) return std::nullopt;

    // Format: <codepoint>[:<shifted>[:<base-layout>]][;<mods>[:<event-type>]]
    const auto semi = params.find(';');
    std::string_view code_field = params.substr(0, semi);
    const auto code_colon = code_field.find(':');
    if (code_colon != std::string_view::npos) code_field = code_field.substr(0, code_colon);

    if (code_field.empty()) return std::nullopt;
    int codepoint = 0;
    for (char c : code_field) {
        if (c < '0' || c > '9') return std::nullopt;
        codepoint = codepoint * 10 + (c - '0');
    }
    if (codepoint <= 0) return std::nullopt;

    int modifiers = 1;   // 1 == no modifiers (the spec biases the field by +1)
    int event_type = 1;  // 1 == press, 2 == repeat, 3 == release
    if (semi != std::string_view::npos) {
        std::string_view mod_field = params.substr(semi + 1);
        const auto mod_colon = mod_field.find(':');
        std::string_view mod_num =
            (mod_colon == std::string_view::npos) ? mod_field : mod_field.substr(0, mod_colon);
        if (!mod_num.empty()) {
            modifiers = 0;
            for (char c : mod_num) {
                if (c < '0' || c > '9') return std::nullopt;
                modifiers = modifiers * 10 + (c - '0');
            }
        }
        if (mod_colon != std::string_view::npos) {
            std::string_view ev = mod_field.substr(mod_colon + 1);
            if (!ev.empty()) {
                event_type = 0;
                for (char c : ev) {
                    if (c < '0' || c > '9') return std::nullopt;
                    event_type = event_type * 10 + (c - '0');
                }
            }
        }
    }
    // A bare release has no legacy equivalent; acting on it would double-fire
    // whatever the matching press already triggered.
    if (event_type == 3) return std::nullopt;

    const int mods = modifiers - 1;  // strip the "always +1" bias
    const bool ctrl = (mods & 0x04) != 0;
    const bool alt = (mods & 0x02) != 0;
    const bool other = (mods & ~0x06) != 0;  // shift/super/hyper/meta/caps/num

    if (ctrl && !alt && !other) {
        if (codepoint >= 'a' && codepoint <= 'z') return codepoint - 'a' + 1;
        if (codepoint >= 'A' && codepoint <= 'Z') return codepoint - 'A' + 1;
        // Ctrl+[ \ ] ^ _ and Ctrl+Space/? also land in the C0 control range.
        if (codepoint >= '[' && codepoint <= '_') return codepoint - '[' + 0x1B;
        if (codepoint == ' ') return 0;
        if (codepoint == '?') return 0x7F;
    }

    if (!ctrl && !alt && !other) {
        if (codepoint == 27) return 0x1B;   // disambiguated Esc
        if (codepoint == 13) return '\r';   // disambiguated Enter
        if (codepoint == 9) return '\t';    // disambiguated Tab
        if (codepoint == 127) return 0x7F;  // disambiguated Backspace
    }

    return std::nullopt;
}

}  // namespace arbiter::opentui
