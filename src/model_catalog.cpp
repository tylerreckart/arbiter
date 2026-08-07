#include "model_catalog.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace arbiter {

namespace {

// Ids are OpenRouter slugs (hosted traffic routes through OpenRouter).
// Keep short Claude aliases for back-compat with older agent JSON.
// context_window values match context_window_for_model heuristics for the
// same id so the API, sidebar, and compaction share one source of truth.
constexpr ModelCatalogEntry kModels[] = {
    // Anthropic Claude (OpenRouter)
    {"anthropic/claude-opus-5",         "openrouter", 1'000'000,
     "Claude Opus 5 — strongest Claude", false},
    {"anthropic/claude-sonnet-5",       "openrouter", 1'000'000,
     "Claude Sonnet 5 — via OpenRouter", true},
    {"anthropic/claude-opus-4.8",       "openrouter", 1'000'000, "", false},
    {"anthropic/claude-opus-4.7",       "openrouter", 1'000'000, "", false},
    {"anthropic/claude-sonnet-4.6",     "openrouter", 1'000'000, "", false},
    {"anthropic/claude-haiku-4.5",      "openrouter", 200'000, "", false},
    {"~anthropic/claude-opus-latest",   "openrouter", 1'000'000, "", false},
    {"~anthropic/claude-sonnet-latest", "openrouter", 1'000'000, "", false},
    {"~anthropic/claude-haiku-latest",  "openrouter", 200'000, "", false},
    // Short Anthropic aliases (rewritten to dotted OpenRouter slugs)
    {"claude-opus-4-7",                 "openrouter", 1'000'000, "", false},
    {"claude-sonnet-4-6",               "openrouter", 1'000'000, "", false},
    {"claude-haiku-4-5",                "openrouter", 200'000, "", false},
    // OpenAI (OpenRouter)
    {"openai/gpt-5.6-sol",              "openrouter", 1'000'000, "", false},
    {"openai/gpt-5.5",                  "openrouter", 1'000'000,
     "GPT-5.5 — via OpenRouter", true},
    {"openai/gpt-5.4",                  "openrouter", 1'000'000, "", false},
    {"openai/gpt-5.4-mini",             "openrouter", 1'000'000, "", false},
    {"~openai/gpt-latest",              "openrouter", 1'000'000,
     "OpenAI latest — via OpenRouter (recommended)", true},
    {"~openai/gpt-mini-latest",         "openrouter", 1'000'000, "", false},
    // Google Gemini (OpenRouter)
    {"google/gemini-3.1-pro-preview",   "openrouter", 1'000'000, "", false},
    {"google/gemini-3.6-flash",         "openrouter", 1'000'000,
     "Gemini 3.6 Flash — via OpenRouter", true},
    {"google/gemini-3.5-flash",         "openrouter", 1'000'000, "", false},
    {"google/gemini-3.1-flash-lite",    "openrouter", 1'000'000,
     "Gemini Flash Lite — via OpenRouter", true},
    // Other strong OpenRouter options used by starter agents
    {"x-ai/grok-4.5",                   "openrouter", 500'000, "", false},
    {"deepseek/deepseek-v4-pro",        "openrouter", 128'000, "", false},
};

} // namespace

const ModelCatalogEntry* model_catalog(std::size_t& count) {
    count = sizeof(kModels) / sizeof(kModels[0]);
    return kModels;
}

const ModelCatalogEntry* find_model_catalog_entry(std::string_view id) {
    if (id.empty()) return nullptr;
    for (const auto& m : kModels) {
        if (id == m.id) return &m;
    }
    return nullptr;
}

std::string format_context_window(int tokens) {
    if (tokens <= 0) return "unknown";
    if (tokens >= 1'000'000 && tokens % 1'000'000 == 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dM", tokens / 1'000'000);
        return buf;
    }
    if (tokens >= 1000 && tokens % 1000 == 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dk", tokens / 1000);
        return buf;
    }
    return std::to_string(tokens);
}

std::string format_model_catalog_list() {
    std::size_t n = 0;
    const auto* models = model_catalog(n);
    std::string out = "Model catalogue (" + std::to_string(n) + "):\n";
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = models[i];
        out += "  ";
        out += m.id;
        out += "  [";
        out += m.provider;
        out += "]  ctx=";
        out += format_context_window(m.context_window);
        if (m.blurb && m.blurb[0] != '\0') {
            out += "  — ";
            out += m.blurb;
        }
        out += '\n';
    }
    out += "Usage: /model <agent-id> <model-id>\n"
           "  e.g. /model research anthropic/claude-sonnet-5\n"
           "Custom OpenRouter / ollama/ ids are accepted; compaction uses\n"
           "a heuristic window (or char-budget fallback) when unknown.";
    return out;
}

} // namespace arbiter
