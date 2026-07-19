// tests/test_commands.cpp — Unit tests for security-critical command functions
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "commands.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace arbiter;

// ---------------------------------------------------------------------------
// is_destructive_exec
// ---------------------------------------------------------------------------

TEST_CASE("is_destructive_exec catches common destructive patterns") {
    // rm variants
    CHECK(is_destructive_exec("rm foo.txt"));
    CHECK(is_destructive_exec("rm -rf /tmp/stuff"));
    CHECK(is_destructive_exec("rmdir somedir"));

    // sudo / doas
    CHECK(is_destructive_exec("sudo apt install foo"));
    CHECK(is_destructive_exec("doas rm foo"));

    // git destructive operations
    CHECK(is_destructive_exec("git reset --hard"));
    CHECK(is_destructive_exec("git clean -f"));
    CHECK(is_destructive_exec("git push --force"));
    CHECK(is_destructive_exec("git push --force-with-lease"));
    CHECK(is_destructive_exec("git branch -D mybranch"));
    CHECK(is_destructive_exec("git checkout -- ."));
    CHECK(is_destructive_exec("git restore ."));

    // Redirects
    CHECK(is_destructive_exec("echo x > file.txt"));
    CHECK(is_destructive_exec("echo x >> file.txt"));

    // find -delete
    CHECK(is_destructive_exec("find . -delete"));
    CHECK(is_destructive_exec("find . -exec rm {} ;"));

    // Disk tools
    CHECK(is_destructive_exec("mkfs.ext4 /dev/sda1"));
    CHECK(is_destructive_exec("dd if=/dev/zero of=/dev/sda"));
    CHECK(is_destructive_exec("wipefs /dev/sda"));
    CHECK(is_destructive_exec("fdisk /dev/sda"));
    CHECK(is_destructive_exec("truncate -s 0 file.txt"));
}

TEST_CASE("is_destructive_exec allows safe commands") {
    CHECK_FALSE(is_destructive_exec("ls -la"));
    CHECK_FALSE(is_destructive_exec("cat foo.txt"));
    CHECK_FALSE(is_destructive_exec("git status"));
    CHECK_FALSE(is_destructive_exec("git log --oneline"));
    CHECK_FALSE(is_destructive_exec("git diff"));
    CHECK_FALSE(is_destructive_exec("grep -r pattern ."));
    CHECK_FALSE(is_destructive_exec("pwd"));
    CHECK_FALSE(is_destructive_exec("wc -l file.txt"));
    CHECK_FALSE(is_destructive_exec("head -20 file.txt"));
    CHECK_FALSE(is_destructive_exec("find . -name '*.cpp'"));
}

// ---------------------------------------------------------------------------
// cmd_exec — destructive command blocking
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// is_tool_result_failure
// ---------------------------------------------------------------------------

TEST_CASE("is_tool_result_failure flags error patterns") {
    CHECK(is_tool_result_failure("ERR: something failed"));
    CHECK(is_tool_result_failure("[/exec x]\nERR: boom\n[END EXEC]\n"));
    CHECK(is_tool_result_failure("UPSTREAM FAILED — retry aborted"));
    CHECK(is_tool_result_failure("SKIPPED: max 3 fetches per turn"));
    CHECK(is_tool_result_failure("stdout\n[exit 1]\n"));
    CHECK(is_tool_result_failure("stdout\n[exit 127]\n"));
    CHECK(is_tool_result_failure("ERR: /write block was truncated"));
    CHECK(is_tool_result_failure("TRUNCATED: budget exhausted"));
}

TEST_CASE("is_tool_result_failure allows clean output") {
    CHECK_FALSE(is_tool_result_failure("file contents here"));
    CHECK_FALSE(is_tool_result_failure("[/exec ls]\nfoo.cpp\nbar.cpp\n[END EXEC]\n"));
    CHECK_FALSE(is_tool_result_failure("OK: wrote 42 bytes to foo.md"));
    CHECK_FALSE(is_tool_result_failure("stdout\n[exit 0]\n"));
    // "error" as prose is fine — only "ERR:" exactly is the failure signal
    CHECK_FALSE(is_tool_result_failure("Discussing errors in general."));
}

TEST_CASE("cmd_exec blocks destructive commands by default") {
    std::string result = cmd_exec("rm -rf /tmp/test_nonexistent_dir_xyz");
    CHECK(result.find("ERR:") == 0);
    CHECK(result.find("destructive") != std::string::npos);
}

