#pragma once
// arbiter/include/api/event_logger.h
//
// Mirrors SSE events to stderr for operators running `arbiter --api`.

#include "json.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <unistd.h>

namespace arbiter {

class EventLogger {
public:
    EventLogger(bool verbose, std::string request_id, std::string tenant_name)
        : verbose_(verbose),
          color_(::isatty(fileno(stderr)) != 0),
          request_id_(std::move(request_id)),
          tenant_name_(std::move(tenant_name)) {
        (void)tenant_name_;   // retained for future structured logging.
    }

    // Emit one event.  `ev` is the SSE event name; `payload` mirrors the
    // JSON body about to be written to the wire.  The logger reads only
    // the fields it cares about; unknown shapes are tolerated.
    //
    // Layout (matches the design mock at docs):
    //   • A two-line block header on request_received (title + subtitle +
    //     horizontal rule).
    //   • A "marker form" for turn boundaries (request_received,
    //     stream_start): `event: <gap> <meta>` on one line followed by
    //     the event name in colour on its own line.
    //   • An "inline form" for events with a primary value (tool_call,
    //     gate, file, done, error): `event: <name> <gap> <value>`.
    //
    // Streamed text content (text/thinking deltas) is intentionally
    // suppressed: it already mirrors back to the client over SSE, and
    // duplicating multi-thousand-token agent prose into the operator's
    // stderr drowns out the event spine the verbose log is meant to
    // surface.  An operator who wants the full text reads the SSE
    // response directly.
    void log(const std::string& ev, const std::shared_ptr<JsonValue>& payload) {
        const bool always = (ev == "request_received" || ev == "done" ||
                             ev == "error");
        if (!always && !verbose_) return;

        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream line;

        if (ev == "request_received") {
            const std::string agent = payload ? payload->get_string("agent") : "";
            const std::string msg   = payload ? payload->get_string("message") : "";
            emit_header_locked("POST", "/v1/orchestrate", agent, msg);
            // Then the request_received marker, with the short request id
            // as its meta value.
            std::string short_id = request_id_;
            if (short_id.size() > 4) short_id.resize(4);
            std::string meta = std::string("req_") + short_id;
            emit_marker_locked(meta, "request_received", kBoldMagenta, line);
            return;
        }
        if (ev == "stream_start") {
            const std::string agent = payload ? payload->get_string("agent") : "";
            const int depth = payload ? static_cast<int>(payload->get_number("depth")) : 0;
            std::ostringstream meta;
            // Display depth+1 so the master shows as "depth 1" rather than
            // "depth 0" — matches the design's 1-indexed convention and
            // reads more naturally to humans skimming the log.
            meta << color_for_agent(agent) << display_agent(agent) << reset()
                 << " " << color(kDim) << "·" << reset()
                 << " depth " << (depth + 1);
            emit_marker_locked(meta.str(), "stream_start", kBoldMagenta, line);
            return;
        }
        if (ev == "agent_start") {
            // Already represented by stream_start; second announce is noise.
            return;
        }
        if (ev == "stream_end") {
            // Successful ends stay quiet (a coloured success marker on
            // its own line adds noise across many parallel streams);
            // failures surface so an operator notices a stalled
            // sub-agent.
            const bool ok = payload && payload->get_bool("ok");
            if (ok) return;
            emit_inline_locked("stream_end", kBoldRed, "stream ended without ok", line);
            return;
        }
        if (ev == "text" || ev == "thinking") {
            // Suppressed — the SSE response already carries the agent's
            // text to the client; mirroring it on the operator's stderr
            // turned the verbose log into a wall of prose.  Tool /
            // status events alone tell the operator-relevant story.
            return;
        }
        if (ev == "tool_call") {
            const std::string tool = payload ? payload->get_string("tool") : "";
            const bool ok = payload && payload->get_bool("ok");
            std::ostringstream value;
            value << color_for_tool(tool) << "/" << tool << reset();
            if (!ok) value << " " << color(kBoldRed) << "ERR" << reset();
            emit_inline_locked("tool_call", kBoldCyan, value.str(), line);
            return;
        }
        if (ev == "token_usage" || ev == "sub_agent_response") {
            // Suppressed — the design surfaces aggregate token counts on
            // `done`, and sub-agent text already streamed via deltas.
            return;
        }
        if (ev == "file") {
            const std::string path = payload ? payload->get_string("path") : "";
            const double size = payload ? payload->get_number("size") : 0;
            std::ostringstream value;
            value << "wrote " << path
                  << " " << color(kDim) << "("
                  << fmt_size(static_cast<int64_t>(size)) << ")" << reset();
            emit_inline_locked("file", kBoldMagenta, value.str(), line);
            return;
        }
        if (ev == "advisor") {
            const std::string kind    = payload ? payload->get_string("kind")  : "";
            const std::string detail  = payload ? payload->get_string("detail") : "";
            const std::string preview = payload ? payload->get_string("preview") : "";
            const bool malformed      = payload && payload->get_bool("malformed");

            const char* clr = kBoldYellow;
            std::string label = "gate";
            std::ostringstream value;
            if (kind == "consult") {
                clr = kBoldCyan;
                label = "advise";
                value << quote_short(detail.empty() ? preview : detail, 80);
            } else if (kind == "gate_continue") {
                clr = kBoldYellow;
                value << "verdict: continue " << color(kGreen) << "✓" << reset();
            } else if (kind == "gate_redirect") {
                clr = kBoldYellow;
                value << "verdict: redirect " << color(kYellow) << "↻" << reset();
                if (!detail.empty()) value << "  " << quote_short(detail, 60);
            } else if (kind == "gate_halt") {
                clr = kBoldRed;
                value << "verdict: halt " << color(kBoldRed) << "✗" << reset();
                if (!detail.empty()) value << "  " << quote_short(detail, 60);
            } else if (kind == "gate_budget") {
                clr = kBoldRed;
                value << "verdict: budget " << color(kBoldRed) << "⛔" << reset();
            } else {
                value << kind << " " << detail;
            }
            if (malformed) value << " " << color(kDim) << "(malformed)" << reset();
            emit_inline_locked(label, clr, value.str(), line);
            return;
        }
        if (ev == "intent") {
            const std::string kind   = payload ? payload->get_string("kind") : "";
            const std::string source = payload ? payload->get_string("source") : "";
            const std::string target = payload ? payload->get_string("target_agent") : "";
            const bool applied       = payload && payload->get_bool("applied");
            std::ostringstream value;
            value << kind;
            if (!source.empty()) value << " " << color(kDim) << source << reset();
            if (!target.empty())
                value << " → " << target << (applied ? "" : " (hint)");
            emit_inline_locked("intent", kBoldCyan, value.str(), line);
            return;
        }
        if (ev == "escalation") {
            const std::string reason = payload ? payload->get_string("reason") : "";
            emit_inline_locked("escalation", kBoldRed,
                                quote_short(reason, 80), line);
            return;
        }
        if (ev == "done") {
            const bool ok    = payload && payload->get_bool("ok");
            const double dur = payload ? payload->get_number("duration_ms") : 0;
            const double in  = payload ? payload->get_number("input_tokens") : 0;
            const double out = payload ? payload->get_number("output_tokens") : 0;
            std::ostringstream value;
            value << (ok ? "ok=true" : "ok=false")
                  << " " << color(kDim) << "·" << reset() << " "
                  << std::fixed << std::setprecision(1) << (dur / 1000.0) << "s";
            const double cost = estimate_cost(in, out);
            if (cost > 0) {
                value << " " << color(kDim) << "·" << reset() << " "
                      << "$" << std::fixed << std::setprecision(4) << cost;
            } else if (in > 0 || out > 0) {
                value << " " << color(kDim) << "·" << reset() << " "
                      << "in=" << static_cast<int>(in)
                      << " out=" << static_cast<int>(out);
            }
            if (!ok && payload) {
                const std::string err = payload->get_string("error");
                if (!err.empty()) value << "  " << quote_short(err, 60);
            }
            emit_inline_locked("done", ok ? kBoldGreen : kBoldRed, value.str(), line);
            return;
        }
        if (ev == "error") {
            const std::string m = payload ? payload->get_string("message") : "";
            emit_inline_locked("error", kBoldRed, quote_short(m, 100), line);
            return;
        }
        // Unknown event — log the name only; useful while iterating.
        emit_inline_locked(ev, kDim, "", line);
    }

private:
    // ANSI colour codes — only emitted when stderr is a TTY.
    static constexpr const char* kReset         = "\033[0m";
    static constexpr const char* kBold          = "\033[1m";
    static constexpr const char* kDim           = "\033[2m";
    static constexpr const char* kRed           = "\033[31m";
    static constexpr const char* kGreen         = "\033[32m";
    static constexpr const char* kYellow        = "\033[33m";
    static constexpr const char* kCyan          = "\033[36m";
    static constexpr const char* kBoldRed       = "\033[1;31m";
    static constexpr const char* kBoldGreen     = "\033[1;32m";
    static constexpr const char* kBoldYellow    = "\033[1;33m";
    static constexpr const char* kBoldCyan      = "\033[1;36m";
    static constexpr const char* kBoldMagenta   = "\033[1;35m";

