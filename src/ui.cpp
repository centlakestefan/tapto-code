// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/ui.h"

#include <iostream>
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

        // Combining / zero-width characters contribute 0 columns.
        if ((cp >= 0x0300 && cp <= 0x036F) ||   // Combining Diacritical Marks
            (cp >= 0x1DC0 && cp <= 0x1DFF) ||   // Combining Diacritical Supplement
            (cp >= 0x20D0 && cp <= 0x20FF) ||   // Combining Diacritical for Symbols
            (cp >= 0xFE20 && cp <= 0xFE2F) ||   // Combining Half Marks
            cp == 0xFE0F ||                       // Variation Selector-16 (emoji style)
            cp == 0x200D) {                       // Zero Width Joiner
            continue;
        }

        // Wide characters (CJK unified, compatibility, radicals, symbols, emoji).
        if ((cp >= 0x1100  && cp <= 0x115F)  ||  // Hangul Jamo
            (cp >= 0x2E80  && cp <= 0x303E)  ||  // CJK Radicals / Kangxi
            (cp >= 0x3041  && cp <= 0x33BF)  ||  // Hiragana … CJK Compatibility
            (cp >= 0x33FF  && cp <= 0xA4CF)  ||  // CJK Unified Ideographs ext.
            (cp >= 0xA960  && cp <= 0xA97F)  ||  // Hangul Jamo Extended-A
            (cp >= 0xAC00  && cp <= 0xD7FF)  ||  // Hangul Syllables
            (cp >= 0xF900  && cp <= 0xFAFF)  ||  // CJK Compatibility Ideographs
            (cp >= 0xFE10  && cp <= 0xFE1F)  ||  // Vertical Forms
            (cp >= 0xFE30  && cp <= 0xFE6F)  ||  // CJK Compatibility Forms
            (cp >= 0xFF00  && cp <= 0xFF60)  ||  // Fullwidth Forms
            (cp >= 0xFFE0  && cp <= 0xFFE6)  ||  // Fullwidth Signs
            (cp >= 0x1B000 && cp <= 0x1B0FF) ||  // Kana Supplement
            (cp >= 0x1F004 && cp <= 0x1F0CF) ||  // Mahjong / Playing Cards
            (cp >= 0x1F300 && cp <= 0x1F9FF) ||  // Misc Symbols, Emoticons, etc.
            (cp >= 0x20000 && cp <= 0x2FFFD) ||  // CJK Extension B–F
            (cp >= 0x30000 && cp <= 0x3FFFD)) {  // CJK Extension G+
            width += 2;
        } else {
            width += 1;
        }
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