TEST_CASE("cmd_exec allows safe commands") {
    std::string result = cmd_exec("echo hello");
    CHECK(result == "hello");
}

TEST_CASE("cmd_exec runs destructive commands when confirmed") {
    // This should run, not block (even though it's a no-op rm on nonexistent)
    std::string result = cmd_exec("echo confirmed_test", /*confirmed=*/true);
    CHECK(result == "confirmed_test");
}

TEST_CASE("cmd_exec truncates output at 32KB") {
    // Generate output larger than 32KB
    std::string result = cmd_exec("yes | head -10000");
    CHECK(result.size() <= 32768 + 50); // 32KB + truncation message
}

// ---------------------------------------------------------------------------
// cmd_fetch — URL validation
// ---------------------------------------------------------------------------

TEST_CASE("cmd_fetch rejects non-http URLs") {
    CHECK(cmd_fetch("").find("ERR:") == 0);
    CHECK(cmd_fetch("ftp://example.com").find("ERR:") == 0);
    CHECK(cmd_fetch("file:///etc/passwd").find("ERR:") == 0);
    CHECK(cmd_fetch("javascript:alert(1)").find("ERR:") == 0);
    CHECK(cmd_fetch("not-a-url").find("ERR:") == 0);
}

TEST_CASE("cmd_fetch accepts http and https") {
    // These may fail due to DNS/network but should NOT fail on URL validation
    std::string http_result = cmd_fetch("http://localhost:1/nonexistent");
    std::string https_result = cmd_fetch("https://localhost:1/nonexistent");

    // They should fail with a connection error, not a URL validation error
    CHECK(http_result.find("URL must start with") == std::string::npos);
    CHECK(https_result.find("URL must start with") == std::string::npos);
}

// ---------------------------------------------------------------------------
// cmd_write — path traversal protection
// ---------------------------------------------------------------------------

TEST_CASE("cmd_write rejects path traversal") {
    CHECK(cmd_write("", "content").find("ERR:") == 0);

    // Relative traversal
    std::string result = cmd_write("../../etc/passwd", "bad");
    CHECK(result.find("ERR:") == 0);

    // Absolute path outside cwd
    std::string result2 = cmd_write("/tmp/rogue_file_test", "bad");
    CHECK(result2.find("ERR:") == 0);
}

TEST_CASE("cmd_write allows paths within cwd") {
    // Create a temp subdir to write into
    std::string test_dir = "test_cmd_write_tmp";
    fs::create_directories(test_dir);

    std::string result = cmd_write(test_dir + "/test_file.txt", "hello world");
    CHECK(result.find("OK:") == 0);
    CHECK(result.find("wrote") != std::string::npos);

    // Verify content
    std::ifstream f(test_dir + "/test_file.txt");
    std::string content;
    std::getline(f, content);
    CHECK(content == "hello world");

    // Clean up
    fs::remove_all(test_dir);
}

// ---------------------------------------------------------------------------
// parse_agent_commands
// ---------------------------------------------------------------------------

TEST_CASE("parse_agent_commands extracts commands") {
    std::string response = "Some text\n/fetch https://example.com\nMore text\n";
    auto cmds = parse_agent_commands(response);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].name == "fetch");
    CHECK(cmds[0].args == "https://example.com");
}

TEST_CASE("parse_agent_commands skips code fences") {
    std::string response = "```\n/fetch https://example.com\n```\n/exec ls\n";
    auto cmds = parse_agent_commands(response);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].name == "exec");
    CHECK(cmds[0].args == "ls");
}

TEST_CASE("parse_agent_commands fence: tilde fence skips commands inside") {
    std::string response = "~~~\n/exec rm -rf /\n~~~\n/exec echo ok\n";
    auto cmds = parse_agent_commands(response);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].name == "exec");
    CHECK(cmds[0].args == "echo ok");
}

TEST_CASE("parse_agent_commands fence: mismatched open/close does not escape block") {
    // Opened with ```, attempted close with ~~~ — block must stay open.
    // The /exec after the mismatch is still inside the fence.
    // Only a matching ``` can close it.
    std::string response = "```\n/exec danger\n~~~\n/exec still_inside\n```\n/exec outside\n";
    auto cmds = parse_agent_commands(response);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].name == "exec");
    CHECK(cmds[0].args == "outside");
}

