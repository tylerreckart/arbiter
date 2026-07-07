#pragma once

#include <string>

namespace arbiter {

// First user message -> an instant, free, deterministic title so a
// conversation is never stuck at "Untitled". Strips writ-style lines
// (lines starting with '/') and common markdown markers, collapses
// whitespace, and cuts at a word boundary at or under 40 display columns
// (display_width, not byte length), appending "…" when cut. Returns "" if
// nothing meaningful survives stripping — callers should leave the title
// as "Untitled" in that case rather than set an empty one.
std::string deterministic_conversation_title(const std::string& first_user_message);

// Sanitizes a model's raw title reply: keeps only the first line, strips
// surrounding quote/backtick/emphasis characters and a single trailing
// punctuation mark, and caps the result at 60 display columns. Returns ""
// if nothing meaningful survives — callers should keep the deterministic
// title in that case rather than overwrite it with junk.
std::string sanitize_model_title(const std::string& raw_reply);

// Reads "<config_dir>/title_model" (mirrors the master_model override
// file), trimmed. Returns "" if the file is absent/empty so callers fall
// back to the index agent's model.
std::string load_title_model_override(const std::string& config_dir);

} // namespace arbiter
