// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/termui.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace tapto::ui {

// ---------------------------------------------------------------------------
// Markdown renderer
// ---------------------------------------------------------------------------
//
// A single-pass, line-oriented renderer that maps common Markdown constructs
// to ANSI escape sequences. It is intentionally simple: no block-level nesting
// (e.g. blockquote inside a list), no HTML passthrough. Those are rare in
// practice and would complicate the code significantly for little gain.
//
// Inline span pass handles: **bold**, *italic*, `code`.
// Block pass handles: headings, fenced code blocks, bullet/numbered lists,
// blockquotes, horizontal rules, tables.

namespace {

// --- ANSI helpers -----------------------------------------------------------

constexpr const char* kReset     = "\x1b[0m";
constexpr const char* kBold      = "\x1b[1m";
constexpr const char* kDim       = "\x1b[2m";
constexpr const char* kItalic    = "\x1b[3m";
constexpr const char* kUnderline = "\x1b[4m";
constexpr const char* kCyan      = "\x1b[36m"; // inline code
constexpr const char* kYellow    = "\x1b[33m"; // warnings
constexpr const char* kRed       = "\x1b[31m"; // errors

// --- Inline span rendering --------------------------------------------------

// Forward-declared because bold/italic recurse into it.
std::string render_inline(const std::string& line);

std::string render_inline(const std::string& line) {
    std::string out;
    out.reserve(line.size() + 64);
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        char c = line[i];

        // Inline code: `...`
        if (c == '`') {
            size_t end = line.find('`', i + 1);
            if (end != std::string::npos) {
                out += kCyan;
                out += line.substr(i + 1, end - i - 1);
                out += kReset;
                i = end + 1;
                continue;
            }
        }

        // Bold: **...**
        if (c == '*' && i + 1 < n && line[i + 1] == '*') {
            size_t end = line.find("**", i + 2);
            if (end != std::string::npos) {
                out += kBold;
                out += render_inline(line.substr(i + 2, end - i - 2));
                out += kReset;
                i = end + 2;
                continue;
            }
        }

        // Italic: *...* (single star, not **).
        if (c == '*' && (i + 1 >= n || line[i + 1] != '*')) {
            size_t end = i + 1;
            while (end < n) {
                if (line[end] == '*' && (end + 1 >= n || line[end + 1] != '*')) break;
                ++end;
            }
            if (end < n) {
                out += kItalic;
                out += render_inline(line.substr(i + 1, end - i - 1));
                out += kReset;
                i = end + 1;
                continue;
            }
        }

        out += c;
        ++i;
    }
    return out;
}

// --- Horizontal rule detection ----------------------------------------------

// Returns true for CommonMark thematic breaks: 3+ '-', '*', or '_' with
// optional spaces, and nothing else on the line.
bool is_hr(const std::string& line) {
    if (line.empty()) return false;
    char marker = '\0';
    int count = 0;
    for (char c : line) {
        if (c == ' ') continue;
        if (marker == '\0') {
            if (c != '-' && c != '*' && c != '_') return false;
            marker = c;
        }
        if (c != marker) return false;
        ++count;
    }
    return count >= 3;
}

// --- Table helpers ----------------------------------------------------------

// True if the codepoint renders two terminal columns (CJK, fullwidth, most
// emoji). A best-effort approximation without a full Unicode database.
bool is_wide(uint32_t cp) {
    return (cp >= 0x1100  && cp <= 0x115F)  || (cp >= 0x2E80  && cp <= 0x303E)  ||
           (cp >= 0x3041  && cp <= 0x33BF)  || (cp >= 0x33FF  && cp <= 0xA4CF)  ||
           (cp >= 0xA960  && cp <= 0xA97F)  || (cp >= 0xAC00  && cp <= 0xD7FF)  ||
           (cp >= 0xF900  && cp <= 0xFAFF)  || (cp >= 0xFE10  && cp <= 0xFE1F)  ||
           (cp >= 0xFE30  && cp <= 0xFE6F)  || (cp >= 0xFF00  && cp <= 0xFF60)  ||
           (cp >= 0xFFE0  && cp <= 0xFFE6)  || (cp >= 0x1B000 && cp <= 0x1B0FF) ||
           (cp >= 0x1F004 && cp <= 0x1F0CF) || (cp >= 0x1F300 && cp <= 0x1F9FF) ||
           (cp >= 0x20000 && cp <= 0x2FFFD) || (cp >= 0x30000 && cp <= 0x3FFFD);
}

// True if the codepoint is zero-width: a combining mark, ZWJ or variation
// selector that composes onto the preceding glyph and adds no column.
bool is_combining(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F)  || (cp >= 0x1DC0 && cp <= 0x1DFF)  ||
           (cp >= 0x20D0 && cp <= 0x20FF)  || (cp >= 0xFE20 && cp <= 0xFE2F) ||
           cp == 0xFE0F || cp == 0x200D;
}

// Returns the terminal display width of a UTF-8 string.
// ASCII: 1 column. Wide (CJK, emoji, etc.): 2 columns. Continuation bytes and
// combining characters: 0 columns added (they compose onto the base glyph).
// This is a best-effort approximation without pulling in a full Unicode database.
size_t display_width(const std::string& s) {
    size_t width = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* end = p + s.size();
    while (p < end) {
        unsigned char b = *p;
        uint32_t cp = 0;
        int bytes = 1;
        if (b < 0x80) {
            cp = b;
            bytes = 1;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F; bytes = 2;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F; bytes = 3;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07; bytes = 4;
        } else {
            // Continuation byte or invalid — skip.
            ++p; continue;
        }
        for (int i = 1; i < bytes && p + i < end; ++i)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += bytes;

        if (is_combining(cp)) continue;        // zero-width: no column added
        width += is_wide(cp) ? 2 : 1;
    }
    return width;
}

