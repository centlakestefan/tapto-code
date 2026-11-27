// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/termui.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
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
        if (line[i] == '\\' && i + 1 < n && line[i + 1] == '|') {
            cell += '|'; // \| is a pipe inside the cell
            ++i;
        } else if (line[i] == '|') {
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

// Returns true if the line could be a table row (contains at least one '|').
// Only a row with a delimiter row under it starts a table (table_rows_of).
bool is_table_row(const std::string& line) {
    return line.find('|') != std::string::npos;
}

// The GFM delimiter row under a header of `columns` cells: a '|' in it, and
// one cell of the form :?-+:? per column. A line of prose with a '|' in it
// has none under it, so it stays prose.
bool is_delimiter_row(const std::string& line, size_t columns) {
    if (!is_table_row(line) || !is_table_separator(line)) return false;
    const auto cells = split_table_row(line);
    if (cells.size() != columns) return false;
    for (const auto& c : cells) {
        size_t b = 0, e = c.size();
        if (b < e && c[b] == ':') ++b;
        if (b < e && c[e - 1] == ':') --e;
        if (b == e || c.find_first_not_of('-', b) < e) return false;
    }
    return true;
}

// What each line is in a table: 0 not in one, 1 the header, 2 the delimiter
// row, 3 a body row. A table runs from a header with its delimiter row to
// the first line without a '|' (or a blank line, or a code fence).
std::vector<char> table_rows_of(const std::vector<std::string>& lines) {
    std::vector<char> role(lines.size(), 0);
    bool in_fence = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].compare(0, 3, "```") == 0) { in_fence = !in_fence; continue; }
        if (in_fence || !is_table_row(lines[i]) || i + 1 >= lines.size()) continue;
        if (!is_delimiter_row(lines[i + 1], split_table_row(lines[i]).size())) continue;
        role[i] = 1;
        role[i + 1] = 2;
        size_t j = i + 2;
        while (j < lines.size() && is_table_row(lines[j]) &&
               lines[j].compare(0, 3, "```") != 0)
            role[j++] = 3;
        i = j - 1;
    }
    return role;
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

// `s` without its ANSI escape sequences (colors, styles).
std::string strip_ansi(const std::string& s) {
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
    return plain;
}

