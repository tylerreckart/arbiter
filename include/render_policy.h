#pragma once

#include "styled_text.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

struct RenderPolicy {
    bool     show_writs        = false;
    size_t   code_preview_rows = 8;
    size_t   max_rows          = 0;
    size_t   max_cols          = 0;
    bool     collapse_fences   = false;
    StyleId  base_style        = StyleId::Default;
};

inline constexpr RenderPolicy kMasterStream{
    false, 8, 0, 0, false, StyleId::Default};
inline constexpr RenderPolicy kInterim{
    false, 0, 8, 480, true, StyleId::Dim};
inline constexpr RenderPolicy kVerbose{
    true, 0, 0, 0, false, StyleId::Default};
// Replaying past turns into scrollback on a conversation switch: writs and
// tool-result frames are never shown (there's no verbose replay) and code
// blocks render through the same collapsible/expandable segment live turns
// use, so a switch doesn't dump megabytes of code into view at once.
// Numerically identical to kMasterStream today — kept as its own preset so
// replay's behavior can diverge later without touching live streaming.
inline constexpr RenderPolicy kReplay{
    false, 8, 0, 0, false, StyleId::Default};

void apply_base_style(StyledLine& line, StyleId base);
[[nodiscard]] std::vector<StyledLine> apply_prose_policy(
    std::vector<StyledLine> lines, const RenderPolicy& policy);

[[nodiscard]] StyledLine styled_plain_line(std::string text, StyleId id);
[[nodiscard]] std::vector<StyledLine> styled_plain_lines(
    const std::vector<std::string>& rows, StyleId id);

[[nodiscard]] std::vector<StyledLine> tool_call_summary_lines(int total, int failed);

// Quiet system/activity chrome: middle-dot prefix so lines don't read as
// model prose. Used for interrupts, confirm outcomes, and other system noise.
// Orchestration meta (intent / advisor) uses styled_runtime_* helpers instead.
[[nodiscard]] StyledLine styled_activity_line(std::string text,
                                              StyleId id = StyleId::System);

// Orchestration-meta chrome: distinct glyphs from `·` activity noise and from
// `→ delegating:` so routing / supervision read as first-class runtime events.
// Truncates long detail with an ellipsis (default 120 display bytes).
[[nodiscard]] StyledLine styled_runtime_event(std::string_view glyph,
                                              StyleId glyph_style,
                                              std::string_view label,
                                              StyleId label_style,
                                              std::string_view detail = {},
                                              StyleId detail_style = StyleId::System,
                                              std::size_t detail_max = 120);

// Intent classify/route: applied → `↗ research · heuristic`; hint-only →
// `· intent research · hint:agent` (warning). Unknown/empty kinds return nullopt
// so callers can stay quiet (same contract as gate_continue).
[[nodiscard]] std::optional<StyledLine> styled_intent_event_line(
    std::string_view kind,
    std::string_view source,
    std::string_view target_agent,
    bool applied);

// Advisor consult (`◇ advise · agent`): softest supervision chrome.
[[nodiscard]] StyledLine styled_advisor_consult_line(std::string_view agent_id,
                                                     std::string_view detail);

// Gate redirect (`↻ redirect · agent`): warning + guidance detail.
[[nodiscard]] StyledLine styled_advisor_redirect_line(std::string_view agent_id,
                                                      std::string_view detail);

// Gate halt / budget exhaustion (`× halt · agent`): error. Prefer this over a
// second activity line when both advisor gate_halt and escalation fire.
[[nodiscard]] StyledLine styled_advisor_halt_line(std::string_view agent_id,
                                                  std::string_view reason);

// Sub-agent interim header (`→ agent_id`) drawn before truncated progress.
[[nodiscard]] StyledLine styled_interim_header(const std::string& agent_id);

// Master-turn delegation status (`→ delegating: /agent …`).
[[nodiscard]] StyledLine styled_delegation_line(std::string_view detail);

// Multi-line permission card for destructive confirms (write/exec).
// Option rows use a › caret on `selected`; Enter commits that row.
// `pending_after` annotates how many prompts remain behind this card.
[[nodiscard]] std::vector<StyledLine> styled_permission_card(
    const std::string& action,
    const std::string& target,
    const std::vector<std::string>& preview_lines,
    int pending_after = 0,
    bool accept_edits_on = false,
    int selected = 0);

// Interactive `/diff` review card with selectable apply / reject / allow-all.
[[nodiscard]] std::vector<StyledLine> styled_diff_review_card(
    int patch_id,
    const std::string& path,
    const std::string& summary,
    const std::vector<std::string>& preview_lines,
    int pending_after = 0,
    bool accept_edits_on = false,
    int selected = 0);

// Yes/No choice card (pane close, switch-anyway) — same › caret picker chrome.
[[nodiscard]] std::vector<StyledLine> styled_yes_no_card(
    const std::string& action,
    const std::string& target,
    const std::vector<std::string>& preview_lines,
    int selected = 1);

} // namespace arbiter