// Split a Markdown table row on '|', trimming whitespace from each cell.
// Leading/trailing empty cells from the outer pipes are dropped.
std::vector<std::string> split_table_row(const std::string& line) {
    std::vector<std::string> cells;
    size_t i = 0;
    const size_t n = line.size();
    // Skip leading pipe if present.
    if (i < n && line[i] == '|') ++i;
    std::string cell;
    for (; i < n; ++i) {
        if (line[i] == '|') {
            // Trim whitespace.
            size_t s = cell.find_first_not_of(' ');
            size_t e = cell.find_last_not_of(' ');
            cells.push_back(s == std::string::npos ? "" : cell.substr(s, e - s + 1));
            cell.clear();
        } else {
            cell += line[i];
        }
    }
    // Last cell (trailing pipe already consumed above or absent).
    {
        size_t s = cell.find_first_not_of(' ');
        size_t e = cell.find_last_not_of(' ');
        std::string last = s == std::string::npos ? "" : cell.substr(s, e - s + 1);
        if (!last.empty()) cells.push_back(last);
    }
    return cells;
}

// Returns true if every non-empty, non-space character in the line is one of
// '|', '-', ':', making it a GFM separator row (e.g. |---|:---:|---| ).
bool is_table_separator(const std::string& line) {
    bool has_dash = false;
    for (char c : line) {
        if (c == ' ' || c == '|' || c == ':') continue;
        if (c == '-') { has_dash = true; continue; }
        return false;
    }
    return has_dash;
}

// Returns true if the line looks like a table row (contains at least one '|').
bool is_table_row(const std::string& line) {
    return line.find('|') != std::string::npos;
}

// Strip inline Markdown markers (* ` _) so we can measure the visible glyph
// width of a cell without the syntax characters inflating the count.
// This does NOT produce rendered output — it only removes the markers.
std::string strip_inline_markers(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        // Inline code: `...` — drop the backticks, keep the content.
        if (c == '`') {
            size_t end = s.find('`', i + 1);
            if (end != std::string::npos) {
                out += s.substr(i + 1, end - i - 1);
                i = end + 1;
                continue;
            }
        }
        // Bold: **...**
        if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            size_t end = s.find("**", i + 2);
            if (end != std::string::npos) {
                out += strip_inline_markers(s.substr(i + 2, end - i - 2));
                i = end + 2;
                continue;
            }
        }
        // Italic: *...*
        if (c == '*' && (i + 1 >= n || s[i + 1] != '*')) {
            size_t end = i + 1;
            while (end < n) {
                if (s[end] == '*' && (end + 1 >= n || s[end + 1] != '*')) break;
                ++end;
            }
            if (end < n) {
                out += strip_inline_markers(s.substr(i + 1, end - i - 1));
                i = end + 1;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// Returns the current terminal column width, or 80 as a safe fallback when
// stdout is redirected or the query fails.
size_t terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return static_cast<size_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return static_cast<size_t>(ws.ws_col);
#endif
    return 80;
}

// The terminal's height in rows, or 24 when stdout is not a terminal.
size_t terminal_height() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return static_cast<size_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return static_cast<size_t>(ws.ws_row);
#endif
    return 24;
}

// The columns a rendered line takes: display_width without the ANSI escape
// sequences (colors, styles), which take none.
size_t visible_width(const std::string& s) {
    std::string plain;
    plain.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            continue;
        }
        plain += s[i];
    }
    return display_width(plain);
}

// Clip a raw (pre-render) cell string to at most `max_cols` visible columns,
// appending '…' (1 column) if truncation was needed.
// Returns the clipped raw string and sets `out_width` to its display width.
std::string clip_cell(const std::string& raw, size_t max_cols, size_t& out_width) {
    if (max_cols == 0) { out_width = 0; return ""; }
    // Walk the string codepoint-by-codepoint until we would exceed max_cols.
    // Reserve one column for the ellipsis so the final cell is still ≤ max_cols.
    const unsigned char* p   = reinterpret_cast<const unsigned char*>(raw.data());
    const unsigned char* end = p + raw.size();
    size_t width = 0;
    const unsigned char* clip_at = p; // byte position where we must stop
    while (p < end) {
        unsigned char b = *p;
        // Pass ANSI escape sequences through without counting visible width, and
        // never clip in the middle of one (would leave a broken control code).
        if (b == 0x1B) {
            const unsigned char* q = p + 1;
            if (q < end && *q == '[') {          // CSI: ESC [ params... final
                ++q;
                while (q < end && !(*q >= 0x40 && *q <= 0x7E)) ++q;
                if (q < end) ++q;                // include the final byte
            } else if (q < end) {
                ++q;                             // lone ESC + one byte (rare)
            }
            clip_at = q;
            p = q;
            continue;
        }
        int bytes = 1;
        uint32_t cp = 0;
        if      (b < 0x80)            { cp = b;        bytes = 1; }
        else if ((b & 0xE0) == 0xC0)  { cp = b & 0x1F; bytes = 2; }
        else if ((b & 0xF0) == 0xE0)  { cp = b & 0x0F; bytes = 3; }
        else if ((b & 0xF8) == 0xF0)  { cp = b & 0x07; bytes = 4; }
        else                          { ++p; continue; }
        for (int i = 1; i < bytes && p + i < end; ++i)
            cp = (cp << 6) | (p[i] & 0x3F);

        size_t cw = 1;
        if ((cp >= 0x1100  && cp <= 0x115F)  || (cp >= 0x2E80  && cp <= 0x303E)  ||
            (cp >= 0x3041  && cp <= 0x33BF)  || (cp >= 0x33FF  && cp <= 0xA4CF)  ||
            (cp >= 0xA960  && cp <= 0xA97F)  || (cp >= 0xAC00  && cp <= 0xD7FF)  ||
            (cp >= 0xF900  && cp <= 0xFAFF)  || (cp >= 0xFE10  && cp <= 0xFE1F)  ||
            (cp >= 0xFE30  && cp <= 0xFE6F)  || (cp >= 0xFF00  && cp <= 0xFF60)  ||
            (cp >= 0xFFE0  && cp <= 0xFFE6)  || (cp >= 0x1B000 && cp <= 0x1B0FF) ||
            (cp >= 0x1F004 && cp <= 0x1F0CF) || (cp >= 0x1F300 && cp <= 0x1F9FF) ||
            (cp >= 0x20000 && cp <= 0x2FFFD) || (cp >= 0x30000 && cp <= 0x3FFFD))
            cw = 2;
        // Zero-width / combining: cw stays 0 effectively — treat as 0.
        if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
            (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
            cp == 0xFE0F || cp == 0x200D)
            cw = 0;

        if (width + cw > max_cols) {
            // Would overflow — stop here and append ellipsis.
            std::string clipped(reinterpret_cast<const char*>(
                reinterpret_cast<const unsigned char*>(raw.data())),
                static_cast<size_t>(clip_at -
                    reinterpret_cast<const unsigned char*>(raw.data())));
            clipped += "\xe2\x80\xa6"; // UTF-8 for U+2026 HORIZONTAL ELLIPSIS
            out_width = width + 1;     // ellipsis occupies 1 column
            return clipped;
        }
        width += cw;
        clip_at = p + bytes;
        p += bytes;
    }
    out_width = width;
    return raw; // no clipping needed
}

