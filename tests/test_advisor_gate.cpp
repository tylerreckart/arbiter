// tests/test_advisor_gate.cpp — Unit tests for the advisor gate's signal
// parser.  Pins the wire format documented in commands.h: the parser is
// the only thing standing between an advisor reply and the runtime gate's
// CONTINUE/REDIRECT/HALT decision, so format drift here means the gate
// silently misroutes.
//
// Runtime-loop tests (REDIRECT injection, HALT escalation, budget cap)
// would need a live or mocked ApiClient.  Those live in higher-tier
// integration tests; this file pins the parser only.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "commands.h"

using namespace index_ai;

TEST_CASE("parse: bare CONTINUE") {
    auto out = parse_advisor_signal("<signal>CONTINUE</signal>");
    CHECK(out.kind == AdvisorGateOutput::Kind::Continue);
    CHECK(out.malformed == false);
    CHECK(out.text.empty());
}

TEST_CASE("parse: CONTINUE tolerates surrounding whitespace and prose") {
    auto out = parse_advisor_signal(
        "  some preamble\n"
        "<signal>CONTINUE</signal>\n"
        "trailing prose\n");
    CHECK(out.kind == AdvisorGateOutput::Kind::Continue);
    CHECK(out.malformed == false);
}

TEST_CASE("parse: REDIRECT extracts guidance body") {
    auto out = parse_advisor_signal(
        "<signal>REDIRECT</signal>\n"
        "<guidance>The executor narrowed the scope. Re-include constraint X.</guidance>");
    CHECK(out.kind == AdvisorGateOutput::Kind::Redirect);
    CHECK(out.text == "The executor narrowed the scope. Re-include constraint X.");
    CHECK(out.malformed == false);
}

TEST_CASE("parse: REDIRECT without guidance flags malformed") {
    auto out = parse_advisor_signal("<signal>REDIRECT</signal>");
    CHECK(out.kind == AdvisorGateOutput::Kind::Redirect);
    CHECK(out.malformed == true);
}

TEST_CASE("parse: HALT extracts reason body") {
    auto out = parse_advisor_signal(
        "<signal>HALT</signal>\n"
        "<reason>About to commit secrets to a public repo.</reason>");
    CHECK(out.kind == AdvisorGateOutput::Kind::Halt);
    CHECK(out.text == "About to commit secrets to a public repo.");
    CHECK(out.malformed == false);
}

TEST_CASE("parse: HALT without reason flags malformed") {
    auto out = parse_advisor_signal("<signal>HALT</signal>");
    CHECK(out.kind == AdvisorGateOutput::Kind::Halt);
    CHECK(out.malformed == true);
}

TEST_CASE("parse: signal token is case-insensitive") {
    auto a = parse_advisor_signal("<signal>continue</signal>");
    CHECK(a.kind == AdvisorGateOutput::Kind::Continue);
    CHECK(a.malformed == false);

    auto b = parse_advisor_signal("<signal>Halt</signal><reason>x</reason>");
    CHECK(b.kind == AdvisorGateOutput::Kind::Halt);
    CHECK(b.malformed == false);
}

TEST_CASE("parse: body retains its original casing") {
    auto out = parse_advisor_signal(
        "<signal>REDIRECT</signal><guidance>Use HTTPS, NOT http.</guidance>");
    CHECK(out.text == "Use HTTPS, NOT http.");
}

TEST_CASE("parse: missing signal tag flags malformed") {
    auto out = parse_advisor_signal("CONTINUE — looks good to me");
    CHECK(out.kind == AdvisorGateOutput::Kind::Continue);  // default
    CHECK(out.malformed == true);
    CHECK(out.raw == "CONTINUE — looks good to me");
}

TEST_CASE("parse: unrecognised signal value flags malformed") {
    auto out = parse_advisor_signal("<signal>WAIT</signal>");
    CHECK(out.malformed == true);
}

TEST_CASE("parse: body whitespace is trimmed") {
    auto out = parse_advisor_signal(
        "<signal>HALT</signal>\n"
        "<reason>\n  leading and trailing newlines  \n</reason>");
    CHECK(out.text == "leading and trailing newlines");
}

TEST_CASE("parse: empty input is malformed CONTINUE") {
    auto out = parse_advisor_signal("");
    CHECK(out.kind == AdvisorGateOutput::Kind::Continue);
    CHECK(out.malformed == true);
}

TEST_CASE("parse: raw is preserved verbatim for telemetry") {
    auto out = parse_advisor_signal("<signal>CONTINUE</signal>");
    CHECK(out.raw == "<signal>CONTINUE</signal>");
}
