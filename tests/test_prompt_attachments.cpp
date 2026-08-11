// Unit tests for TUI prompt image-drop /attach helpers.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "repl/prompt_attachments.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using arbiter::PasteImageResult;
using arbiter::PromptAttachment;
using arbiter::attachment_status_label;
using arbiter::extract_images_from_paste;
using arbiter::load_image_attachment;
using arbiter::mime_for_image_path;
using arbiter::path_looks_like_image;

namespace {

// Minimal 1×1 PNG (same fixture used by vision body tests).
constexpr unsigned char kPngBytes[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x05, 0xFE, 0xD4, 0xEF, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

struct TempPng {
    std::filesystem::path path;
    explicit TempPng(const std::string& name) {
        path = std::filesystem::temp_directory_path() / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(kPngBytes),
                  static_cast<std::streamsize>(sizeof(kPngBytes)));
    }
    ~TempPng() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_CASE("mime_for_image_path recognizes common extensions") {
    CHECK(mime_for_image_path("a.PNG") == "image/png");
    CHECK(mime_for_image_path("/tmp/x.jpeg") == "image/jpeg");
    CHECK(mime_for_image_path("y.webp") == "image/webp");
    CHECK(mime_for_image_path("z.gif") == "image/gif");
    CHECK(mime_for_image_path("readme.md").empty());
    CHECK(path_looks_like_image("shot.png"));
    CHECK_FALSE(path_looks_like_image("shot.txt"));
}

TEST_CASE("load_image_attachment reads a png into base64") {
    TempPng png("arbiter_attach_test.png");
    PromptAttachment att;
    std::string err;
    REQUIRE(load_image_attachment(png.path.string(), att, err));
    CHECK(err.empty());
    CHECK(att.media_type == "image/png");
    CHECK(att.label == "arbiter_attach_test.png");
    CHECK_FALSE(att.image_data.empty());
    // Base64 of the fixture starts with iVBORw0KGgo (PNG magic).
    CHECK(att.image_data.rfind("iVBORw0KGgo", 0) == 0);
}

TEST_CASE("load_image_attachment rejects missing and non-image paths") {
    PromptAttachment att;
    std::string err;
    CHECK_FALSE(load_image_attachment("/no/such/file.png", att, err));
    CHECK(err.find("not found") != std::string::npos);

    TempPng png("arbiter_attach_test2.png");
    auto txt = png.path;
    txt.replace_extension(".txt");
    {
        std::ofstream out(txt);
        out << "hello";
    }
    CHECK_FALSE(load_image_attachment(txt.string(), att, err));
    CHECK(err.find("supported image") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(txt, ec);
}

TEST_CASE("extract_images_from_paste consumes a bare image path drop") {
    TempPng png("arbiter_paste_drop.png");
    auto result = extract_images_from_paste(png.path.string());
    CHECK(result.remaining_text.empty());
    REQUIRE(result.attachments.size() == 1);
    CHECK(result.attachments[0].label == "arbiter_paste_drop.png");
    CHECK(result.errors.empty());
}

TEST_CASE("extract_images_from_paste handles file:// and quotes") {
    TempPng png("arbiter_paste_uri.png");
    const std::string uri = "file://" + png.path.string();
    auto result = extract_images_from_paste("'" + uri + "'");
    CHECK(result.remaining_text.empty());
    REQUIRE(result.attachments.size() == 1);
    CHECK(result.attachments[0].media_type == "image/png");
}

TEST_CASE("extract_images_from_paste leaves ordinary prose alone") {
    auto result = extract_images_from_paste("please look at shot.png carefully");
    CHECK(result.attachments.empty());
    CHECK(result.remaining_text == "please look at shot.png carefully");
}

TEST_CASE("extract_images_from_paste leaves prose with trailing image URL alone") {
    auto result = extract_images_from_paste("see https://example.com/foo.png");
    CHECK(result.attachments.empty());
    CHECK(result.errors.empty());
    CHECK(result.remaining_text == "see https://example.com/foo.png");
}

TEST_CASE("extract_images_from_paste loads multiline multi-file drops") {
    TempPng a("arbiter_multi_a.png");
    TempPng b("arbiter_multi_b.png");
    const std::string paste = a.path.string() + "\n" + b.path.string() + "\n";
    auto result = extract_images_from_paste(paste);
    CHECK(result.remaining_text.empty());
    REQUIRE(result.attachments.size() == 2);
    CHECK(attachment_status_label(result.attachments)
              .find("arbiter_multi_a.png") != std::string::npos);
}