// One space-separated word in a Markdown cell: its visible text (markers
// removed), its display width in terminal columns, and the ANSI style it was
// rendered in ("" when plain).
struct StyledWord {
    std::string text;
    size_t      width;
    const char* style;
};

// Split a raw cell (with inline Markdown: **bold**, *italic*, `code`) into
// space-separated styled words, mirroring render_inline()'s marker handling.
// The word's style is whichever style was active at its start.
std::vector<StyledWord> tokenize_styled(const std::string& cell) {
    std::vector<StyledWord> words;
    std::string cur;
    const char* cur_style = "";
    const size_t n = cell.size();
    size_t i = 0;

    auto flush = [&]() {
        if (!cur.empty()) {
            StyledWord w;
            w.text  = cur;
            w.width = display_width(cur);
            w.style = cur_style;
            words.push_back(std::move(w));
            cur.clear();
        }
    };

    while (i < n) {
        char c = cell[i];

        // Inline code: `...`
        if (c == '`') {
            size_t end = cell.find('`', i + 1);
            if (end != std::string::npos) {
                const char* save = cur_style;
                cur_style = kCyan;
                for (size_t j = i + 1; j < end; ++j) {
                    if (cell[j] == ' ') flush();
                    else cur += cell[j];
                }
                flush();
                cur_style = save;
                i = end + 1;
                continue;
            }
        }

        // Bold: **...**
        if (c == '*' && i + 1 < n && cell[i + 1] == '*') {
            size_t end = cell.find("**", i + 2);
            if (end != std::string::npos) {
                const char* save = cur_style;
                cur_style = kBold;
                for (size_t j = i + 2; j < end; ++j) {
                    if (cell[j] == ' ') flush();
                    else cur += cell[j];
                }
                flush();
                cur_style = save;
                i = end + 2;
                continue;
            }
        }

        // Italic: *...* (single star, not **)
        if (c == '*' && (i + 1 >= n || cell[i + 1] != '*')) {
            size_t end = i + 1;
            while (end < n) {
                if (cell[end] == '*' && (end + 1 >= n || cell[end + 1] != '*')) break;
                ++end;
            }
            if (end < n) {
                const char* save = cur_style;
                cur_style = kItalic;
                for (size_t j = i + 1; j < end; ++j) {
                    if (cell[j] == ' ') flush();
                    else cur += cell[j];
                }
                flush();
                cur_style = save;
                i = end + 1;
                continue;
            }
        }

        if (c == ' ') flush();
        else cur += c;
        ++i;
    }
    flush();
    return words;
}

