#include "tui/sidebar_format.h"

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <regex>
#include <string>

namespace arbiter {

namespace {

struct ModelRates {
    double in_per_m;
    double out_per_m;
};

bool starts_with_ci(std::string_view hay, std::string_view needle) {
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(hay[i]))
            != std::tolower(static_cast<unsigned char>(needle[i])))
            return false;
    }
    return true;
}

ModelRates rates_for_model(std::string_view model) {
    if (model.empty()) return {3.0, 15.0};

    if (starts_with_ci(model, "ollama/") || starts_with_ci(model, "local/"))
        return {0.0, 0.0};

    if (model.find("haiku") != std::string_view::npos)
        return {0.8, 4.0};
    if (model.find("opus") != std::string_view::npos)
        return {15.0, 75.0};
    if (model.find("sonnet") != std::string_view::npos)
        return {3.0, 15.0};

    // GPT-family rough defaults (OpenRouter-ish).
    if (model.find("gpt-4o-mini") != std::string_view::npos)
        return {0.15, 0.6};
    if (model.find("gpt-4o") != std::string_view::npos)
        return {2.5, 10.0};
    if (model.find("gpt-") != std::string_view::npos)
        return {2.5, 10.0};

    return {3.0, 15.0};
}

} // namespace

std::string format_token_count(int n) {
    if (n < 0) n = 0;
    char buf[24];
    if (n >= 1'000'000) {
        std::snprintf(buf, sizeof(buf), "%.1fM",
                      static_cast<double>(n) / 1'000'000.0);
        return buf;
    }
    if (n >= 10'000) {
        std::snprintf(buf, sizeof(buf), "%.1fk",
                      static_cast<double>(n) / 1'000.0);
        return buf;
    }
    if (n >= 1'000) {
        std::snprintf(buf, sizeof(buf), "%.1fk",
                      static_cast<double>(n) / 1'000.0);
        return buf;
    }
    return std::to_string(n);
}

double estimate_cost_usd(std::string_view model, int in_tokens, int out_tokens) {
    if (in_tokens <= 0 && out_tokens <= 0) return 0.0;
    const ModelRates r = rates_for_model(model);
    return (static_cast<double>(in_tokens) / 1'000'000.0) * r.in_per_m
         + (static_cast<double>(out_tokens) / 1'000'000.0) * r.out_per_m;
}

std::string format_cost_usd(double usd) {
    if (usd <= 0.0) return "$0.00";
    char buf[32];
    if (usd < 0.01)
        std::snprintf(buf, sizeof(buf), "$%.4f", usd);
    else
        std::snprintf(buf, sizeof(buf), "$%.2f", usd);
    return buf;
}

std::string cost_basis_label(std::string_view primary_model, bool mixed_models) {
    if (mixed_models) return "mixed";
    if (primary_model.empty()) return "default";
    if (starts_with_ci(primary_model, "ollama/") ||
        starts_with_ci(primary_model, "local/"))
        return "local";
    return std::string(primary_model);
}

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

std::string format_context_pct(int prompt_tokens, std::string_view model) {
    const int pct = context_pct_value(prompt_tokens, model);
    if (pct < 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    return buf;
}

std::string format_sidebar_model(std::string_view model) {
    if (model.empty()) return {};
    // Drop the leading provider segment (`openrouter/…`, `ollama\…`, …).
    static const std::regex kAfterSep(R"(^[^/\\]+[/\\](.+)$)");
    std::cmatch match;
    if (std::regex_match(model.data(), model.data() + model.size(), match, kAfterSep)) {
        return match[1].str();
    }
    return std::string(model);
}

} // namespace arbiter
