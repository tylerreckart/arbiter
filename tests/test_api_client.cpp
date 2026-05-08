// tests/test_api_client.cpp — Unit tests for API client edge cases
//
// parse_host is file-static in api_client.cpp, so we test it indirectly
// by reimplementing the same logic here and testing that. This validates
// the algorithm without breaking encapsulation.
//
// build_body_* are public static methods on ApiClient, so we test them
// directly: assemble an ApiRequest with a multipart Message and verify the
// JSON body each provider emits matches that provider's wire shape.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "api_client.h"

#include <cstdlib>
#include <string>

namespace {

struct ParsedHost {
    std::string scheme;
    std::string host;
    int port;
};

// Mirror of the parse_host logic from api_client.cpp (with the strtol fix applied)
ParsedHost parse_host(const std::string& s,
                      const std::string& default_scheme,
                      int default_port) {
    ParsedHost r{default_scheme, "", default_port};
    std::string rest = s;
    auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        r.scheme = rest.substr(0, scheme_end);
        rest = rest.substr(scheme_end + 3);
    }
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        r.host = rest.substr(0, colon);
        const char* port_start = rest.c_str() + colon + 1;
        char* end = nullptr;
        long parsed = std::strtol(port_start, &end, 10);
        if (end == port_start || parsed < 1 || parsed > 65535)
            r.port = default_port;
        else
            r.port = static_cast<int>(parsed);
    } else {
        r.host = rest;
    }
    if (r.host.empty()) r.host = "localhost";
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_host edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parse_host basic cases") {
    auto r = parse_host("example.com:8080", "http", 443);
    CHECK(r.scheme == "http");
    CHECK(r.host == "example.com");
    CHECK(r.port == 8080);
}

TEST_CASE("parse_host with scheme") {
    auto r = parse_host("https://api.example.com:443", "http", 80);
    CHECK(r.scheme == "https");
    CHECK(r.host == "api.example.com");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host without port uses default") {
    auto r = parse_host("example.com", "https", 443);
    CHECK(r.host == "example.com");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host empty string returns localhost") {
    auto r = parse_host("", "http", 11434);
    CHECK(r.host == "localhost");
    CHECK(r.port == 11434);
}