// Wrap a cell (with inline Markdown) into lines each at most `max_cols`
// visible columns. Styled words survive the wrap: each emitted line carries
// the ANSI codes for the words it contains, so a **bold** word that is
// hard-broken is bold on both parts. Breaks at spaces; a single word wider
// than the width is hard-broken (continuation flush-left).
std::vector<std::string> wrap_inline(const std::string& cell, size_t max_cols) {
    std::vector<std::string> lines;
    if (max_cols == 0) return lines;

    // Break a single word wider than max_cols into chunks that each fit.
    // If the word is styled, each chunk is wrapped in its style.
    auto hard_break = [&](const StyledWord& w) {
        const unsigned char* p   = reinterpret_cast<const unsigned char*>(w.text.data());
        const unsigned char* end = p + w.text.size();
        std::string chunk;
        size_t width = 0;
        while (p < end) {
            unsigned char b = *p;
            int bytes = 1;
            if      ((b & 0xE0) == 0xC0) bytes = 2;
            else if ((b & 0xF0) == 0xE0) bytes = 3;
            else if ((b & 0xF8) == 0xF0) bytes = 4;
            uint32_t cp = b;
            for (int j = 1; j < bytes && p + j < end; ++j)
                cp = (cp << 6) | (p[j] & 0x3F);
            size_t cw = is_wide(cp) ? 2 : 1;
            if (width + cw > max_cols && !chunk.empty()) {
                if (*w.style) lines.push_back(std::string(w.style) + chunk + kReset);
                else          lines.push_back(chunk);
                chunk.clear();
                width = 0;
            }
            chunk.append(reinterpret_cast<const char*>(p), bytes);
            width += cw;
            p += bytes;
        }
        if (!chunk.empty()) {
            if (*w.style) lines.push_back(std::string(w.style) + chunk + kReset);
            else          lines.push_back(chunk);
        }
    };

    const auto words = tokenize_styled(cell);
    std::string line;
    size_t line_w = 0;

    auto flush_line = [&]() {
        if (!line.empty()) { lines.push_back(line); line.clear(); line_w = 0; }
    };

    for (const auto& w : words) {
        if (w.width <= max_cols) {
            const std::string rendered =
                *w.style ? std::string(w.style) + w.text + kReset : w.text;
            if (line.empty()) {
                line = rendered;
                line_w = w.width;
            } else if (line_w + 1 + w.width <= max_cols) {
                line += " " + rendered;
                line_w += 1 + w.width;
            } else {
                flush_line();
                line   = rendered;
                line_w = w.width;
            }
        } else {
            flush_line();
            hard_break(w);
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Render a collected table (header row + data rows) into out. Columns are
// padded to uniform width per column; cells wider than their column wrap onto
// continuation lines (keeping inline Markdown styling) rather than being
// clipped. `max_width` is the maximum total table width in visible columns
// (the terminal width, or a smaller width in tests).
void render_table(std::string& out,
                  const std::vector<std::string>& header,
                  const std::vector<std::vector<std::string>>& rows,
                  size_t max_width) {
    if (header.empty()) return;
    const size_t ncols = header.size();

    // Column width is the widest cell in that column. We strip the Markdown
    // markers first (** * ` etc.) because those characters are consumed by
    // the inline pass and never reach the terminal, so counting them would
    // over-estimate the width and break alignment. display_width() handles
    // multi-byte / wide (CJK, emoji) glyphs.
    std::vector<size_t> widths(ncols, 0);
    auto col_w = [&](const std::vector<std::string>& cells, size_t c) {
        return c < cells.size() ? display_width(strip_inline_markers(cells[c])) : size_t{0};
    };
    for (size_t c = 0; c < ncols; ++c) widths[c] = col_w(header, c);
    for (const auto& row : rows)
        for (size_t c = 0; c < ncols; ++c) widths[c] = std::max(widths[c], col_w(row, c));

    // The table renders as: 1 (left border) + ncols * (column + 3), where the
    // +3 is "space + right padding column" -- a left/right space and a
    // separator border on each side of the cell.
    auto table_w = [&]() -> size_t {
        size_t w = 1;
        for (size_t c = 0; c < ncols; ++c) w += widths[c] + 3;
        return w;
    };

    // When the natural column widths overflow `max_width`, redistribute the
    // available space equally across columns (floor 1 each) so the table
    // always fits. Cells are then wrapped into the (narrower) column width.
    if (table_w() > max_width) {
        const size_t total_avail =
            max_width > ncols * 4 + 1 ? max_width - 1 - ncols * 3 : ncols;
        const size_t per_col = total_avail / ncols;
        for (size_t c = 0; c < ncols; ++c) widths[c] = std::max(per_col, size_t{1});
    }

    // A wrapped cell: one emitted line per visual row, each carrying its own
    // ANSI styling (see wrap_inline).
    struct Cell { std::vector<std::string> lines; };

    auto wrap_cells = [&](const std::vector<std::string>& cells) {
        std::vector<Cell> wrapped;
        for (size_t c = 0; c < ncols; ++c) {
            // A row shorter than the header leaves a missing cell, rendered empty.
            const std::string raw = (c < cells.size()) ? cells[c] : std::string{};
            Cell cl;
            cl.lines = wrap_inline(raw, widths[c]);
            if (cl.lines.empty()) cl.lines.push_back("");
            wrapped.push_back(std::move(cl));
        }
        return wrapped;
    };

    // Emit the wrapped rows of one table row (header or data) aligned in a
    // grid: the row occupies as many lines as its tallest cell.
    auto emit_grid = [&](const std::vector<Cell>& wrapped, bool is_header) {
        size_t nlines = 1;
        for (const auto& cl : wrapped) nlines = std::max(nlines, cl.lines.size());
        for (size_t ln = 0; ln < nlines; ++ln) {
            out += kDim;
            out += "│";
            out += kReset;
            for (size_t c = 0; c < ncols; ++c) {
                const std::string& text =
                    (ln < wrapped[c].lines.size()) ? wrapped[c].lines[ln] : "";
                if (is_header && ln == 0)
                    out += std::string(" ") + kBold + text + kReset;
                else
                    out += " " + text;
                const size_t tw = visible_width(text);
                if (tw < widths[c]) out.append(widths[c] - tw, ' ');
                out += std::string(" ") + kDim + "│" + kReset;
            }
            out += "\n";
        }
    };

    // A horizontal divider. `heavy` picks the left corner: "├" under the
    // header, "┼" between data rows (matching the vertical grid lines).
    auto emit_sep = [&](bool heavy) {
        out += kDim;
        out += heavy ? "├" : "┼";
        for (size_t c = 0; c < ncols; ++c) {
            for (size_t k = 0; k < widths[c] + 2; ++k) out += "─";
            out += (c + 1 < ncols) ? "┼" : "┤";
        }
        out += kReset;
        out += "\n";
    };

    // Top border.
    out += kDim;
    out += "┌";
    for (size_t c = 0; c < ncols; ++c) {
        for (size_t k = 0; k < widths[c] + 2; ++k) out += "─";
        out += (c + 1 < ncols) ? "┬" : "┐";
    }
    out += kReset;
    out += "\n";

    emit_grid(wrap_cells(header), true);
    emit_sep(/*heavy=*/true);
    for (size_t r = 0; r < rows.size(); ++r) {
        emit_grid(wrap_cells(rows[r]), false);
        if (r + 1 < rows.size()) emit_sep(/*heavy=*/false);
    }

    // Bottom border.
    out += kDim;
    out += "└";
    for (size_t c = 0; c < ncols; ++c) {
        for (size_t k = 0; k < widths[c] + 2; ++k) out += "─";
        out += (c + 1 < ncols) ? "┴" : "┘";
    }
    out += kReset;
    out += "\n";
}



// --- Block-level rendering --------------------------------------------------

// Internal implementation: renders Markdown text to ANSI-styled output,
// wrapping tables so the rendered width never exceeds `max_width` visible
// columns. The public wrapper tapto::ui::render_markdown (see termui.h)
// supplies the terminal width and calls this.
std::string render_markdown_impl(const std::string& text, size_t max_width) {
    std::string out;
    out.reserve(text.size() * 2);

    // Split into lines, preserving blank lines as paragraph separators.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else            { cur += c; }
        }
        lines.push_back(cur);
    }

    bool in_fence = false;
    std::string fence_lang;

    // Table accumulation state.
    bool in_table = false;
    std::vector<std::string>              table_header;
    std::vector<std::vector<std::string>> table_rows;

    // Flush a completed table and reset state.
    auto flush_table = [&]() {
        if (!in_table) return;
        if (!table_header.empty())
            render_table(out, table_header, table_rows, max_width);
        in_table = false;
        table_header.clear();
        table_rows.clear();
    };

    for (const std::string& raw : lines) {

        // ---- Fenced code block toggle --------------------------------------
        if (raw.size() >= 3 && raw.substr(0, 3) == "```") {
            flush_table();
            if (!in_fence) {
                in_fence   = true;
                fence_lang = raw.size() > 3 ? raw.substr(3) : "";
                out += kDim;
                if (!fence_lang.empty()) out += fence_lang + "\n";
            } else {
                in_fence = false;
                out += kReset;
                out += "\n";
            }
            continue;
        }

        // ---- Inside a fenced code block ------------------------------------
        if (in_fence) {
            out += raw + "\n";
            continue;
        }

        // ---- Table rows ----------------------------------------------------
        if (is_table_row(raw)) {
            if (is_table_separator(raw)) {
                // Separator row: marks end of header, start of body — skip.
                continue;
            }
            auto cells = split_table_row(raw);
            if (!in_table) {
                // First pipe-row is the header.
                in_table     = true;
                table_header = std::move(cells);
            } else {
                table_rows.push_back(std::move(cells));
            }
            continue;
        }

        // Any non-table line ends an active table.
        flush_table();

        // ---- Horizontal rule -----------------------------------------------
        if (is_hr(raw)) {
            out += kDim;
            out.append(60, '-');
            out += kReset;
            out += "\n";
            continue;
        }

        // ---- Headings: # / ## / ### ----------------------------------------
        if (!raw.empty() && raw[0] == '#') {
            size_t level = 0;
            while (level < raw.size() && raw[level] == '#') ++level;
            std::string content = level < raw.size() ? raw.substr(level) : "";
            if (!content.empty() && content[0] == ' ') content = content.substr(1);

            if (level == 1)
                out += std::string(kBold) + kUnderline + content + kReset + "\n";
            else if (level == 2)
                out += std::string(kBold) + content + kReset + "\n";
            else
                out += std::string(kDim) + content + kReset + "\n";
            continue;
        }

        // ---- Blockquote: > -------------------------------------------------
        if (!raw.empty() && raw[0] == '>') {
            std::string content = raw.size() > 1 ? raw.substr(1) : "";
            if (!content.empty() && content[0] == ' ') content = content.substr(1);
            out += std::string(kDim) + "│ " + kReset;
            out += kDim + render_inline(content) + kReset + "\n";
            continue;
        }

        // ---- Bullet list: - / * / + ----------------------------------------
        if (raw.size() >= 2 &&
            (raw[0] == '-' || raw[0] == '*' || raw[0] == '+') &&
            raw[1] == ' ') {
            out += "  • " + render_inline(raw.substr(2)) + "\n";
            continue;
        }

        // Indented bullet (2 spaces + marker) — one level of nesting.
        if (raw.size() >= 4 && raw[0] == ' ' && raw[1] == ' ' &&
            (raw[2] == '-' || raw[2] == '*' || raw[2] == '+') &&
            raw[3] == ' ') {
            out += "    ◦ " + render_inline(raw.substr(4)) + "\n";
            continue;
        }

        // ---- Numbered list: 1. ---------------------------------------------
        {
            size_t j = 0;
            while (j < raw.size() && raw[j] >= '0' && raw[j] <= '9') ++j;
            if (j > 0 && j + 1 < raw.size() && raw[j] == '.' && raw[j + 1] == ' ') {
                out += "  " + raw.substr(0, j + 1) + " " +
                       render_inline(raw.substr(j + 2)) + "\n";
                continue;
            }
        }

        // ---- Plain paragraph line ------------------------------------------
        out += render_inline(raw) + "\n";
    }

    flush_table();
    if (in_fence) out += kReset; // safety reset if block was never closed

    return out;
}

// --- Status line state ------------------------------------------------------

bool        g_status_visible = false;
std::string g_status_text;

// --- Streamed reply state ---------------------------------------------------
//
// A streamed reply is written as it arrives: the chain of thought dimmed, the
// text plain. What is open on the current line, and the text block last
// written, are kept so the final reply can be redrawn as Markdown in place of
// its plain text instead of being printed a second time.

enum class StreamOpen { None, Reasoning, Text };
StreamOpen  g_stream_open = StreamOpen::None;
bool        g_stream_at_col0 = true;
std::string g_stream_text;       // the text block being written, as received
std::string g_stream_text_shown; // the same as written (tabs expanded)
std::string g_stream_reasoning;  // the reasoning block being written

// The last finished blocks of this turn, and whether the text block is still
// what the screen ends with (nothing written since), so it can be redrawn.
std::string g_last_text;
std::string g_last_text_shown;
std::string g_last_reasoning;
unsigned    g_output_seq = 0;      // bumped by every other output during a turn
unsigned    g_last_text_seq = ~0u; // g_output_seq when the last text block ended

void note_output() { ++g_output_seq; }

void erase_status_line() {
    std::cout << "\r\x1b[K";
}

std::string iteration_prefix(int iteration, int max_iterations) {
    if (iteration <= 0) return "";

    // Right-align the counter in a 3-char field: "[  1] ", "[ 42] ", "[100] ".
    std::string num = std::to_string(iteration);
    if (num.size() < 3) num = std::string(3 - num.size(), ' ') + num;

    // Warn as the counter nears the cap: yellow at 80%, red at 95% (red wins).
    const char* color = "";
    if (max_iterations > 0) {
        if (iteration >= max_iterations * 95 / 100)      color = kRed;
        else if (iteration >= max_iterations * 80 / 100) color = kYellow;
    }

    if (*color)
        return "[" + std::string(color) + num + kReset + "] ";
    return "[" + num + "] ";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Markdown (public)
// ---------------------------------------------------------------------------

// Render Markdown to ANSI-styled output, wrapping tables so they fit the
// terminal's current width. Pure function (no side effects): returns the
// rendered string for the caller to emit. Tests call the same code path
// through this entry point.
std::string render_markdown(const std::string& text, size_t max_width) {
    return render_markdown_impl(text, max_width);
}

// ---------------------------------------------------------------------------
// Progress / status line
// ---------------------------------------------------------------------------

void set_status(const std::string& text, int iteration, int max_iterations) {
    constexpr size_t kMaxWidth = 78;

    // clip_cell measures visible columns (UTF-8/wide-aware) and skips ANSI
    // escapes, so the counter's color codes don't count toward the width.
    size_t visible = 0;
    g_status_text = clip_cell(iteration_prefix(iteration, max_iterations) + text,
                              kMaxWidth, visible);

    note_output();
    if (!g_status_visible)
        std::cout << "\x1b[?25l"; // hide cursor while the status line is live

    std::cout << "\r" << g_status_text << "\x1b[K" << std::flush;
    g_status_visible = true;
}

void commit_status() {
    if (!g_status_visible) return;
    note_output();
    std::cout << "\r" << g_status_text << "\x1b[K\n" << std::flush;
    g_status_visible = false;
    g_status_text.clear();
}

void end_status() {
    if (!g_status_visible) return;
    std::cout << "\r\x1b[K\x1b[?25h" << std::flush; // erase line + restore cursor
    g_status_visible = false;
    g_status_text.clear();
}

// ---------------------------------------------------------------------------
// Permanent output
// ---------------------------------------------------------------------------

void emit_intermediate(const std::string& text, bool is_reasoning, bool print_cot) {
    if (text.empty() || !print_cot) return;
    note_output();
    if (g_status_visible) erase_status_line();
    if (is_reasoning)
        std::cout << kDim << text << kReset << "\n" << std::flush;
    else
        std::cout << text << "\n" << std::flush;
}

namespace {

std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// The rows `text` took when written from column 0 of an empty line, wrapped
// at `cols`: each of its lines at least one, a long one as many as it wraps to.
size_t rows_taken(const std::string& text, size_t cols) {
    cols = std::max<size_t>(cols, 1);
    size_t rows = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end == text.size() && start == text.size()) break; // nothing after the last newline
        const size_t w = visible_width(text.substr(start, end - start));
        rows += w == 0 ? 1 : (w + cols - 1) / cols;
        start = end + 1;
    }
    return rows;
}

// Close whatever block is open on the current line, ending the line.
void close_stream_block() {
    if (!g_stream_at_col0) std::cout << "\n";
    g_stream_at_col0 = true;
    if (g_stream_open == StreamOpen::Text) {
        g_last_text = g_stream_text;
        g_last_text_shown = g_stream_text_shown;
        g_last_text_seq = g_output_seq;
    } else if (g_stream_open == StreamOpen::Reasoning) {
        g_last_reasoning = g_stream_reasoning;
    }
    g_stream_text.clear();
    g_stream_text_shown.clear();
    g_stream_reasoning.clear();
    g_stream_open = StreamOpen::None;
}

} // namespace