TEST_CASE("parse_agent_commands fence: info string on opening line") {
    // ```python or ```cpp should open a fence just like a bare ```
    std::string response = "```python\n/exec echo inside\n```\n/exec echo outside\n";
    auto cmds = parse_agent_commands(response);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].name == "exec");
    CHECK(cmds[0].args == "echo outside");
}

TEST_CASE("parse_agent_commands fence: unclosed fence suppresses trailing commands") {
    // An unclosed ``` means everything after is inside the block.
    std::string response = "```\n/exec danger\n/exec also_danger\n";
    auto cmds = parse_agent_commands(response);
    CHECK(cmds.empty());
}

TEST_CASE("parse_agent_commands fence: nested open attempt is ignored") {
    // Once inside a ``` block, a second ``` line is not a new fence — it
    // is treated as a close of the outer block.
    std::string response = "```\n```\n/exec this_is_outside\n";
    auto cmds = parse_agent_commands(response);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].args == "this_is_outside");
}

TEST_CASE("parse_agent_commands fence: empty file is safe") {
    auto cmds = parse_agent_commands("");
    CHECK(cmds.empty());
}

TEST_CASE("parse_agent_commands fence: only fence markers, no commands") {
    std::string response = "```\n```\n";
    auto cmds = parse_agent_commands(response);
    CHECK(cmds.empty());
}

TEST_CASE("parse_agent_commands recognises /help with and without topic") {
    {
        auto cmds = parse_agent_commands("/help\n");
        REQUIRE(cmds.size() == 1);
        CHECK(cmds[0].name == "help");
        CHECK(cmds[0].args.empty());
    }
    {
        auto cmds = parse_agent_commands("/help mem\n");
        REQUIRE(cmds.size() == 1);
        CHECK(cmds[0].name == "help");
        CHECK(cmds[0].args == "mem");
    }
    {
        // Multi-token args are accepted by the parser; the dispatcher takes
        // the first token to resolve the topic (e.g. "/help mem add" → "mem").
        auto cmds = parse_agent_commands("/help mem add\n");
        REQUIRE(cmds.size() == 1);
        CHECK(cmds[0].name == "help");
        CHECK(cmds[0].args == "mem add");
    }
}

TEST_CASE("parse_agent_commands handles /mem add entry as a /endmem-terminated block") {
    {
        // Happy path: header line, body, /endmem.
        std::string response =
            "/mem add entry reference Honeycomb pricing\n"
            "Pro tier $130/mo for 100M events.  Span:trace 5–20 typical.\n"
            "Source: live fetch.\n"
            "/endmem\n";
        auto cmds = parse_agent_commands(response);
        REQUIRE(cmds.size() == 1);
        CHECK(cmds[0].name == "mem");
        CHECK(cmds[0].args == "add entry reference Honeycomb pricing");
        CHECK(cmds[0].content.find("Pro tier $130") != std::string::npos);
        CHECK(cmds[0].content.find("/endmem")       == std::string::npos);
        CHECK(cmds[0].truncated == false);
    }
    {
        // Truncated: header but no /endmem.  cmd.truncated must be true so
        // the orchestrator can decide to continue rather than commit
        // a half-written entry.
        std::string response =
            "/mem add entry reference Half-written\n"
            "Body in progress, model ran out of tokens";
        auto cmds = parse_agent_commands(response);
        REQUIRE(cmds.size() == 1);
        CHECK(cmds[0].name == "mem");
        CHECK(cmds[0].truncated == true);
    }
    {
        // /mem add link is single-line; the parser must NOT swallow
        // following lines as a block.
        std::string response =
            "/mem add link 88 supports 42\n"
            "/exec ls\n";
        auto cmds = parse_agent_commands(response);
        REQUIRE(cmds.size() == 2);
        CHECK(cmds[0].name == "mem");
        CHECK(cmds[0].args == "add link 88 supports 42");
        CHECK(cmds[0].content.empty());
        CHECK(cmds[1].name == "exec");
    }
    {
        // /mem read / /mem search / /mem entries stay single-line — the
        // /mem add entry block path must not engage on other /mem subforms.
        std::string response =
            "/mem search observability\n"
            "/mem entry 42\n";
        auto cmds = parse_agent_commands(response);
        REQUIRE(cmds.size() == 2);
        CHECK(cmds[0].args == "search observability");
        CHECK(cmds[1].args == "entry 42");
        CHECK(cmds[0].content.empty());
        CHECK(cmds[1].content.empty());
    }
    {
        // /mem invalidate is single-line.  Prefix collision check: starts
        // with "/mem add entry"-substring would hijack via the block path
        // (/mem add entry vs /mem invalidate share length but differ at
        // byte 5), so this guard pins the parser branch.
        std::string response =
            "/mem invalidate 17\n"
            "/exec ls\n";
        auto cmds = parse_agent_commands(response);
        REQUIRE(cmds.size() == 2);
        CHECK(cmds[0].name == "mem");
        CHECK(cmds[0].args == "invalidate 17");
        CHECK(cmds[0].content.empty());
        CHECK(cmds[0].truncated == false);
        CHECK(cmds[1].name == "exec");
    }
    {
        // /mem search --rerank is still single-line.  The flag travels
        // through `cmd.args` verbatim and is parsed by the dispatcher
        // (api_server's reader callback), not by parse_agent_commands.
        std::string response =
            "/mem search auth flow --rerank\n"
            "/exec ls\n";
        auto cmds = parse_agent_commands(response);
        REQUIRE(cmds.size() == 2);
        CHECK(cmds[0].name == "mem");
        CHECK(cmds[0].args == "search auth flow --rerank");
        CHECK(cmds[0].content.empty());
        CHECK(cmds[0].truncated == false);
        CHECK(cmds[1].name == "exec");
    }
}