// Render a collected table (header row + data rows) into out.
// Columns are padded to uniform width per column.
void render_table(std::string& out,
                  const std::vector<std::string>& header,
                  const std::vector<std::vector<std::string>>& rows) {
    // Compute column widths in visible terminal columns.
    // We strip Markdown syntax markers first (** * ` etc.) because those
    // characters are consumed by render_inline() and never reach the terminal,
    // so counting them would over-estimate the cell width and break alignment.
    // display_width() then handles multi-byte / wide (CJK, emoji) glyphs.
    const size_t ncols = header.size();
    std::vector<size_t> widths(ncols, 0);
    for (size_t c = 0; c < ncols; ++c)
        widths[c] = display_width(strip_inline_markers(header[c]));
    for (const auto& row : rows)
        for (size_t c = 0; c < row.size() && c < ncols; ++c)
            widths[c] = std::max(widths[c], display_width(strip_inline_markers(row[c])));

    // Shrink column widths so the table fits within the terminal.
    // Total rendered width = 1 (left border) + ncols * (1 space + width + 1 space)
    //                      + ncols * 1 (separator/right border) = 1 + ncols*(width+3)
    // We repeatedly shave one column from the widest until we fit, with a floor
    // of 1 column per cell so every column remains at least minimally visible.
    const size_t term_w = terminal_width();
    auto table_w = [&]() -> size_t {
        size_t w = 1; // left border
        for (size_t c = 0; c < ncols; ++c) w += widths[c] + 3; // space + cell + space + separator
        return w;
    };
    while (table_w() > term_w) {
        // Find the widest column that still has room to shrink.
        size_t worst = 0;
        for (size_t c = 1; c < ncols; ++c)
            if (widths[c] > widths[worst]) worst = c;
        if (widths[worst] <= 1) break; // can't shrink any further
        --widths[worst];
    }

    // Helper: emit one row, padding each cell to its column width.
    auto emit_row = [&](const std::vector<std::string>& cells, bool is_header) {
        out += kDim;
        out += "│";
        out += kReset;
        for (size_t c = 0; c < ncols; ++c) {
            const std::string& cell = c < cells.size() ? cells[c] : "";
            // Clip the raw cell to the (possibly reduced) column width before
            // rendering inline Markdown, so the ANSI sequences wrap the clipped text.
            size_t clipped_w = 0;
            std::string clipped = clip_cell(strip_inline_markers(cell), widths[c], clipped_w);
            // Re-apply inline markup only if no clipping occurred (clipped content
            // would have its markers split mid-span). When clipping is needed the
            // plain stripped text with an ellipsis is clear enough.
            std::string rendered = (clipped_w == display_width(strip_inline_markers(cell)))
                ? render_inline(cell)
                : clipped;
            if (is_header)
                out += std::string(" ") + kBold + rendered + kReset;
            else
                out += " " + rendered;
            // Pad to column width using the clipped visible width.
            if (clipped_w < widths[c])
                out.append(widths[c] - clipped_w, ' ');
            out += " ";
            out += kDim;
            out += "│";
            out += kReset;
        }
        out += "\n";
    };

    // Helper: emit a horizontal divider.
    auto emit_divider = [&](bool heavy) {
        out += kDim;
        out += heavy ? "├" : "┼";
        for (size_t c = 0; c < ncols; ++c) {
            for (size_t k = 0; k < widths[c] + 2; ++k) out += "─";
            out += (c + 1 < ncols) ? "┼" : (heavy ? "┤" : "┤");
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

    // Header row + divider.
    emit_row(header, true);
    emit_divider(true);

    // Data rows.
    for (size_t r = 0; r < rows.size(); ++r) {
        emit_row(rows[r], false);
        if (r + 1 < rows.size()) emit_divider(false);
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

std::string render_markdown(const std::string& text) {
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
            render_table(out, table_header, table_rows);
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

void erase_status_line() {
    std::cout << "\r\x1b[K";
}

std::string iteration_prefix(int iteration, int max_iterations) {
    if (iteration <= 0) return "";
    return "[" + std::to_string(iteration) + "/" + std::to_string(max_iterations) + "] ";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Progress / status line
// ---------------------------------------------------------------------------

void set_status(const std::string& text, int iteration, int max_iterations) {
    constexpr size_t kMaxWidth = 78;

    g_status_text = iteration_prefix(iteration, max_iterations) + text;
    if (g_status_text.size() > kMaxWidth)
        g_status_text = g_status_text.substr(0, kMaxWidth - 3) + "...";

    if (!g_status_visible)
        std::cout << "\x1b[?25l"; // hide cursor while the status line is live

    std::cout << "\r" << g_status_text << "\x1b[K" << std::flush;
    g_status_visible = true;
}

void commit_status() {
    if (!g_status_visible) return;
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
    if (g_status_visible) erase_status_line();
    if (is_reasoning)
        std::cout << kDim << text << kReset << "\n" << std::flush;
    else
        std::cout << text << "\n" << std::flush;
}

void print_reply(const std::string& text) {
    std::cout << render_markdown(text) << "\n";
}

void print_line(const std::string& text) {
    std::cout << text << "\n";
}

void print_error(const std::string& text) {
    std::cerr << kRed << "error: " << text << kReset << "\n";
}

void print_warning(const std::string& text) {
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
                  const std::string& model) {
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

    // Info line: version + provider/model
    std::cout << kDim << "  v" << version
              << "  │  " << provider << "  │  " << model
              << kReset << "\n";

    // Hints
    std::cout << kDim
              << "  /clear  /list-commands  /add-command <name> <cmd>  /help  /exit"
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
    std::cout << "Slash commands: /clear, /list-commands, "
                 "/add-command <name> <command...>, /exit\n";
}

void print_help(const std::vector<std::string>& tool_names) {
    std::cout << "\n"
              << kBold << "  Slash commands\n" << kReset
              << "    /clear                        clear conversation history\n"
              << "    /list-commands                list allow-listed shell commands\n"
              << "    /add-command <name> <cmd>     add a new allow-listed command\n"
              << "    /help                         show this help\n"
              << "    /exit                         quit\n"
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