void begin_turn_output() {
    g_stream_open = StreamOpen::None;
    g_stream_at_col0 = true;
    g_stream_text.clear();
    g_stream_text_shown.clear();
    g_stream_reasoning.clear();
    g_last_text.clear();
    g_last_text_shown.clear();
    g_last_reasoning.clear();
    g_last_text_seq = ~0u;
}

void print_stream(const tapto::StreamEvent& ev) {
    using Kind = tapto::StreamEvent::Kind;
    if (ev.kind == Kind::End) {
        close_stream_block();
        std::cout << std::flush;
        return;
    }
    if (ev.text.empty()) return;

    // The first piece takes the status line's place; the status comes back
    // with the next set_status.
    if (g_status_visible) {
        erase_status_line();
        std::cout << "\x1b[?25h";
        g_status_visible = false;
        g_status_text.clear();
    }

    const StreamOpen kind = ev.kind == Kind::Reasoning ? StreamOpen::Reasoning : StreamOpen::Text;
    if (g_stream_open != kind) {
        close_stream_block();
        g_stream_open = kind;
    }

    // Tabs are written as spaces, so the rows the block takes can be counted.
    // Line ends a block starts with are not written: models open their
    // answer with a blank line or two after the chain of thought.
    const bool block_empty = kind == StreamOpen::Reasoning ? g_stream_reasoning.empty()
                                                          : g_stream_text_shown.empty();
    std::string shown;
    shown.reserve(ev.text.size());
    for (char c : ev.text) {
        if (c == '\r') continue;
        if (c == '\n' && block_empty && shown.empty()) continue;
        if (c == '\t') shown += "    ";
        else shown += c;
    }
    if (shown.empty()) return;

    if (kind == StreamOpen::Reasoning) {
        g_stream_reasoning += ev.text;
        std::cout << kDim << shown << kReset;
    } else {
        g_stream_text += ev.text;
        g_stream_text_shown += shown;
        std::cout << shown;
    }
    g_stream_at_col0 = shown.back() == '\n';
    std::cout << std::flush;
}

