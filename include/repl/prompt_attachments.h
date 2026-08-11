#pragma once
// Image attachments for the TUI prompt / readline.
//
// Terminals deliver drag-and-drop as bracketed-paste of a file path (or
// file:// URI).  These helpers detect image paths, load them as base64
// ContentPart-compatible blobs, and keep a display label for the input
// chrome / user-echo.  The runtime already speaks vision via ContentPart;
// this is the missing TUI ingest path.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace arbiter {

// Cap matches the API URL-image fetch limit (20 MB).  Large enough for
// screenshots; small enough that a mistaken drop of a video doesn't blow
// the process heap.
inline constexpr std::size_t kPromptImageMaxBytes = 20ULL * 1024 * 1024;

struct PromptAttachment {
    std::string label;       // basename for chrome / echo
    std::string path;        // resolved absolute path (when known)
    std::string media_type;  // image/png | image/jpeg | …
    std::string image_data;  // raw base64, no data: prefix
};

struct PasteImageResult {
    // Text that should still land in the editor (non-image leftovers).
    std::string remaining_text;
    std::vector<PromptAttachment> attachments;
    std::vector<std::string> errors;
};

// True when `mime` starts with "image/" (case-sensitive; callers normalize).
[[nodiscard]] bool is_image_media_type(std::string_view mime);

// Extension → MIME.  Empty when the path does not look like a supported
// image (png/jpg/jpeg/gif/webp).
[[nodiscard]] std::string mime_for_image_path(std::string_view path);

[[nodiscard]] bool path_looks_like_image(std::string_view path);

// Read `path` into `out`.  Returns false and sets `err` on missing file,
// non-image, oversized, or I/O failure.
[[nodiscard]] bool load_image_attachment(const std::string& path,
                                         PromptAttachment& out,
                                         std::string& err);

// Inspect bracketed-paste (or similar) text.  When the paste is entirely
// one or more image file paths (newlines / whitespace separated, optional
// quotes / file://), loads them as attachments and leaves remaining_text
// empty.  Mixed prose is left untouched so ordinary paste stays literal.
[[nodiscard]] PasteImageResult extract_images_from_paste(std::string_view paste);

// Short status / echo label: "shot.png" or "shot.png +2".
[[nodiscard]] std::string attachment_status_label(
    const std::vector<PromptAttachment>& attachments);

} // namespace arbiter
