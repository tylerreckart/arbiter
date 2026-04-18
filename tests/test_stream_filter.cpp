// tests/test_stream_filter.cpp — Unit tests for StreamFilter
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "tui/stream_filter.h"

#include <string>

using namespace index_ai;

// Helper: feed a sequence of chunks, return everything that made it to the
// sink.  Mirrors the real pipeline — markdown renderer, output queue, etc.
// all live downstream of this one sink.
static std::string run(const Config& cfg, const std::vector<std::string>& chunks) {
    std::string out;
    StreamFilter f(cfg, [&out](const std::string& s) { out += s; });
    for (const auto& c : chunks) f.feed(c);
    f.flush();
    return out;
}

TEST_CASE("verbose mode passes everything through unchanged") {
    Config cfg;
    cfg.verbose = true;
    std::string got = run(cfg, {"/exec ls\n", "hello\n", "/fetch http://x\n"});
    CHECK(got == "/exec ls\nhello\n/fetch http://x\n");
}

TEST_CASE("non-verbose swallows /exec /fetch /agent /mem /advise lines") {
    Config cfg;  // verbose = false
    std::string got = run(cfg, {
        "Thinking...\n",
        "/exec ls -la\n",
        "/fetch https://example.com\n",
        "/agent sub do thing\n",
        "/mem read\n",
        "/advise should I?\n",
        "Done.\n"
    });
    CHECK(got == "Thinking...\nDone.\n");
}

TEST_CASE("non-verbose keeps plain narrative prose") {
    Config cfg;
    std::string got = run(cfg, {
        "Line one.\n",
        "Line two with /slash in middle.\n",
        "Line three.\n"
    });
    CHECK(got == "Line one.\nLine two with /slash in middle.\nLine three.\n");
}

TEST_CASE("non-verbose swallows /write block including body and /endwrite") {
    Config cfg;
    std::string got = run(cfg, {
        "before\n",
        "/write foo.txt\n",
        "line 1 of body\n",
        "line 2 of body\n",
        "/endwrite\n",
        "after\n"
    });
    CHECK(got == "before\nafter\n");
}

TEST_CASE("partial line is buffered until newline arrives") {
    Config cfg;
    std::string out;
    StreamFilter f(cfg, [&out](const std::string& s) { out += s; });

    // Split a single line across three chunks.
    f.feed("hel");
    CHECK(out == "");          // buffered
    f.feed("lo wor");
    CHECK(out == "");          // still buffered
    f.feed("ld\n");
    CHECK(out == "hello world\n");
    f.flush();
    CHECK(out == "hello world\n");
}

TEST_CASE("command split across chunks is still swallowed") {
    Config cfg;
    std::string out;
    StreamFilter f(cfg, [&out](const std::string& s) { out += s; });

    f.feed("before\n/ex");
    f.feed("ec ls -la\n");
    f.feed("after\n");
    f.flush();
    CHECK(out == "before\nafter\n");
}

TEST_CASE("/write block spanning multiple feed chunks") {
    Config cfg;
    std::string out;
    StreamFilter f(cfg, [&out](const std::string& s) { out += s; });

    f.feed("intro\n/write ");
    f.feed("foo.txt\nfile line A\n");
    f.feed("file line B\n/endwri");
    f.feed("te\noutro\n");
    f.flush();
    CHECK(out == "intro\noutro\n");
}

TEST_CASE("framing markers ([TOOL RESULTS], [END EXEC], [/exec …]) are swallowed") {
    Config cfg;
    std::string got = run(cfg, {
        "result summary\n",
        "[TOOL RESULTS]\n",
        "[/exec ls]\n",
        "file output here\n",
        "[END EXEC]\n",
        "[END TOOL RESULTS]\n",
        "synthesis\n"
    });
    // The prose between [/exec ...] and [END EXEC] is NOT swallowed by
    // StreamFilter — only the framing markers themselves are.  The exec
    // output inside is plain text and would only appear if the agent quoted
    // it, in which case showing it is desired behavior.
    CHECK(got == "result summary\nfile output here\nsynthesis\n");
}

TEST_CASE("flush emits final partial line if not a swallow target") {
    Config cfg;
    std::string got = run(cfg, {"final partial line"});  // no trailing \n
    CHECK(got == "final partial line");
}

TEST_CASE("flush swallows final partial /cmd line") {
    Config cfg;
    std::string got = run(cfg, {"/exec no-newline-here"});
    CHECK(got == "");
}

TEST_CASE("indented /cmd still swallowed (agent sometimes indents in lists)") {
    Config cfg;
    std::string got = run(cfg, {
        "  /exec ls\n",
        "\t/fetch http://x\n",
        "keep this\n"
    });
    CHECK(got == "keep this\n");
}

TEST_CASE("/execute or /mention does not trigger swallow (prefix-plus-boundary check)") {
    Config cfg;
    std::string got = run(cfg, {
        "/execute is not a command\n",   // /exec followed by "u" — not boundary
        "/mental is not a command\n"     // /mem followed by "t" — not boundary
    });
    CHECK(got == "/execute is not a command\n/mental is not a command\n");
}

TEST_CASE("in_write_block flag tracks /write state across feeds") {
    Config cfg;
    std::string out;
    StreamFilter f(cfg, [&out](const std::string& s) { out += s; });

    CHECK_FALSE(f.in_write_block());
    f.feed("/write foo\n");
    CHECK(f.in_write_block());
    f.feed("body\n");
    CHECK(f.in_write_block());
    f.feed("/endwrite\n");
    CHECK_FALSE(f.in_write_block());
}