// The columns a rendered line takes: display_width without the ANSI escape
// sequences, which take none.
size_t visible_width(const std::string& s) {
    return display_width(strip_ansi(s));
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

    const std::vector<char> table_role = table_rows_of(lines);

    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& raw = lines[li];

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
        // Only rows that table_rows_of found under a header and delimiter row:
        // a '|' in a sentence is not a table.
        if (table_role[li] == 1) {
            flush_table(); // a table straight after another
            in_table     = true;
            table_header = split_table_row(raw);
            continue;
        }
        if (table_role[li] == 2) continue; // the delimiter row
        if (table_role[li] == 3) {
            table_rows.push_back(split_table_row(raw));
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

// What set_status was last given, so count_stream can redraw the line with
// its counter and keep the iteration counter.
std::string g_status_base;
int         g_status_iteration = 0;
int         g_status_max = 0;

// count_stream: the characters of the current request, and when the line
// was last redrawn for them.
std::size_t g_count_reasoning = 0;
std::size_t g_count_text = 0;
std::chrono::steady_clock::time_point g_count_drawn;

// refresh_status: once it runs for a turn, the status line also shows how
// long the request (or tool) has run (when g_status_elapsed), and what the
// plugins say. The text draw_status was last given, so the line can be redrawn.
bool        g_status_ticking = false;
bool        g_status_elapsed = true;
std::string g_status_details;
std::string g_status_drawn;
std::chrono::steady_clock::time_point g_status_since;

// Taken by every function here that writes to the terminal during a turn:
// refresh_status() runs on a thread of its own. Recursive, since some of
// them call others.
std::recursive_mutex g_out_mutex;
using OutLock = std::lock_guard<std::recursive_mutex>;

// --- Headless (set_headless) ------------------------------------------------

bool g_headless = false;
bool g_stderr_tty = true; // whether stderr keeps its ANSI styling, headless

bool stderr_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

// One line of headless progress, to stderr.
void headless_line(const std::string& text) {
    if (g_stderr_tty) std::cerr << text << kReset << "\n" << std::flush;
    else std::cerr << strip_ansi(text) << "\n" << std::flush;
}

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

void set_headless(bool on) {
    g_headless = on;
    g_stderr_tty = stderr_is_terminal();
}

namespace {

// What refresh_status adds after the status text, dimmed: " · 45s · 27 tok/s
// · 1 user · cache 63%". Empty until it has run this turn, and when there is
// neither a time to show nor details.
std::string status_suffix() {
    if (!g_status_ticking) return "";
    std::string s;
    if (g_status_elapsed) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - g_status_since)
                              .count();
        s += " \xC2\xB7 "; // ·
        s += secs < 60 ? std::to_string(secs) + "s"
                       : std::to_string(secs / 60) + "m " + std::to_string(secs % 60) + "s";
    }
    if (!g_status_details.empty()) s += " \xC2\xB7 " + g_status_details;
    return s.empty() ? s : std::string(kDim) + s + kReset;
}

// Write the status line as it stands, with its suffix. The committed line
// (commit_status) is the text alone.
void write_status_line() {
    constexpr size_t kMaxWidth = 78;
    size_t visible = 0;
    std::cout << "\r" << clip_cell(g_status_text + status_suffix(), kMaxWidth, visible) << kReset
              << "\x1b[K" << std::flush;
}

void draw_status(const std::string& text, int iteration, int max_iterations) {
    constexpr size_t kMaxWidth = 78;

    // clip_cell measures visible columns (UTF-8/wide-aware) and skips ANSI
    // escapes, so the counter's color codes don't count toward the width.
    size_t visible = 0;
    g_status_text = clip_cell(iteration_prefix(iteration, max_iterations) + text,
                              kMaxWidth, visible);
    g_status_drawn = text;

    note_output();
    if (!g_status_visible)
        std::cout << "\x1b[?25l"; // hide cursor while the status line is live

    write_status_line();
    g_status_visible = true;
}

} // namespace

void refresh_status(bool elapsed, const std::string& details) {
    OutLock lock(g_out_mutex);
    if (g_headless) return;
    g_status_elapsed = elapsed;
    g_status_details = details;
    g_status_ticking = true;
    // Redrawn in place, without counting as output: a streamed reply still
    // on the screen stays redrawable as Markdown (note_output).
    if (g_status_visible) write_status_line();
}

void set_status(const std::string& text, int iteration, int max_iterations) {
    OutLock lock(g_out_mutex);
    g_status_since = std::chrono::steady_clock::now(); // the time shown is this request's
    // Each request starts with one: a new count.
    g_count_reasoning = g_count_text = 0;
    if (g_headless) {
        // Not redrawn in place: kept, and written as a line once committed.
        g_status_text = iteration_prefix(iteration, max_iterations) + text;
        return;
    }
    g_status_base = text;
    g_status_iteration = iteration;
    g_status_max = max_iterations;
    draw_status(text, iteration, max_iterations);
}

void count_stream(const tapto::StreamEvent& ev) {
    using Kind = tapto::StreamEvent::Kind;
    if (g_headless || ev.kind == Kind::End || ev.text.empty()) return;
    OutLock lock(g_out_mutex);

    // Characters, not bytes: each starts with an ASCII or a UTF-8 lead byte.
    std::size_t chars = 0;
    for (unsigned char c : ev.text) chars += (c & 0xC0) != 0x80;
    (ev.kind == Kind::Reasoning ? g_count_reasoning : g_count_text) += chars;

    // At most ten redraws a second: a local model sends a piece per token.
    const auto now = std::chrono::steady_clock::now();
    if (g_status_visible && now - g_count_drawn < std::chrono::milliseconds(100)) return;
    g_count_drawn = now;

    const bool writing = ev.kind == Kind::Text;
    const std::size_t n = writing ? g_count_text : g_count_reasoning;
    draw_status((writing ? std::string("Writing...") : g_status_base) + " (" + std::to_string(n) +
                    (n == 1 ? " char)" : " chars)"),
                g_status_iteration, g_status_max);
}

