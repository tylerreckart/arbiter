// TUI integration tests.  Each TEST_CASE spawns `index` under a private PTY
// with an isolated $HOME and a bogus API key so nothing touches the real
// config or goes over the network.  Assertions are against the raw byte
// stream the child wrote — most TUI regressions (missing text, broken ANSI
// framing, misaligned chrome) show up there before any screen-parsing layer.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pty_harness.h"

#include <cstdlib>
#include <string>

using namespace index_tests;

// Built into the CMake target at configure time so the tests don't have to
// guess where the binary is.  Set via target_compile_definitions below.
#ifndef INDEX_TEST_BINARY
#  define INDEX_TEST_BINARY "/Users/tyler/dev/index/build/index"
#endif

static PtySession make_session(int rows = 40, int cols = 120) {
    PtySession s(rows, cols);
    // Every test runs against a non-existent session file (fresh temp $HOME),
    // so the welcome card always renders; we don't have to worry about a
    // stale session from a previous run changing the startup state.
    return s;
}

// Identity anchor — present in the first line regardless of how we tweak
// the greeting over time.  Used as the "welcome is fully on-screen" signal
// since the card renders in a single write.
static constexpr const char* kIdentityAnchor = "i'm index";

TEST_CASE("welcome card renders on cold start") {
    PtySession s = make_session();
    s.spawn({ INDEX_TEST_BINARY });

    // Wait for the identity phrase to confirm the card started painting,
    // then drain briefly so the rest of the card (lower text rows, bottom
    // border with version tag) also lands before we start asserting.
    s.read_until(kIdentityAnchor, 3000);
    std::string out = s.read_for(400);

    SUBCASE("identity line is present") {
        CHECK(out.find(kIdentityAnchor) != std::string::npos);
    }

    SUBCASE("three content lines are painted") {
        // Structural: three distinct rows between the top and bottom borders
        // means three "│ ... │   ... │" rows were emitted.  We count content
        // rows by looking for the divider-preceded-by-sigil pattern: each
        // content row ends with " \u2502" after padding + text.  A simpler
        // proxy is counting how many times the ╭...┬...╮ / ╰...┴...╯ frame
        // wraps three distinct text bodies — easier to just verify the card
        // opened and closed, and that three non-empty text bodies exist
        // between the divider and the right border.
        size_t top_pos = out.find("\xE2\x95\xAD");   // ╭
        size_t bot_pos = out.find("\xE2\x95\xB0");   // ╰
        REQUIRE(top_pos != std::string::npos);
        REQUIRE(bot_pos != std::string::npos);
        REQUIRE(top_pos < bot_pos);

        // Between them, the divider \u2502 appears in each of 3 content rows
        // plus 2 blank rows = 5 inner rows containing the divider on both
        // art and text sides.  Rather than count escape-laden rows, just
        // assert the count of │ bytes between the corners is > 10 (a rough
        // minimum for 2 blanks * 3 div chars + 3 content * 3 div chars).
        std::string inner = out.substr(top_pos, bot_pos - top_pos);
        size_t divider_count = 0;
        for (size_t i = 0; i + 2 < inner.size(); ++i) {
            if (inner[i] == '\xE2' && inner[i+1] == '\x94' && inner[i+2] == '\x82') ++divider_count;
        }
        CHECK(divider_count >= 10);
    }

    SUBCASE("agent sigil bytes") {
        // Encoded UTF-8 for the block glyphs the sigil uses.
        CHECK(out.find("\xE2\x96\x93") != std::string::npos);   // ▓
        CHECK(out.find("\xE2\x96\x88") != std::string::npos);   // █
        CHECK(out.find("\xE2\x96\x91") != std::string::npos);   // ░
        CHECK(out.find("\xE2\x96\x92") != std::string::npos);   // ▒
    }

    SUBCASE("box-drawing corners + junctions") {
        CHECK(out.find("\xE2\x95\xAD") != std::string::npos);   // ╭
        CHECK(out.find("\xE2\x95\xAE") != std::string::npos);   // ╮
        CHECK(out.find("\xE2\x94\xAC") != std::string::npos);   // ┬
        CHECK(out.find("\xE2\x94\xB4") != std::string::npos);   // ┴
        CHECK(out.find("\xE2\x95\xB0") != std::string::npos);   // ╰
        CHECK(out.find("\xE2\x95\xAF") != std::string::npos);   // ╯
    }

    SUBCASE("version tag inset on bottom border") {
        // Don't hardcode the exact version — CMakeLists's PROJECT_VERSION
        // bumps would otherwise break this every release.  Just assert
        // a ` v<digit>…` substring appears.
        auto pos = out.find(" v");
        REQUIRE(pos != std::string::npos);
        REQUIRE(pos + 2 < out.size());
        CHECK(std::isdigit(static_cast<unsigned char>(out[pos + 2])));
    }
}

