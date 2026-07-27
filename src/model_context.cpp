#include "model_context.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace arbiter {

namespace {

bool starts_with_ci(std::string_view hay, std::string_view needle) {
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(hay[i]))
            != std::tolower(static_cast<unsigned char>(needle[i])))
            return false;
    }
    return true;
}

} // namespace

int context_window_for_model(std::string_view model) {
    if (model.empty()) return 0;
    if (starts_with_ci(model, "ollama/") || starts_with_ci(model, "local/"))
        return 0;

    // Current OpenRouter Claude / GPT-5.x / Gemini 3.x windows are 1M-class;
    // keep a conservative 200k for older Claude short ids without a "5"/"4.6"+
    // marker so sidebar % does not under-report fill on large contexts.
    if (model.find("claude") != std::string_view::npos) {
        if (model.find("sonnet-5") != std::string_view::npos ||
            model.find("opus-5") != std::string_view::npos ||
            model.find("sonnet-4.6") != std::string_view::npos ||
            model.find("opus-4.") != std::string_view::npos ||
            model.find("claude-sonnet-latest") != std::string_view::npos ||
            model.find("claude-opus-latest") != std::string_view::npos)
            return 1'000'000;
        return 200'000;
    }

    if (model.find("gpt-5") != std::string_view::npos ||
        model.find("gpt-latest") != std::string_view::npos ||
        model.find("gpt-mini-latest") != std::string_view::npos)
        return 1'000'000;

    if (model.find("gemini-3") != std::string_view::npos ||
        model.find("gemini-2.5") != std::string_view::npos)
        return 1'000'000;

    if (model.find("grok-4") != std::string_view::npos)
        return 500'000;

    if (model.find("gpt-4o-mini") != std::string_view::npos)
        return 128'000;
    if (model.find("gpt-4o") != std::string_view::npos)
        return 128'000;
    if (model.find("gpt-4") != std::string_view::npos)
        return 128'000;
    if (model.find("gpt-3.5") != std::string_view::npos)
        return 16'385;

    return 128'000;
}

int context_pct_value(int prompt_tokens, std::string_view model) {
    if (prompt_tokens <= 0) return -1;
    const int window = context_window_for_model(model);
    if (window <= 0) return -1;
    return std::min(100, (prompt_tokens * 100) / window);
}

} // namespace arbiter
