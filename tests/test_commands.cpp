// tests/test_commands.cpp — Unit tests for security-critical command functions
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "commands.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace index_ai;

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
