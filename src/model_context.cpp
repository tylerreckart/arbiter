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

    if (model.find("opus") != std::string_view::npos ||
        model.find("sonnet") != std::string_view::npos ||
        model.find("haiku") != std::string_view::npos)
        return 200'000;

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