void commit_status() {
    OutLock lock(g_out_mutex);
    if (g_headless) {
        if (!g_status_text.empty()) headless_line(g_status_text);
        g_status_text.clear();
        return;
    }
    if (!g_status_visible) return;
    note_output();
    std::cout << "\r" << g_status_text << "\x1b[K\n" << std::flush;
    g_status_visible = false;
    g_status_text.clear();
}

void end_status() {
    OutLock lock(g_out_mutex);
    // The turn's ticking ends with its status line; the next turn's
    // refresh_status starts it again.
    g_status_ticking = false;
    g_status_details.clear();
    if (g_headless) {
        g_status_text.clear();
        return;
    }
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
    OutLock lock(g_out_mutex);
    if (g_headless) {
        headless_line(is_reasoning ? std::string(kDim) + text : text);
        return;
    }
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

// The time of day, HH:MM:SS.
std::string clock_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

// The columns the last row of `text` takes, as written from column 0 and
// wrapped at `cols`; a trailing line end is not a row of its own.
size_t last_row_width(std::string text, size_t cols) {
    if (!text.empty() && text.back() == '\n') text.pop_back();
    const size_t nl = text.rfind('\n');
    const size_t w = visible_width(nl == std::string::npos ? text : text.substr(nl + 1));
    cols = std::max<size_t>(cols, 1);
    return w == 0 ? 0 : (w % cols == 0 ? cols : w % cols);
}

} // namespace

void stamp_time_above(size_t used, size_t cols) {
    if (g_headless || cols == 0) return;
    OutLock lock(g_out_mutex);
    // HH:MM:SS ending one column short of the edge (writing the last column
    // can wrap), and only with two blank columns between it and the text.
    constexpr size_t kWidth = 8;
    if (used + 2 + kWidth + 1 > cols) return;
    const size_t col = cols - kWidth; // 1-based column the stamp starts at
    std::cout << "\x1b[1A\x1b[" << col << "G" << kDim << clock_now() << kReset << "\r\n"
              << std::flush;
}

void begin_turn_output() {
    OutLock lock(g_out_mutex);
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
    OutLock lock(g_out_mutex);
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
    OutLock lock(g_out_mutex);
    if (g_headless) {
        std::cout << trimmed(text) << "\n" << std::flush;
        return;
    }
    const size_t cols = terminal_width();
    const std::string reply = trimmed(text);
    const std::string streamed = trimmed(g_last_text);

    // Streamed already. When the plain text is still the last thing on the
    // screen and all of it is in view, it is redrawn as Markdown in its place;
    // otherwise it stays as it is. What the reply adds after it (a note that
    // it was cut short) is printed below.
    //
    // Each case ends with the cursor on the row below the reply's last, and
    // the time it finished goes at the right of that last row.
    if (!streamed.empty() && reply.compare(0, streamed.size(), streamed) == 0) {
        const size_t rows = rows_taken(g_last_text_shown, cols);
        const bool in_view = g_last_text_seq == g_output_seq && rows + 1 < terminal_height();
        if (in_view) {
            const std::string shown = render_markdown_impl(text, cols);
            std::cout << "\x1b[" << rows << "F\x1b[J" << shown;
            stamp_time_above(last_row_width(shown, cols), cols);
        } else {
            const std::string rest = trimmed(reply.substr(streamed.size()));
            if (!rest.empty()) {
                const std::string shown = render_markdown_impl(rest, cols);
                std::cout << shown;
                stamp_time_above(last_row_width(shown, cols), cols);
            } else if (g_last_text_seq == g_output_seq) {
                stamp_time_above(last_row_width(g_last_text_shown, cols), cols);
            }
        }
        std::cout << "\n";
        begin_turn_output();
        return;
    }
    // A reply that is only chain of thought was shown as it streamed, dimmed.
    if (!reply.empty() && reply == trimmed(g_last_reasoning)) {
        std::cout << "\n";
        begin_turn_output();
        return;
    }
    const std::string shown = render_markdown_impl(text, cols);
    std::cout << shown;
    stamp_time_above(last_row_width(shown, cols), cols);
    std::cout << "\n";
    begin_turn_output();
}

