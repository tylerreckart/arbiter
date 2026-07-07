#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/transcript_replay.h"

using namespace arbiter;

TEST_CASE("replay_tail_begin keeps everything when history fits in the tail window") {
    CHECK(replay_tail_begin(0) == 0);
    CHECK(replay_tail_begin(1) == 0);
    CHECK(replay_tail_begin(kReplayTailMessages) == 0);
}

TEST_CASE("replay_tail_begin windows to the last kReplayTailMessages") {
    CHECK(replay_tail_begin(kReplayTailMessages + 1) == 1);
    CHECK(replay_tail_begin(kReplayTailMessages + 10) == 10);
    CHECK(replay_tail_begin(500) == 500 - kReplayTailMessages);
}

TEST_CASE("is_replay_noise skips [TOOL RESULTS] and [PANE RESULT] re-entry frames") {
    Message tool_result;
    tool_result.role = "user";
    tool_result.content = "\n[/fetch https://example.com]\nsome body\n[END TOOL RESULTS]";
    CHECK(is_replay_noise(tool_result));

    Message pane_result;
    pane_result.role = "user";
    pane_result.content = "[PANE RESULT from 'coder' (task: fix bug)]\nfixed\n[END PANE RESULT]";
    CHECK(is_replay_noise(pane_result));
}

TEST_CASE("is_replay_noise does not flag real user turns or assistant messages") {
    Message real_user;
    real_user.role = "user";
    real_user.content = "please refactor this function";
    CHECK_FALSE(is_replay_noise(real_user));

    Message assistant;
    assistant.role = "assistant";
    assistant.content = "[END TOOL RESULTS]"; // wrong role — must not match
    CHECK_FALSE(is_replay_noise(assistant));
}
