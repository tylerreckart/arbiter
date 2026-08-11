// Image attachments for the TUI prompt / readline.
#include "repl/prompt_attachments.h"

#include "commands.h"  // base64_encode

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace arbiter {
namespace {

std::string to_lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string basename_of(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) return std::string(path);
    return std::string(path.substr(slash + 1));
}

void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

// Strip one layer of matching quotes and optional file:// prefix.
std::string normalize_path_token(std::string_view raw) {
    std::string s(raw);
    trim_inplace(s);
    if (s.size() >= 2) {
        const char a = s.front();
        const char b = s.back();
        if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
            s = s.substr(1, s.size() - 2);
            trim_inplace(s);
        }
    }
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
        // file:///abs/path → /abs/path; file://hostname/path is rare for drops.
        if (s.rfind("//", 0) == 0) {
            // file:////host/path — keep leading slash form if present later
            auto rest = s.substr(2);
            auto slash = rest.find('/');
            if (slash != std::string::npos) s = rest.substr(slash);
            else s = "/" + rest;
        }
    }
    // Unescape common terminal escape of spaces: "my\ file.png"
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            out.push_back(s[i + 1]);
            ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

bool file_exists_regular(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

// Split paste into candidate path tokens.  Prefer newline-separated drops
// (common for multi-file DnD); otherwise treat the whole paste as one token
// so paths with spaces stay intact when quoted.
std::vector<std::string> candidate_tokens(std::string_view paste) {
    std::string body(paste);
    // Normalize CRLF.
    std::string norm;
    norm.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\r') {
            if (i + 1 < body.size() && body[i + 1] == '\n') continue;
            norm.push_back('\n');
        } else {
            norm.push_back(body[i]);
        }
    }
    trim_inplace(norm);
    if (norm.empty()) return {};

    std::vector<std::string> lines;
    {
        std::istringstream iss(norm);
        std::string line;
        while (std::getline(iss, line)) {
            trim_inplace(line);
            if (!line.empty()) lines.push_back(std::move(line));
        }
    }
    if (lines.empty()) return {};

    // Multi-line paste: each line is a candidate.
    if (lines.size() > 1) return lines;

    // Single line: also try whitespace-split when every token looks like
    // an image path (multi-file drop without newlines).
    const std::string& one = lines[0];
    std::vector<std::string> ws;
    {
        std::string cur;
        bool in_squote = false;
        bool in_dquote = false;
        for (size_t i = 0; i < one.size(); ++i) {
            const char c = one[i];
            if (c == '\\' && i + 1 < one.size() && !in_squote) {
                cur.push_back(c);
                cur.push_back(one[++i]);
                continue;
            }
            if (c == '\'' && !in_dquote) { in_squote = !in_squote; cur.push_back(c); continue; }
            if (c == '"' && !in_squote) { in_dquote = !in_dquote; cur.push_back(c); continue; }
            if (!in_squote && !in_dquote && std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { ws.push_back(cur); cur.clear(); }
                continue;
            }
            cur.push_back(c);
        }
        if (!cur.empty()) ws.push_back(std::move(cur));
    }
    if (ws.size() <= 1) return lines;

    bool all_imageish = true;
    for (auto& t : ws) {
        const std::string p = normalize_path_token(t);
        if (!path_looks_like_image(p)) { all_imageish = false; break; }
    }
    return all_imageish ? ws : lines;
}

} // namespace

bool is_image_media_type(std::string_view mime) {
    return mime.size() >= 6 && mime.substr(0, 6) == "image/";
}

std::string mime_for_image_path(std::string_view path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext = to_lower_ascii(std::string(path.substr(dot + 1)));
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    return {};
}

bool path_looks_like_image(std::string_view path) {
    return !mime_for_image_path(path).empty();
}

bool load_image_attachment(const std::string& path,
                           PromptAttachment& out,
                           std::string& err) {
    err.clear();
    const std::string cleaned = normalize_path_token(path);
    if (cleaned.empty()) {
        err = "empty path";
        return false;
    }
    const std::string mime = mime_for_image_path(cleaned);
    if (mime.empty()) {
        err = "not a supported image type (png/jpeg/gif/webp): " + cleaned;
        return false;
    }
    if (!file_exists_regular(cleaned)) {
        err = "file not found: " + cleaned;
        return false;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(cleaned, ec);
    if (ec) {
        err = "cannot stat: " + cleaned;
        return false;
    }
    if (sz == 0) {
        err = "empty image file: " + cleaned;
        return false;
    }
    if (static_cast<std::size_t>(sz) > kPromptImageMaxBytes) {
        err = "image too large (max 20 MB): " + cleaned;
        return false;
    }
    std::ifstream in(cleaned, std::ios::binary);
    if (!in) {
        err = "cannot read: " + cleaned;
        return false;
    }
    std::string bytes(static_cast<std::size_t>(sz), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(sz));
    if (!in) {
        err = "read failed: " + cleaned;
        return false;
    }

    std::error_code canon_ec;
    auto canon = std::filesystem::weakly_canonical(cleaned, canon_ec);

    out.label = basename_of(cleaned);
    out.path = canon_ec ? cleaned : canon.string();
    out.media_type = mime;
    out.image_data = base64_encode(bytes);
    return true;
}

PasteImageResult extract_images_from_paste(std::string_view paste) {
    PasteImageResult result;
    auto tokens = candidate_tokens(paste);
    if (tokens.empty()) {
        result.remaining_text = std::string(paste);
        return result;
    }

    // Only hijack the paste when every candidate loads (or at least looks
    // like an image path).  Mixed prose → leave the paste alone.
    std::vector<PromptAttachment> loaded;
    std::vector<std::string> errors;
    for (auto& tok : tokens) {
        const std::string path = normalize_path_token(tok);
        if (!path_looks_like_image(path)) {
            result.remaining_text = std::string(paste);
            return result;
        }
        PromptAttachment att;
        std::string err;
        if (!load_image_attachment(path, att, err)) {
            // Path looks like an image but failed to load — still treat as
            // an image drop (don't dump the path into the buffer), surface
            // the error, and skip the bad file.
            errors.push_back(std::move(err));
            continue;
        }
        loaded.push_back(std::move(att));
    }

    if (loaded.empty() && errors.empty()) {
        result.remaining_text = std::string(paste);
        return result;
    }
    result.attachments = std::move(loaded);
    result.errors = std::move(errors);
    // remaining_text stays empty — paths were consumed as attachments.
    return result;
}

std::string attachment_status_label(
    const std::vector<PromptAttachment>& attachments) {
    if (attachments.empty()) return {};
    if (attachments.size() == 1) return "img " + attachments.front().label;
    return "img " + attachments.front().label
         + " +" + std::to_string(attachments.size() - 1);
}

} // namespace arbiter