namespace {

// Every entry rendered as it was on the screen, one string per line: the
// prompt in the prompt color behind its marker with a blank line on either
// side, the chain of thought dimmed, each tool call behind its request's
// counter, the model's text as Markdown.
std::vector<std::string> transcript_lines(const std::vector<TranscriptEntry>& entries, size_t cols) {
    using Who = TranscriptEntry::Who;
    std::vector<std::string> lines;
    const TranscriptEntry* prev = nullptr;
    for (const auto& e : entries) {
        const bool apart = e.who == Who::User || e.who == Who::Summary;
        if (prev && (apart || prev->who == Who::User || prev->who == Who::Summary))
            lines.emplace_back();
        prev = &e;
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
                            "(the conversation before this was summarized)" + kReset);
            break;
        case TranscriptEntry::Who::Reasoning: {
            std::istringstream in(e.text);
            std::string l;
            while (std::getline(in, l)) {
                std::string shown;
                for (char c : l) shown += c == '\t' ? std::string("    ") : std::string(1, c);
                block.push_back(std::string(kDim) + shown + kReset);
            }
            break;
        }
        case TranscriptEntry::Who::Tool:
            block.push_back(iteration_prefix(e.iteration, 0) + e.text);
            break;
        }
        lines.insert(lines.end(), block.begin(), block.end());
    }
    return lines;
}

} // namespace

void redraw_transcript(const std::vector<TranscriptEntry>& entries) {
    if (g_headless) return;
    end_status();
    // the screen, the scrollback, the cursor home
    std::cout << "\x1b[0m\x1b[2J\x1b[3J\x1b[H";
    const size_t cols = std::max<size_t>(terminal_width(), 1);
    for (const auto& l : transcript_lines(entries, cols)) std::cout << l << "\n";
    if (!entries.empty()) std::cout << "\n";
    std::cout << std::flush;
    note_output();
}

void print_transcript(const std::vector<TranscriptEntry>& entries, const std::string& after) {
    if (entries.empty()) return;
    const size_t cols = std::max<size_t>(terminal_width(), 1);
    // The rows a line takes, wrapped at the terminal's width.
    auto rows_of = [&](const std::string& line) {
        const size_t w = visible_width(line);
        return w == 0 ? size_t{1} : (w + cols - 1) / cols;
    };
    const std::vector<std::string> lines = transcript_lines(entries, cols);

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
    OutLock lock(g_out_mutex);
    if (g_headless) {
        headless_line(text);
        return;
    }
    note_output();
    std::cout << text << "\n";
}

void print_error(const std::string& text) {
    OutLock lock(g_out_mutex);
    if (g_headless && !g_stderr_tty) {
        std::cerr << "error: " << strip_ansi(text) << "\n" << std::flush;
        return;
    }
    // A status line still up (a turn that failed partway) goes first, or
    // the message would run on from it.
    end_status();
    note_output();
    std::cerr << kRed << "error: " << text << kReset << "\n";
}

