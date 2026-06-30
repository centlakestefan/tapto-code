// SPDX-License-Identifier: Apache-2.0
// Standalone test driver: renders a deliberately wide Markdown table through
// print_reply() so the clip / column-shrink code paths are exercised.

#include "tapto/ui.h"

int main() {
    const char* md = R"(
| # | Component | Status | Version | Last Updated | Maintainer | License | Notes |
|---|-----------|--------|---------|--------------|------------|---------|-------|
| 1 | **render_inline** | ✅ Passing | `v2.4.1` | 2026-01-15 | Alice Bergström | Apache-2.0 | Handles bold, italic, inline code |
| 2 | **clip_cell** | ⚠️ Review | `v2.4.1` | 2026-01-20 | Bob Nakamura | Apache-2.0 | Wide-char and ellipsis edge cases |
| 3 | display_width | ✅ Passing | `v2.3.9` | 2025-12-01 | Carol Lindqvist | Apache-2.0 | CJK, emoji, combining diacritics |
| 4 | strip_inline_markers | ✅ Passing | `v2.3.9` | 2025-11-30 | Alice Bergström | Apache-2.0 | Must not alter visible glyph count |
| 5 | render_table | 🔴 Failing | `v2.4.1` | 2026-01-21 | Dave Okonkwo | Apache-2.0 | Clip + ANSI re-render conflict |
| 6 | terminal_width | ✅ Passing | `v2.1.0` | 2025-06-10 | Eve Johansson | Apache-2.0 | Win32 + POSIX ioctl fallback to 80 |
)";

    tapto::ui::print_reply(md);
    return 0;
}