    // Column where event values align.  "event:" is 6 chars; "event: " + an
    // event-name token leaves us at "event: <name> <pad> value" with the
    // value starting at column 24 from line origin.  Tuned by eye against
    // the design mock — wide enough that "tool_call" and "request_received"
    // both fit comfortably without wrapping the value column off-screen.
    static constexpr int kValueCol = 24;
    // Per-agent colour palette — muted 256-colour shades.  The previous
    // bright-only palette read uniformly garish across siblings in a
    // /parallel fan-out; these tones stay distinguishable side-by-side
    // without competing for attention.  Hashed on the *display* name
    // (post-`seed-` strip) so a starter and its prefixed twin draw in
    // the same colour.
    static constexpr const char* kAgentPalette[] = {
        "\033[38;5;109m",  // soft cyan
        "\033[38;5;144m",  // khaki
        "\033[38;5;110m",  // light steel blue
        "\033[38;5;138m",  // dusty pink
        "\033[38;5;108m",  // sage
        "\033[38;5;180m",  // warm tan
        "\033[38;5;175m",  // mauve
        "\033[38;5;152m",  // pale aqua
        "\033[38;5;187m",  // light buff
        "\033[38;5;146m",  // periwinkle
    };
    // Per-tool colour palette — distinct from the agent palette so the
    // tool token visually separates from the agent token on the same
    // line.  Hashed on the tool name (`search`, `fetch`, `mem`, ...) so
    // every invocation of the same tool draws in the same colour.
    static constexpr const char* kToolPalette[] = {
        "\033[38;5;73m",   // teal
        "\033[38;5;178m",  // gold
        "\033[38;5;168m",  // rose
        "\033[38;5;105m",  // periwinkle (deeper)
        "\033[38;5;137m",  // terracotta
        "\033[38;5;79m",   // seafoam
        "\033[38;5;167m",  // coral
        "\033[38;5;115m",  // mint
        "\033[38;5;215m",  // peach
        "\033[38;5;141m",  // amethyst
    };