void print_warning(const std::string& text) {
    OutLock lock(g_out_mutex);
    if (g_headless && !g_stderr_tty) {
        std::cerr << "warning: " << strip_ansi(text) << "\n" << std::flush;
        return;
    }
    // A status line still up (a turn that failed partway) goes first, or
    // the message would run on from it.
    end_status();
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

void print_help(const std::vector<SlashCommand>& commands,
                const std::vector<std::string>& tool_names) {
    // The commands from the table the prompt's menu shows too, so the two
    // cannot drift apart: the usage padded to a column, then what it does.
    std::cout << "\n" << kBold << "  Slash commands\n" << kReset;
    for (const auto& c : commands) {
        std::string usage = "/" + c.name + (c.args.empty() ? "" : " " + c.args);
        const size_t w = display_width(usage);
        usage += w < 30 ? std::string(30 - w, ' ') : std::string(" ");
        std::cout << "    " << usage << c.description << "\n";
    }
    std::cout << "\n"
              << "    A setting goes to its default place; --session, --project or --global\n"
              << "    says where instead (/cot, /effort, /add-command, /add-plugin,\n"
              << "    /remove-plugin), and unset removes one there. Folders are never saved.\n"
              << "\n"
              << kBold << "  Key bindings\n" << kReset
              << "    ESC                           interrupt the model while it is working;\n"
              << "                                  at the prompt, clear what you typed\n"
              << "    /                             a menu of the commands as you type one:\n"
              << "                                  Up/Down select, Tab or Enter fills the name\n"
              << "                                  in, Enter on a whole name runs it, ESC closes it\n"
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
// Slash-command menu
// ---------------------------------------------------------------------------

std::vector<SlashCommand> slash_menu_matches(const std::vector<SlashCommand>& commands,
                                             const std::string& typed) {
    std::vector<SlashCommand> out;
    for (const auto& c : commands) {
        if (c.name.size() >= typed.size() && c.name.compare(0, typed.size(), typed) == 0)
            out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const SlashCommand& a, const SlashCommand& b) { return a.name < b.name; });
    return out;
}

std::vector<std::string> slash_menu_lines(const std::vector<SlashCommand>& matches,
                                          size_t selected, size_t max_rows, size_t cols) {
    std::vector<std::string> lines;
    if (matches.empty() || max_rows == 0 || cols < 8) return lines;
    if (selected >= matches.size()) selected = matches.size() - 1;

    // The window shown: from the top, scrolled just far enough that the
    // selected entry is its last row.
    const size_t rows = std::min(max_rows, matches.size());
    const size_t first = selected >= rows ? selected - rows + 1 : 0;

    // The usages are padded to one width, so the descriptions line up; a very
    // long usage is not allowed to push every description off the screen.
    std::vector<std::string> usages;
    size_t usage_cols = 0;
    for (size_t i = first; i < first + rows; ++i) {
        const SlashCommand& c = matches[i];
        usages.push_back("/" + c.name + (c.args.empty() ? "" : " " + c.args));
        usage_cols = std::max(usage_cols, display_width(usages.back()));
    }
    usage_cols = std::min(usage_cols, size_t{32});

    // Never into the last column: a row that fills it wraps on some
    // terminals, and the editor counts the rows it drew to move back up.
    const size_t limit = cols - 1;
    auto fit = [&](const std::string& styled) {
        if (visible_width(styled) <= limit) return styled + kReset;
        size_t w = 0;
        return clip_cell(styled, limit - 1, w) + kReset; // the ellipsis takes the last one
    };

    for (size_t k = 0; k < rows; ++k) {
        const size_t i = first + k;
        std::string usage = usages[k];
        const size_t uw = display_width(usage);
        if (uw < usage_cols) usage.append(usage_cols - uw, ' ');
        const std::string& desc = matches[i].description;
        std::string row;
        if (i == selected) {
            row = std::string(kBold) + kPromptColor + "\xE2\x96\xB8 " + usage; // ▸
            if (!desc.empty()) row += "  " + desc;
        } else {
            row = "  " + usage;
            if (!desc.empty()) row += std::string("  ") + kDim + desc;
        }
        lines.push_back(fit(row));
    }
    // Say so when the window hides some: the menu scrolls only as the
    // selection moves, so they are otherwise easy to miss.
    if (rows < matches.size()) {
        lines.push_back(fit(std::string(kDim) + "  (" + std::to_string(selected + 1) + " of " +
                            std::to_string(matches.size()) + ")"));
    }
    return lines;
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