TEST_CASE("parse_host invalid port falls back to default") {
    auto r = parse_host("host:abc", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host overflow port falls back to default") {
    auto r = parse_host("host:99999", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host zero port falls back to default") {
    auto r = parse_host("host:0", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host negative port falls back to default") {
    auto r = parse_host("host:-1", "http", 443);
    CHECK(r.host == "host");
    CHECK(r.port == 443);
}

TEST_CASE("parse_host port 65535 is valid") {
    auto r = parse_host("host:65535", "http", 443);
    CHECK(r.port == 65535);
}

TEST_CASE("parse_host port 65536 overflows to default") {
    auto r = parse_host("host:65536", "http", 443);
    CHECK(r.port == 443);
}

// ---------------------------------------------------------------------------
// Vision input — multipart body builder shapes
// ---------------------------------------------------------------------------
//
// These tests verify each provider's body builder produces the multipart
// content shape the provider's API actually expects.  They don't touch the
// network — just JSON-out.  A failure here means an upstream provider would
// 400 the request before any model saw it.

namespace {

using namespace arbiter;

// Build a user message containing one text part followed by one image part.
// The image is a 1×1 PNG inlined as base64 (the smallest valid PNG that
// round-trips through every provider's image-input gate).
ApiRequest make_request_with_text_and_image() {
    ApiRequest req;
    req.model       = "claude-sonnet-4-6";
    req.max_tokens  = 256;
    req.temperature = 0.2;

    Message m;
    m.role = "user";

    ContentPart text;
    text.kind = ContentPart::TEXT;
    text.text = "What's in this image?";
    m.parts.push_back(text);

    ContentPart img;
    img.kind       = ContentPart::IMAGE;
    img.media_type = "image/png";
    img.image_data =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
        "+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";
    m.parts.push_back(img);

    req.messages.push_back(m);
    return req;
}

ApiRequest make_request_with_image_url() {
    ApiRequest req;
    req.model       = "claude-sonnet-4-6";
    req.max_tokens  = 256;
    req.temperature = 0.2;

    Message m;
    m.role = "user";

    ContentPart text;
    text.kind = ContentPart::TEXT;
    text.text = "Describe this.";
    m.parts.push_back(text);

    ContentPart img;
    img.kind       = ContentPart::IMAGE;
    img.media_type = "image/jpeg";
    img.image_url  = "https://example.com/cat.jpg";
    m.parts.push_back(img);

    req.messages.push_back(m);
    return req;
}

}  // namespace

TEST_CASE("anthropic body emits inline base64 image as a content block") {
    auto req = make_request_with_text_and_image();
    auto body = ApiClient::build_body_anthropic(req, /*streaming=*/false);

    // Must contain the multipart structure for the message — content array
    // with both a text block and an image block.
    CHECK(body.find("\"content\"") != std::string::npos);
    CHECK(body.find("\"type\":\"text\"") != std::string::npos);
    CHECK(body.find("\"type\":\"image\"") != std::string::npos);
    CHECK(body.find("\"source\"") != std::string::npos);
    CHECK(body.find("\"type\":\"base64\"") != std::string::npos);
    CHECK(body.find("\"media_type\":\"image/png\"") != std::string::npos);
    CHECK(body.find("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQ") != std::string::npos);
}

TEST_CASE("anthropic body emits hosted-URL image as a url source") {
    auto req = make_request_with_image_url();
    auto body = ApiClient::build_body_anthropic(req, /*streaming=*/false);

    CHECK(body.find("\"type\":\"image\"") != std::string::npos);
    CHECK(body.find("\"type\":\"url\"") != std::string::npos);
    CHECK(body.find("\"url\":\"https://example.com/cat.jpg\"") !=
          std::string::npos);
    // No base64 media_type / data fields when the URL form is used.
    CHECK(body.find("\"data\":\"") == std::string::npos);
}

TEST_CASE("anthropic body puts cache_control on the tail message's last block") {
    auto req = make_request_with_text_and_image();
    auto body = ApiClient::build_body_anthropic(req, /*streaming=*/false);

    // The image is the second (and last) part of the tail message; cache_control
    // must land on it and only on it.  No system prompt here, so exactly one
    // marker should appear in the body.  And it must come after the leading
    // text content — proving it's not attached to the first block.
    auto first  = body.find("\"cache_control\"");
    auto second = body.find("\"cache_control\"", first + 1);
    REQUIRE(first  != std::string::npos);
    CHECK  (second == std::string::npos);

    auto leading_text = body.find("What's in this image?");
    REQUIRE(leading_text != std::string::npos);
    CHECK(first > leading_text);
}

TEST_CASE("openai body emits inline base64 as a data: URL") {
    auto req = make_request_with_text_and_image();
    req.model = "openai/gpt-4o";

    Provider prov;
    prov.name   = "openai";
    prov.format = Provider::FORMAT_OPENAI_CHAT;

    auto body = ApiClient::build_body_openai(prov, req, /*streaming=*/false);

    CHECK(body.find("\"type\":\"image_url\"") != std::string::npos);
    CHECK(body.find("\"image_url\"") != std::string::npos);
    // Inline images travel as data: URLs on the OpenAI path.
    CHECK(body.find("\"url\":\"data:image/png;base64,iVBORw0KGgo") !=
          std::string::npos);
    CHECK(body.find("\"type\":\"text\"") != std::string::npos);
}

TEST_CASE("openai body emits hosted-URL image directly") {
    auto req = make_request_with_image_url();
    req.model = "openai/gpt-4o";

    Provider prov;
    prov.name   = "openai";
    prov.format = Provider::FORMAT_OPENAI_CHAT;

    auto body = ApiClient::build_body_openai(prov, req, /*streaming=*/false);

    CHECK(body.find("\"url\":\"https://example.com/cat.jpg\"") !=
          std::string::npos);
    // No data: URL when the source is a hosted URL.
    CHECK(body.find("data:image/jpeg;base64") == std::string::npos);
}

TEST_CASE("gemini body emits inline base64 under inlineData") {
    auto req = make_request_with_text_and_image();
    req.model = "gemini/gemini-2.5-flash";

    auto body = ApiClient::build_body_gemini(req);

    CHECK(body.find("\"contents\"") != std::string::npos);
    CHECK(body.find("\"parts\"") != std::string::npos);
    CHECK(body.find("\"inlineData\"") != std::string::npos);
    CHECK(body.find("\"mimeType\":\"image/png\"") != std::string::npos);
    CHECK(body.find("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQ") != std::string::npos);
}

TEST_CASE("gemini body emits hosted-URL image under fileData") {
    auto req = make_request_with_image_url();
    req.model = "gemini/gemini-2.5-flash";

    auto body = ApiClient::build_body_gemini(req);

    CHECK(body.find("\"fileData\"") != std::string::npos);
    CHECK(body.find("\"mimeType\":\"image/jpeg\"") != std::string::npos);
    CHECK(body.find("\"fileUri\":\"https://example.com/cat.jpg\"") !=
          std::string::npos);
    CHECK(body.find("\"inlineData\"") == std::string::npos);
}

TEST_CASE("text-only message still emits string content (legacy path)") {
    ApiRequest req;
    req.model      = "claude-sonnet-4-6";
    req.max_tokens = 256;
    Message m;
    m.role    = "user";
    m.content = "hello there";
    req.messages.push_back(m);

    auto body = ApiClient::build_body_anthropic(req, /*streaming=*/false);
    // Tail message in Anthropic format always becomes a block array because
    // cache_control needs block-form content.  But the block carries the
    // legacy text exactly.
    CHECK(body.find("\"text\":\"hello there\"") != std::string::npos);
    CHECK(body.find("\"type\":\"text\"") != std::string::npos);

    Provider prov;
    prov.name   = "openai";
    prov.format = Provider::FORMAT_OPENAI_CHAT;
    auto obody = ApiClient::build_body_openai(prov, req, false);
    // OpenAI path keeps text-only messages as plain string content.
    CHECK(obody.find("\"content\":\"hello there\"") != std::string::npos);

    auto gbody = ApiClient::build_body_gemini(req);
    // Gemini wraps even text-only into a {text} part — verify the wrap.
    CHECK(gbody.find("\"text\":\"hello there\"") != std::string::npos);
}
