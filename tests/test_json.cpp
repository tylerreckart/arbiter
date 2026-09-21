// tests/test_json.cpp — JSON string \u escapes, including UTF-16
// surrogate pairs.  Isolated from unit_mcp so this stays independently
// mergeable against open MCP framing PRs.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "json.h"

using namespace arbiter;

TEST_CASE("json_parse BMP \\u escapes") {
    auto v = json_parse(R"("caf\u00e9")");
    REQUIRE(v);
    REQUIRE(v->is_string());
    CHECK(v->as_string() == "café");
}

TEST_CASE("json_parse UTF-16 surrogate pair is one scalar") {
    // U+1F600 GRINNING FACE — Python json.dumps("😀") default form.
    auto v = json_parse(R"("\uD83D\uDE00")");
    REQUIRE(v);
    REQUIRE(v->is_string());
    CHECK(v->as_string() == "😀");
    CHECK(v->as_string().size() == 4);
    CHECK(static_cast<unsigned char>(v->as_string()[0]) == 0xF0);
    CHECK(static_cast<unsigned char>(v->as_string()[1]) == 0x9F);
    CHECK(static_cast<unsigned char>(v->as_string()[2]) == 0x98);
    CHECK(static_cast<unsigned char>(v->as_string()[3]) == 0x80);
}

TEST_CASE("json_parse lowercase surrogate pair") {
    auto v = json_parse(R"("\ud83d\ude00")");
    REQUIRE(v);
    CHECK(v->as_string() == "😀");
}

TEST_CASE("json_serialize of a decoded pair is well-formed UTF-8 JSON") {
    auto v = json_parse(R"("\uD83D\uDE00")");
    REQUIRE(v);
    const std::string wire = json_serialize(*v);
    CHECK(wire == "\"😀\"");
    auto round = json_parse(wire);
    REQUIRE(round);
    CHECK(round->as_string() == "😀");
}

TEST_CASE("json_parse unpaired high surrogate becomes U+FFFD") {
    auto v = json_parse(R"("\uD83D")");
    REQUIRE(v);
    CHECK(v->as_string() == "\xEF\xBF\xBD");
}

TEST_CASE("json_parse unpaired low surrogate becomes U+FFFD") {
    auto v = json_parse(R"("\uDE00")");
    REQUIRE(v);
    CHECK(v->as_string() == "\xEF\xBF\xBD");
}

TEST_CASE("json_parse high surrogate followed by BMP keeps the BMP") {
    auto v = json_parse(R"("\uD83D\u00e9")");
    REQUIRE(v);
    CHECK(v->as_string() == "\xEF\xBF\xBD" "é");
}

TEST_CASE("json_parse nested object with escaped emoji (MCP/A2A shape)") {
    auto v = json_parse(
        R"({"result":{"content":[{"type":"text","text":"hi \uD83D\uDE00"}]}})");
    REQUIRE(v);
    auto text = v->get("result")->get("content")->as_array().at(0)->get("text");
    REQUIRE(text);
    CHECK(text->as_string() == "hi 😀");
}

TEST_CASE("json_parse rejects truncated \\u hex") {
    CHECK_THROWS(json_parse(R"("\uD8")"));
    CHECK_THROWS(json_parse(R"("\uD83D\uDE0")"));
}
