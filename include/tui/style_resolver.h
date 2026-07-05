#pragma once

#include "styled_text.h"
#include "tui/tui_design.h"

#include <cstdint>

namespace arbiter {

struct ResolvedStyle {
    const TuiRgba* fg = nullptr;
    std::uint32_t  attrs = 0;
};

[[nodiscard]] ResolvedStyle resolve_style(StyleId id);

} // namespace arbiter