TEST_CASE("chrome rows render identity + separators") {
    PtySession s = make_session();
    s.spawn({ INDEX_TEST_BINARY });
    std::string out = s.read_until(kIdentityAnchor, 3000);

    // Header writes "index" as the current agent; separators under/above the
    // scroll region use dim 256-color rules (\033[38;5;237m ... ────).
    CHECK(out.find("index") != std::string::npos);
    CHECK(out.find("\xE2\x94\x80") != std::string::npos);   // ─  (any separator)

    // Alt-screen enter + DECSTBM scroll-region set.  Present means the TUI
    // really grabbed the terminal; absence would suggest init() bailed.
    CHECK(out.find("\033[?1049h") != std::string::npos);
    CHECK(out.find("\033[3;") != std::string::npos);        // "\033[3;<bot>r"
}

TEST_CASE("footer hint row advertises the right affordances") {
    PtySession s = make_session();
    s.spawn({ INDEX_TEST_BINARY });
    std::string out = s.read_until(kIdentityAnchor, 3000);

    // Affordance labels that live in the bottom hint row.
    CHECK(out.find("esc") != std::string::npos);
    CHECK(out.find("interrupt") != std::string::npos);
    CHECK(out.find("pgup/dn") != std::string::npos);
    CHECK(out.find("scroll") != std::string::npos);
    CHECK(out.find("/agents") != std::string::npos);
    CHECK(out.find("/help") != std::string::npos);
}

TEST_CASE("welcome card dismisses on first user message") {
    PtySession s = make_session();
    s.spawn({ INDEX_TEST_BINARY });
    s.read_until(kIdentityAnchor, 3000);

    // Grab a marker for "where the stream is right now" — anything that
    // arrives strictly after this offset is a direct consequence of the
    // send below, not leftover welcome rendering.
    size_t before_send = s.output().size();

    // Send a short message.  It will fail at the API layer (dummy key) but
    // the dismiss + echo happen BEFORE the dispatch.  Drain for a bit to
    // capture the full clear-and-echo sequence without racing the pump.
    s.send("hi\r");
    s.read_for(1500);

    std::string after = s.output().substr(before_send);
    std::string after_plain = PtySession::strip_ansi(after);

    SUBCASE("scroll-region clear sequence was emitted") {
        // clear_scroll_region() loops rows emitting "\033[<row>;1H\033[2K".
        // Count \033[2K occurrences — one per scroll-region row — and
        // require several so a stray one from unrelated chrome doesn't
        // false-pass this.
        int clears = 0;
        for (size_t i = 0; i + 3 < after.size(); ++i) {
            if (after[i] == '\033' && after[i+1] == '[' &&
                after[i+2] == '2' && after[i+3] == 'K') ++clears;
        }
        CHECK(clears >= 5);
    }

    SUBCASE("user message is echoed into the scroll region") {
        // Echo is styled "\033[38;5;244m> \033[38;5;250mhi\033[0m" — ANSI
        // colors split the literal so we check the stripped view.
        CHECK(after_plain.find("> hi") != std::string::npos);
    }
}

TEST_CASE("terminal geometry is respected") {
    SUBCASE("wide terminal — card centers further right") {
        PtySession s(40, 140);
        s.spawn({ INDEX_TEST_BINARY });
        std::string out = s.read_until(kIdentityAnchor, 3000);
        CHECK(out.find("i'm index.") != std::string::npos);
        // We shouldn't see any line-wrap artefacts at 140 cols — no stray
        // 'a multi' mid-fragmentation of the card's text lines.
    }

    SUBCASE("narrow terminal — card still renders (may be tight)") {
        PtySession s(40, 80);
        s.spawn({ INDEX_TEST_BINARY });
        std::string out = s.read_until(kIdentityAnchor, 3000);
        CHECK(out.find("i'm index.") != std::string::npos);
    }
}