    // Strip a `seed-` prefix from the displayed agent name.  The starter
    // agents seeded by `arbiter --init` carry that prefix internally for
    // disambiguation; surfacing it in every log line is just noise.
    static std::string display_agent(const std::string& name) {
        constexpr const char* kPrefix = "seed-";
        constexpr size_t      kLen    = 5;
        if (name.size() > kLen && name.compare(0, kLen, kPrefix) == 0)
            return name.substr(kLen);
        return name;
    }
    const char* color_for_agent(const std::string& name) const {
        if (!color_ || name.empty()) return "";
        const std::string disp = display_agent(name);
        size_t h = 0;
        for (char c : disp) h = h * 131 + static_cast<unsigned char>(c);
        constexpr size_t N = sizeof(kAgentPalette) / sizeof(kAgentPalette[0]);
        return kAgentPalette[h % N];
    }
    const char* color_for_tool(const std::string& name) const {
        if (!color_ || name.empty()) return "";
        size_t h = 0;
        for (char c : name) h = h * 131 + static_cast<unsigned char>(c);
        constexpr size_t N = sizeof(kToolPalette) / sizeof(kToolPalette[0]);
        return kToolPalette[h % N];
    }

    const char* color(const char* c) const { return color_ ? c : ""; }
    const char* reset() const               { return color_ ? kReset : ""; }

// Pad an ostringstream out to `kValueCol` from line origin, given the
    // visible character count already written.  ANSI escapes don't count
    // toward visible width — callers pass the visible-only count.
    static void pad_to_value_col(std::ostringstream& line, int written) {
        int pad = kValueCol - written;
        if (pad < 1) pad = 1;
        for (int i = 0; i < pad; ++i) line << ' ';
    }

