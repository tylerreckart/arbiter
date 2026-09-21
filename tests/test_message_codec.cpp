#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "message_codec.h"

using namespace arbiter;

TEST_CASE("decode_messages_json skips JSON null and other non-objects") {
    const auto restored = decode_messages_json(
        R"([null,"skip",1,true,[],{"role":"user","content":"keep"},{"role":"assistant","content":"also"}])");
    REQUIRE(restored.size() == 2);
    CHECK(restored[0].role == "user");
    CHECK(restored[0].content == "keep");
    CHECK(restored[0].thinking.empty());
    CHECK(restored[0].tool_trace.empty());
    CHECK(restored[1].role == "assistant");
    CHECK(restored[1].content == "also");
}

TEST_CASE("decode_messages_json all-non-object array is empty, not empty-role rows") {
    const auto restored = decode_messages_json(R"([null, "x", 0])");
    CHECK(restored.empty());
}

TEST_CASE("decode_messages_json still round-trips object rows") {
    Message user;
    user.role = "user";
    user.content = "do the thing";
    Message asst;
    asst.role = "assistant";
    asst.content = "done";
    asst.thinking = "step";
    const auto back = decode_messages_json(encode_messages_json({user, asst}));
    REQUIRE(back.size() == 2);
    CHECK(back[0].role == "user");
    CHECK(back[0].content == "do the thing");
    CHECK(back[1].role == "assistant");
    CHECK(back[1].thinking == "step");
}