TEST_CASE("/mem add entry dispatcher rejects empty body and unclosed block") {
    // Stub writer captures (kind, args, body) so we can inspect what the
    // dispatcher passes through.  Returns "OK: stubbed" so the dispatcher
    // treats it as a successful write.
    std::string captured_kind, captured_args, captured_body, captured_caller;
    auto writer = [&](const std::string& k, const std::string& a,
                      const std::string& b, const std::string& cid)
                      -> std::string {
        captured_kind   = k;
        captured_args   = a;
        captured_body   = b;
        captured_caller = cid;
        return "OK: stubbed entry recorded\n";
    };

    {
        // Block with body — should reach the writer with body intact.
        std::vector<AgentCommand> cmds;
        AgentCommand c;
        c.name = "mem";
        c.args = "add entry reference Test title";
        c.content = "Body line one.\nBody line two.";
        c.truncated = false;
        cmds.push_back(c);

        captured_kind.clear();
        auto out = execute_agent_commands(
            cmds, "test", "",
            /*agent_invoker=*/nullptr, /*confirm=*/nullptr,
            /*dedup_cache=*/nullptr, /*advisor_invoker=*/nullptr,
            /*tool_status=*/nullptr, /*pane_spawner=*/nullptr,
            /*write_interceptor=*/nullptr, /*exec_disabled=*/false,
            /*parallel_invoker=*/nullptr,
            /*structured_memory_reader=*/nullptr,
            /*structured_memory_writer=*/writer);

        // The dispatcher consumes "entry" before invoking the writer, so
        // `args` here is the type+title remainder, not the full second token.
        CHECK(captured_kind == "add-entry");
        CHECK(captured_args == "reference Test title");
        CHECK(captured_body == "Body line one.\nBody line two.");
        CHECK(out.find("OK: stubbed") != std::string::npos);
    }
    {
        // Empty body — dispatcher must refuse before reaching the writer.
        captured_kind.clear();
        std::vector<AgentCommand> cmds;
        AgentCommand c;
        c.name = "mem";
        c.args = "add entry reference Title only";
        c.content = "";
        c.truncated = false;
        cmds.push_back(c);

        auto out = execute_agent_commands(
            cmds, "test", "",
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            false, nullptr, nullptr, writer);
        CHECK(captured_kind.empty());   // writer was NOT called
        CHECK(out.find("ERR: /mem add entry requires a content body") != std::string::npos);
    }
    {
        // Truncated block (no /endmem) — dispatcher must refuse.
        captured_kind.clear();
        std::vector<AgentCommand> cmds;
        AgentCommand c;
        c.name = "mem";
        c.args = "add entry reference Truncated";
        c.content = "Half a body";
        c.truncated = true;
        cmds.push_back(c);

        auto out = execute_agent_commands(
            cmds, "test", "",
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            false, nullptr, nullptr, writer);
        CHECK(captured_kind.empty());
        CHECK(out.find("not closed with /endmem") != std::string::npos);
    }
    {
        // /mem add link does NOT require a body.  Passes args through.
        captured_kind.clear();
        std::vector<AgentCommand> cmds;
        AgentCommand c;
        c.name = "mem";
        c.args = "add link 1 supports 2";
        c.content = "";
        c.truncated = false;
        cmds.push_back(c);

        auto out = execute_agent_commands(
            cmds, "test", "",
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            false, nullptr, nullptr, writer);
        // Same convention as add-entry: dispatcher strips the "link" token.
        CHECK(captured_kind == "add-link");
        CHECK(captured_args == "1 supports 2");
        CHECK(captured_body.empty());
    }
}