void print_reply(const std::string& text) {
    const size_t cols = terminal_width();
    const std::string reply = trimmed(text);
    const std::string streamed = trimmed(g_last_text);

    // Streamed already. When the plain text is still the last thing on the
    // screen and all of it is in view, it is redrawn as Markdown in its place;
    // otherwise it stays as it is. What the reply adds after it (a note that
    // it was cut short) is printed below.
    if (!streamed.empty() && reply.compare(0, streamed.size(), streamed) == 0) {
        const size_t rows = rows_taken(g_last_text_shown, cols);
        const bool in_view = g_last_text_seq == g_output_seq && rows + 1 < terminal_height();
        if (in_view) {
            std::cout << "\x1b[" << rows << "F\x1b[J"
                      << render_markdown_impl(text, cols) << "\n";
        } else {
            const std::string rest = trimmed(reply.substr(streamed.size()));
            if (!rest.empty()) std::cout << render_markdown_impl(rest, cols);
            std::cout << "\n";
        }
        begin_turn_output();
        return;
    }
    // A reply that is only chain of thought was shown as it streamed, dimmed.
    if (!reply.empty() && reply == trimmed(g_last_reasoning)) {
        std::cout << "\n";
        begin_turn_output();
        return;
    }
    std::cout << render_markdown_impl(text, cols) << "\n";
    begin_turn_output();
}

