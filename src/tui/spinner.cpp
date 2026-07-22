#include "tui/spinner.h"

#include <chrono>
#include <string>

namespace arbiter {

namespace {

constexpr const char* kBrailleFrames[] = {
    "\u2801", "\u2802", "\u2804", "\u2840", "\u2848", "\u2850",
    "\u2860", "\u28C0", "\u28C1", "\u28C2", "\u28C4", "\u28CC",
    "\u28D4", "\u28E4", "\u28E5", "\u28E6", "\u28EE", "\u28F6",
    "\u28F7", "\u28FF", "\u287F", "\u283F", "\u281F", "\u281F",
    "\u285B", "\u281B", "\u282B", "\u288B", "\u280B", "\u280D",
    "\u2809", "\u2809", "\u2811", "\u2821", "\u2881",
};
constexpr int kBrailleFrameCount =
    static_cast<int>(sizeof(kBrailleFrames) / sizeof(kBrailleFrames[0]));

// Ellipsis is U+2026 so the phrase reads as one glyph, not three dots.
constexpr const char* kWaitPhrases[] = {
    "Working\u2026",
    "Looking into that\u2026",
    "Thinking it through\u2026",
    "Digging in\u2026",
    "One moment\u2026",
    "On it\u2026",
    "Figuring this out\u2026",
    "Taking a look\u2026",
    "Hang tight\u2026",
    "Almost there\u2026",
};
constexpr int kWaitPhraseCount =
    static_cast<int>(sizeof(kWaitPhrases) / sizeof(kWaitPhrases[0]));

constexpr int kBraillePeriodMs = 80;
constexpr int kPhrasePeriodMs  = 2400;

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string_view spinner_frame() {
    const int i = static_cast<int>((steady_ms() / kBraillePeriodMs) % kBrailleFrameCount);
    return kBrailleFrames[i];
}

std::string_view wait_phrase() {
    const int i = static_cast<int>((steady_ms() / kPhrasePeriodMs) % kWaitPhraseCount);
    return kWaitPhrases[i];
}

std::string wait_status_label() {
    std::string out;
    out.reserve(48);
    out += spinner_frame();
    out += ' ';
    out += wait_phrase();
    return out;
}

std::string spinner_status_label(std::string_view label) {
    std::string out;
    out.reserve(label.size() + 8);
    out += spinner_frame();
    out += ' ';
    out.append(label.data(), label.size());
    return out;
}

} // namespace arbiter