TEST_CASE("/parallel duplicate agent_id produces explicit error") {
    // When the same agent_id appears more than once in a /parallel block,
    // execute_agent_commands must reject it with the exact error string
    // defined in commands.cpp before calling the parallel_invoker.

    // Build a /parallel command whose body lists the same agent twice.
    AgentCommand cmd;
    cmd.name = "parallel";
    cmd.args = "";
    cmd.content = "/agent researcher find patterns\n/agent researcher find more\n";
    cmd.truncated = false;

    std::vector<AgentCommand> cmds;
    cmds.push_back(cmd);

    // The parallel_invoker must NOT be called — set it to one that panics
    // so we detect any accidental call.
    bool invoker_called = false;
    auto guard_invoker = [&](const std::vector<std::pair<std::string, std::string>>&)
        -> std::vector<std::string> {
        invoker_called = true;
        return {};
    };

    auto out = execute_agent_commands(
        cmds, "index", "",
        /*agent_invoker=*/nullptr, /*confirm=*/nullptr,
        /*dedup_cache=*/nullptr, /*advisor_invoker=*/nullptr,
        /*tool_status=*/nullptr, /*pane_spawner=*/nullptr,
        /*write_interceptor=*/nullptr, /*exec_disabled=*/false,
        /*parallel_invoker=*/guard_invoker);

    CHECK(!invoker_called);
    CHECK(out.find("ERR: /parallel cannot invoke 'researcher' twice in one batch") != std::string::npos);
    CHECK(out.find("each agent_id must appear at most once") != std::string::npos);
}

TEST_CASE("/help dispatch returns topic body or ERR for unknown topic") {
    // The /help dispatch path needs no callbacks — it reads from the
    // help corpus baked into commands.cpp.  Smoke-test the three shapes:
    //   - no topic           → topic index
    //   - known topic        → topic body
    //   - unknown topic      → ERR with caller-facing hint

    {
        std::vector<AgentCommand> cmds;
        AgentCommand h; h.name = "help"; h.args = "";
        cmds.push_back(h);
        auto result = execute_agent_commands(cmds, "test", "");
        CHECK(result.find("[/help]") != std::string::npos);
        CHECK(result.find("Available topics:") != std::string::npos);
        CHECK(result.find("[END HELP]") != std::string::npos);
    }
    {
        std::vector<AgentCommand> cmds;
        AgentCommand h; h.name = "help"; h.args = "mem";
        cmds.push_back(h);
        auto result = execute_agent_commands(cmds, "test", "");
        CHECK(result.find("[/help mem]") != std::string::npos);
        // Anchor on a string from the mem topic body so we know the right
        // corpus entry was emitted.
        CHECK(result.find("/mem entries") != std::string::npos);
    }
    {
        std::vector<AgentCommand> cmds;
        AgentCommand h; h.name = "help"; h.args = "definitely-not-a-topic";
        cmds.push_back(h);
        auto result = execute_agent_commands(cmds, "test", "");
        CHECK(result.find("ERR: unknown topic") != std::string::npos);
    }
    {
        // First-token resolution: "/help mem add" should resolve to the
        // "mem" topic, not error out on the multi-word args.
        std::vector<AgentCommand> cmds;
        AgentCommand h; h.name = "help"; h.args = "Mem Add";  // also case-insensitive
        cmds.push_back(h);
        auto result = execute_agent_commands(cmds, "test", "");
        CHECK(result.find("/mem entries") != std::string::npos);
        CHECK(result.find("ERR:") == std::string::npos);
    }
}