void print_transcript(const std::vector<TranscriptEntry>& entries, const std::string& after) {
    if (entries.empty()) return;
    const size_t cols = std::max<size_t>(terminal_width(), 1);
    // The rows a line takes, wrapped at the terminal's width.
    auto rows_of = [&](const std::string& line) {
        const size_t w = visible_width(line);
        return w == 0 ? size_t{1} : (w + cols - 1) / cols;
    };

    // Every entry rendered as it was on the screen, one string per line: the
    // prompt in the prompt color behind its marker, the model's text as
    // Markdown, a blank line between the two.
    std::vector<std::string> lines;
    for (const auto& e : entries) {
        if (!lines.empty()) lines.emplace_back();
        std::vector<std::string> block;
        switch (e.who) {
        case TranscriptEntry::Who::User: {
            std::istringstream in(e.text);
            std::string l;
            bool first = true;
            while (std::getline(in, l)) {
                block.push_back(std::string(kPromptColor) + (first ? "> " : "") + l + kPromptReset);
                first = false;
            }
            break;
        }
        case TranscriptEntry::Who::Model: {
            std::istringstream in(render_markdown_impl(e.text, cols));
            std::string l;
            while (std::getline(in, l)) block.push_back(l);
            while (!block.empty() && visible_width(block.back()) == 0) block.pop_back();
            break;
        }
        case TranscriptEntry::Who::Summary:
            block.push_back(std::string(kDim) +
                            "(the conversation before this was summarized with /compact)" + kReset);
            break;
        }
        lines.insert(lines.end(), block.begin(), block.end());
    }

    // About one screenful: the lines from the end that fit in the terminal's
    // height, less what is printed after them (`after`, the blank line that
    // ends this, the prompt) and the line saying that more came before. Rows
    // are counted as long lines wrap. What does not fit is said to be there,
    // not shown.
    size_t reserve = 3;
    {
        std::istringstream in(after);
        std::string l;
        while (std::getline(in, l)) reserve += rows_of(l);
    }
    const size_t height = terminal_height();
    const size_t budget = height > reserve + 3 ? height - reserve : 3;
    size_t first = lines.size();
    size_t rows = 0;
    while (first > 0) {
        const size_t r = rows_of(lines[first - 1]);
        if (rows + r > budget) break;
        rows += r;
        --first;
    }
    while (first < lines.size() && visible_width(lines[first]) == 0) ++first;
    if (first > 0) std::cout << kDim << "(earlier messages not shown)" << kReset << "\n";
    for (size_t i = first; i < lines.size(); ++i) std::cout << lines[i] << "\n";
    std::cout << "\n" << std::flush;
}

void print_line(const std::string& text) {
    note_output();
    std::cout << text << "\n";
}

void print_error(const std::string& text) {
    note_output();
    std::cerr << kRed << "error: " << text << kReset << "\n";
}

void print_warning(const std::string& text) {
    note_output();
    std::cerr << kYellow << "warning: " << text << kReset << "\n";
}

void print_usage(const std::string& text) {
    std::cerr << text;
}

// ---------------------------------------------------------------------------
// Chat session header
// ---------------------------------------------------------------------------