    // Emit a single SSE record in "marker form": one line `event: <gap>
    // <meta>`, then the event name on its own line in `event_color`.
    // Used for turn boundaries (request_received, stream_start) where
    // the event itself is the salient signal and the meta value just
    // contextualizes which agent / id the boundary applies to.
    void emit_marker_locked(const std::string& meta_value,
                             const std::string& event_name,
                             const char* event_color,
                             std::ostringstream& line) {
        line << color(kDim) << "event:" << reset();
        pad_to_value_col(line, /*written=*/6);   // "event:" is 6 chars
        line << color(kDim) << meta_value << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();

        line << color(event_color) << event_name << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();
    }

    // Emit a single SSE record in "inline form": `event: <name> <gap>
    // <value>` on one line.  Used for events with a primary value
    // (tool_call, gate, file, done, error).  The value column lines up
    // with the marker form's meta column so the two intermix cleanly.
    void emit_inline_locked(const std::string& event_name,
                             const char* event_color,
                             const std::string& value,
                             std::ostringstream& line) {
        line << color(kDim) << "event: " << reset()
             << color(event_color) << event_name << reset();
        const int written = 7 + static_cast<int>(event_name.size());
        pad_to_value_col(line, written);
        line << value;
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();
    }

    // Emit the request header — three lines anchoring the rest of the
    // log block.  Fired exactly once per request, on request_received.
    void emit_header_locked(const std::string& method,
                             const std::string& path,
                             const std::string& agent,
                             const std::string& message) {
        std::ostringstream line;
        line << color(kBold) << "arbiter "
             << color(kDim) << "↗" << reset()
             << color(kBold) << " " << method << " " << path << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();

        line << color(kDim) << "agent: " << reset()
             << color_for_agent(agent) << display_agent(agent) << reset()
             << color(kDim) << " message: " << reset()
             << quote_short(message, 70);
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
        line.str(""); line.clear();

        // Horizontal rule.  Width is screen-friendly without ever
        // wrapping in an 80-col terminal.  Drawn dim so it recedes.
        line << color(kDim);
        for (int i = 0; i < 70; ++i) line << "─";
        line << reset();
        std::fputs(line.str().c_str(), stderr);
        std::fputc('\n', stderr);
    }

    // Emit one text/thinking line in the column layout: streamed text
    // Rough cost estimate in USD for the demo log.  Sonnet-equivalent
    // pricing ($3/M input, $15/M output).  Returns 0 when no tokens
    // were used so the caller can fall back to a tokens display.
    static double estimate_cost(double in_tokens, double out_tokens) {
        if (in_tokens <= 0 && out_tokens <= 0) return 0.0;
        return (in_tokens / 1'000'000.0) * 3.0
             + (out_tokens / 1'000'000.0) * 15.0;
    }

    // Truncate to a screen-friendly preview and quote.  Newlines flatten to
    // spaces so a single log line stays one row in the operator's terminal.
    static std::string quote_short(const std::string& s, size_t cap = 110) {
        std::string out;
        out.reserve(std::min(s.size(), cap) + 8);
        out += '"';
        size_t take = std::min(s.size(), cap);
        for (size_t i = 0; i < take; ++i) {
            char c = s[i];
            if (c == '\n' || c == '\r' || c == '\t') out += ' ';
            else out += c;
        }
        out += '"';
        if (s.size() > cap) out += "…";
        return out;
    }

    // Bytes → "120B" / "3.4KB" / "1.2MB".  Demo-friendly file/size labels.
    static std::string fmt_size(int64_t bytes) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1);
        if (bytes < 1024)              o << bytes << "B";
        else if (bytes < 1024 * 1024)  o << (bytes / 1024.0) << "KB";
        else                            o << (bytes / (1024.0 * 1024.0)) << "MB";
        return o.str();
    }

    bool        verbose_;
    bool        color_;
    std::string request_id_;
    std::string tenant_name_;
    std::mutex  mu_;
};

class SseStream;
void emit_error(SseStream& sse, const std::string& msg);

} // namespace arbiter