TEST_CASE("tool_status_label emits accurate sidebar labels") {
    AgentCommand cmd;

    cmd.name = "exec";
    cmd.args = "git status --short";
    CHECK(tool_status_label(cmd) == "exec:git status --short");

    cmd.name = "fetch";
    cmd.args = "https://example.com/docs";
    CHECK(tool_status_label(cmd) == "fetch:https://example.com/docs");

    cmd.name = "mcp";
    cmd.args = "call playwright browser_navigate {\"url\":\"x\"}";
    CHECK(tool_status_label(cmd) == "mcp:playwright.browser_navigate");

    cmd.name = "todo";
    cmd.args = "add Fix auth middleware";
    CHECK(tool_status_label(cmd) == "todo:add Fix auth middleware");

    cmd.name = "schedule";
    cmd.args = "in 1 hour: run nightly tests";
    CHECK(tool_status_label(cmd) == "schedule:create in 1 hour: run nightly tests");

    cmd.name = "write";
    cmd.args = "--persist src/main.cpp";
    CHECK(tool_status_label(cmd) == "write:src/main.cpp");
}

TEST_CASE("tool_activity_detail truncates args for event rows") {
    AgentCommand cmd;
    cmd.name = "exec";
    cmd.args = "git status --short";
    CHECK(tool_activity_detail(cmd) == "git status --short");

    cmd.name = "write";
    cmd.args = "--persist src/main.cpp";
    CHECK(tool_activity_detail(cmd) == "src/main.cpp");
}

TEST_CASE("tool_result_preview strips framing and collapses newlines") {
    const std::string block =
        "[/exec git status]\n"
        "M src/main.cpp\n"
        "?? notes.txt\n"
        "[END EXEC]\n\n";
    const auto preview = tool_result_preview(block);
    CHECK(preview.find("[/exec") == std::string::npos);
    CHECK(preview.find("[END") == std::string::npos);
    CHECK(preview.find("M src/main.cpp") != std::string::npos);
    CHECK(preview.find('\n') == std::string::npos);
}

TEST_CASE("execute_agent_commands emits Started then Finished tool events") {
    std::vector<ToolActivityEvent> events;
    ToolStatusFn capture = [&](const ToolActivityEvent& ev) {
        events.push_back(ev);
    };

    std::vector<AgentCommand> cmds;
    AgentCommand h;
    h.name = "help";
    h.args = "mem";  // topic whose body does not contain the literal "ERR:"
    cmds.push_back(h);

    auto result = execute_agent_commands(
        cmds, "test", "",
        /*agent_invoker=*/nullptr,
        /*confirm=*/nullptr,
        /*dedup_cache=*/nullptr,
        /*advisor_invoker=*/nullptr,
        capture);
    CHECK(result.find("[/help") != std::string::npos);
    REQUIRE(events.size() == 2);
    CHECK(events[0].phase == ToolActivityEvent::Phase::Started);
    CHECK(events[0].kind == "help");
    CHECK_FALSE(events[0].id.empty());
    CHECK(events[1].phase == ToolActivityEvent::Phase::Finished);
    CHECK(events[1].id == events[0].id);
    CHECK(events[1].ok);
    CHECK(events[1].label == tool_status_label(h));
}

TEST_CASE("tool event ids are unique across execute_agent_commands calls") {
    std::vector<std::string> ids;
    ToolStatusFn capture = [&](const ToolActivityEvent& ev) {
        if (ev.phase == ToolActivityEvent::Phase::Started) ids.push_back(ev.id);
    };
    std::vector<AgentCommand> cmds;
    AgentCommand h;
    h.name = "help";
    h.args = "mem";
    cmds.push_back(h);
    execute_agent_commands(cmds, "test", "", nullptr, nullptr, nullptr, nullptr, capture);
    execute_agent_commands(cmds, "test", "", nullptr, nullptr, nullptr, nullptr, capture);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] != ids[1]);
}

TEST_CASE("write confirm request carries path summary and preview lines") {
    ConfirmRequest seen;
    bool called = false;
    ConfirmFn capture = [&](const ConfirmRequest& req) {
        called = true;
        seen = req;
        return false;  // decline so we don't touch disk
    };

    std::vector<AgentCommand> cmds;
    AgentCommand w;
    w.name = "write";
    w.args = "tmp_confirm_card.txt";
    w.content = "line one\nline two\nline three\n";
    cmds.push_back(w);

    auto result = execute_agent_commands(
        cmds, "test", "",
        nullptr, capture);
    CHECK(called);
    CHECK(seen.action == "write");
    CHECK(seen.target == "tmp_confirm_card.txt");
    CHECK(seen.summary.find("lines") != std::string::npos);
    REQUIRE_FALSE(seen.preview_lines.empty());
    CHECK(seen.preview_lines.front().find("line one") != std::string::npos);
    CHECK(result.find("user declined") != std::string::npos);
}