void print_banner(const std::string& version,
                  const std::string& provider,
                  const std::string& model,
                  const std::string& url) {
    // ASCII art — kept to 58 columns so it fits inside an 80-col terminal
    // even after the left margin added by most terminal emulators.
    //
    //  ████████╗ █████╗ ██████╗ ████████╗ ██████╗
    //     ██╔══╝██╔══██╗██╔══██╗╚══██╔══╝██╔═══██╗
    //     ██║   ███████║██████╔╝   ██║   ██║   ██║
    //     ██║   ██╔══██║██╔═══╝    ██║   ██║   ██║
    //     ██║   ██║  ██║██║        ██║   ╚██████╔╝
    //     ╚═╝   ╚═╝  ╚═╝╚═╝        ╚═╝    ╚═════╝
    //
    //   ██████╗ ██████╗ ██████╗ ███████╗
    //  ██╔════╝██╔═══██╗██╔══██╗██╔════╝
    //  ██║     ██║   ██║██║  ██║█████╗
    //  ██║     ██║   ██║██║  ██║██╔══╝
    //  ╚██████╗╚██████╔╝██████╔╝███████╗
    //   ╚═════╝ ╚═════╝ ╚═════╝ ╚══════╝

    std::cout
        << "\n"
        << kBold << kCyan
        << "  ████████╗ █████╗ ██████╗ ████████╗ ██████╗ \n"
        << "     ██╔══╝██╔══██╗██╔══██╗╚══██╔══╝██╔═══██╗\n"
        << "     ██║   ███████║██████╔╝   ██║   ██║   ██║\n"
        << "     ██║   ██╔══██║██╔═══╝    ██║   ██║   ██║\n"
        << "     ██║   ██║  ██║██║        ██║   ╚██████╔╝\n"
        << "     ╚═╝   ╚═╝  ╚═╝╚═╝        ╚═╝    ╚═════╝ \n"
        << kReset
        << "\n"
        << kBold
        << "   ██████╗ ██████╗ ██████╗ ███████╗\n"
        << "  ██╔════╝██╔═══██╗██╔══██╗██╔════╝\n"
        << "  ██║     ██║   ██║██║  ██║█████╗  \n"
        << "  ██║     ██║   ██║██║  ██║██╔══╝  \n"
        << "  ╚██████╗╚██████╔╝██████╔╝███████╗\n"
        << "   ╚═════╝ ╚═════╝ ╚═════╝ ╚══════╝\n"
        << kReset
        << "\n";

    // Info line: version + provider/model, then the endpoint on its own line so
    // a long URL doesn't wrap the rest of it.
    std::cout << kDim << "  v" << version
              << "  │  " << provider << "  │  " << model
              << kReset << "\n";
    std::cout << kDim << "  " << url << kReset << "\n";

    // Hints
    std::cout << kDim
              << "  ESC to interrupt  │  /clear  /compact  /resume  /cot on|off  /env  /add-folder <path> [rw]  /add-command <name> <cmd>  /list-commands  /help  /exit"
              << kReset << "\n\n";
}

void print_chat_header(const std::string& provider,
                       const std::string& model,
                       const std::vector<std::string>& tool_names) {
    std::cout << "tapto-code chat - provider: " << provider
              << ", model: " << model << "\n"
              << "Tools: ";
    for (size_t i = 0; i < tool_names.size(); ++i) {
        std::cout << (i ? ", " : "") << tool_names[i];
    }
    std::cout << "\n";
}

void print_chat_hints() {
    std::cout << "Slash commands: /clear, /compact, /resume, /add-folder <path> [rw], /list-commands, "
                 "/add-command <name> <command...>, /exit\n";
}

void print_help(const std::vector<std::string>& tool_names) {
    std::cout << "\n"
              << kBold << "  Slash commands\n" << kReset
              << "    /clear                        clear conversation history\n"
              << "    /compact [focus]              summarize and restart from the summary\n"
              << "    /resume                       continue the saved conversation, or undo /clear\n"
              << "    /cot [on|off|unset]           show / set chain-of-thought output (saved globally)\n"
              << "    /effort [value|unset]         show / set OpenAI reasoning-effort (saved for the project)\n"
              << "    /env                          show session environment (directory, model, history)\n"
              << "    /list-commands                list allow-listed shell commands\n"
              << "    /add-command <name> <cmd>     add a new allow-listed command ([--ro], [--note \"...\"])\n"
              << "    /add-folder <path> [ro|rw]    grant the model a folder for this session: rw to edit\n"
              << "    /remove-folder <path|label>   revoke it\n"
              << "    /list-folders                 show the working directory and what is granted\n"
              << "    /add-plugin <name> [ro|rw]    load a plugin's tools: read tools, or rw for all\n"
              << "    /remove-plugin <name>         take its tools away\n"
              << "    /list-plugins                 show the plugins loaded and installed\n"
              << "\n"
              << "    A setting goes to its default place; --session, --project or --global\n"
              << "    says where instead (/cot, /effort, /add-command, /add-plugin,\n"
              << "    /remove-plugin), and unset removes one there. Folders are never saved.\n"
              << "    /help                         show this help\n"
              << "    /exit                         quit\n"
              << "\n"
              << kBold << "  Key bindings\n" << kReset
              << "    ESC                           interrupt the model while it is working\n"
              << "\n"
              << kBold << "  Active tools\n" << kReset;
    for (const auto& t : tool_names)
        std::cout << "    " << t << "\n";
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// First-run / interactive prompts
// ---------------------------------------------------------------------------

void print_setup_welcome() {
    std::cout << "Welcome to tapto-code. Let's set up your AI provider.\n";
}

void print_setup_provider_prompt() {
    std::cout << "Provider (claude / openai / gemini): " << std::flush;
}

void print_setup_apikey_prompt(const char* env_var_name) {
    std::cout << "API key";
    if (env_var_name && *env_var_name)
        std::cout << " (or leave blank to use $" << env_var_name << ")";
    std::cout << ": " << std::flush;
}

void print_setup_saved(const std::string& path) {
    std::cout << "Saved to " << path << "\n\n";
}

void print_prompt(const std::string& text) {
    std::cout << text << std::flush;
}

void print_prompt_accepted() {
    std::cout << "\n" << std::flush;
}

// ---------------------------------------------------------------------------
// Config / command listing
// ---------------------------------------------------------------------------

void print_config_entry(const std::string& scope,
                        const std::string& key,
                        const std::string& value) {
    if (!scope.empty()) std::cout << scope << "\t";
    std::cout << key << "=" << value << "\n";
}

void print_command_entry(const std::string& scope,
                         const std::string& name,
                         const std::string& command) {
    if (!scope.empty()) std::cout << scope << "\t";
    std::cout << name << " = " << command << "\n";
}

void print_command_added(const std::string& name,
                         const std::string& scope,
                         const std::string& command) {
    std::cout << "Added '" << name << "' (" << scope << "): " << command << "\n";
}

void print_command_removed(const std::string& name, const std::string& scope) {
    std::cout << "Removed '" << name << "' from " << scope << "\n";
}

void print_no_commands() {
    std::cout << "(no commands configured)\n";
}

} // namespace tapto::ui
