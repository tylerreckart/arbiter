// index_ai/src/title_generator.cpp — see title_generator.h

#include "title_generator.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>

namespace index_ai {

void print_turn_rule(const std::string& label,
                     const std::string& color,
                     const std::string& right_label,
                     int cols) {
    const char* dim = "\033[38;5;238m";
    const char* rst = "\033[0m";

    int prefix = 4;  // "─── "
    int suffix = right_label.empty() ? 0 : (int)right_label.size() + 2;
    int label_w = (int)label.size() + 2;
    int fill = std::max(0, cols - prefix - label_w - suffix);

    std::string line;
    line += dim;
    line += "───";
    line += color;
    line += " ";
    line += label;
    line += " ";
    line += dim;
    for (int i = 0; i < fill; ++i) line += "─";
    if (!right_label.empty()) {
        line += "\033[38;5;241m ";
        line += right_label;
        line += dim;
        line += " ─";
    }
    line += rst;
    line += "\n";
    std::cout << line;
}

void generate_title_async(ApiClient& client,
                          const std::string& user_msg,
                          const std::string& assistant_snippet,
                          std::function<void(const std::string&)> on_title) {
    std::thread([&client, user_msg, assistant_snippet, on_title = std::move(on_title)]() {
        ApiRequest req;
        req.model       = "claude-haiku-4-5-20251001";
        req.max_tokens  = 12;
        req.temperature = 0.3;
        // No system_prompt — putting all instructions inline in the user turn
        // avoids the model echoing the system prompt as its response.

        std::string ctx = user_msg.substr(0, 200);
        if (!assistant_snippet.empty())
            ctx += "\n\n" + assistant_snippet.substr(0, 150);

        // "Title:" at the end cues completion behavior rather than a reply.
        req.messages = {{
            "user",
            "Conversation excerpt:\n" + ctx +
            "\n\nWrite a 5-7 word task title for this conversation. "
            "Reply with the title words only — no punctuation, no quotes.\n\nTitle:"
        }};

        auto resp = client.complete(req);
        if (!resp.ok || resp.content.empty()) return;

        std::string title = resp.content;
        while (!title.empty() && (title.back() == '\n' || title.back() == '\r' ||
                                   title.back() == ' '  || title.back() == '.'))
            title.pop_back();
        while (!title.empty() && (title.front() == '\n' || title.front() == ' '))
            title.erase(title.begin());
        if (title.size() >= 2 && title.front() == '"' && title.back() == '"')
            title = title.substr(1, title.size() - 2);

        if (!title.empty() && on_title)
            on_title(title);
    }).detach();
}

} // namespace index_ai
