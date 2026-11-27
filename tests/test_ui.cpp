// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// Markdown table rendering (tapto/termui.h, src/ui.cpp): a table is rendered
// as a box whose columns are padded to a uniform width, and whose cells wrap
// (keeping inline styling) instead of being clipped when they do not fit.
// render_markdown(text, max_width) is a pure function, so these tests drive it
// at explicit widths without a live terminal and check the visible width of
// every emitted line.

#include "tapto/termui.h"

#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check_true(int line, const char* what, bool cond) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
}

void check_eq(int line, const char* what, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected:\n" << expected << "\n"
              << "  actual:\n" << actual << "\n";
}

#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))
#define CHECK_EQ(actual, expected) check_eq(__LINE__, #actual, (actual), (expected))

void check_eq_sz(int line, const char* what, size_t actual, size_t expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected: " << expected << "\n"
              << "  actual:   " << actual << "\n";
}
#define CHECK_EQ_SZ(actual, expected) check_eq_sz(__LINE__, #actual, (actual), (expected))

// --- ANSI / width helpers (independent of the code under test) -------------

// True if the codepoint renders two terminal columns. Mirrors the wide ranges
// the renderer uses; enough to measure the strings these tests emit.
bool wide_cp(uint32_t cp) {
    return (cp >= 0x1100  && cp <= 0x115F)  || (cp >= 0x2E80  && cp <= 0x303E)  ||
           (cp >= 0x3041  && cp <= 0x33BF)  || (cp >= 0x33FF  && cp <= 0xA4CF)  ||
           (cp >= 0xA960  && cp <= 0xA97F)  || (cp >= 0xAC00  && cp <= 0xD7FF)  ||
           (cp >= 0xF900  && cp <= 0xFAFF)  || (cp >= 0xFE10  && cp <= 0xFE1F)  ||
           (cp >= 0xFE30  && cp <= 0xFE6F)  || (cp >= 0xFF00  && cp <= 0xFF60)  ||
           (cp >= 0xFFE0  && cp <= 0xFFE6)  || (cp >= 0x1B000 && cp <= 0x1B0FF) ||
           (cp >= 0x1F004 && cp <= 0x1F0CF) || (cp >= 0x1F300 && cp <= 0x1F9FF) ||
           (cp >= 0x20000 && cp <= 0x2FFFD) || (cp >= 0x30000 && cp <= 0x3FFFD);
}

// Remove ANSI CSI sequences (ESC [ ... final-byte).
std::string strip_ansi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

// Visible terminal width of a string (ANSI-free assumed; wide = 2, else 1).
size_t viswidth(const std::string& s) {
    size_t w = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* end = p + s.size();
    while (p < end) {
        unsigned char b = *p;
        int bytes = 1;
        uint32_t cp = 0;
        if      (b < 0x80)            { cp = b;         bytes = 1; }
        else if ((b & 0xE0) == 0xC0)  { cp = b & 0x1F;  bytes = 2; }
        else if ((b & 0xF0) == 0xE0)  { cp = b & 0x0F;  bytes = 3; }
        else if ((b & 0xF8) == 0xF0)  { cp = b & 0x07;  bytes = 4; }
        else { ++p; continue; }
        for (int i = 1; i < bytes && p + i < end; ++i)
            cp = (cp << 6) | (p[i] & 0x3F);
        w += wide_cp(cp) ? 2 : 1;
        p += bytes;
    }
    return w;
}

// Split a rendered string into its visual lines. A trailing newline (as
// render_table emits one after its bottom border) does not yield a trailing
// empty line; internal blank lines are preserved.
std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else            { cur += c; }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// True when every visual line of `rendered` (ANSI stripped) is at most
// `max_width` visible columns wide.
bool all_lines_fit(const std::string& rendered, size_t max_width) {
    for (const auto& ln : lines_of(rendered))
        if (viswidth(strip_ansi(ln)) > max_width) return false;
    return true;
}

} // namespace

int main() {
    using tapto::ui::render_markdown;
    using namespace tapto::ui; // the headless output below

    // --- A table that fits renders as a clean, aligned box ------------------
    {
        const std::string md =
            "| A | B |\n"
            "| --- | --- |\n"
            "| 1 | 2 |";
        const std::string rendered = render_markdown(md, /*max_width=*/80);

        // Strip the styling and compare the exact box: columns are one column
        // wide, padded to that width, and the whole thing is nine columns.
        const std::string expected =
            "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xAC\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90" // ┌───┬───┐
            "\n"
            "\xE2\x94\x82 A \xE2\x94\x82 B \xE2\x94\x82"  // │ A │ B │
            "\n"
            "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xA4" // ├───┼───┤
            "\n"
            "\xE2\x94\x82 1 \xE2\x94\x82 2 \xE2\x94\x82"  // │ 1 │ 2 │
            "\n"
            "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xB4\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x98"; // └───┴───┘

        // render_table ends the block with a newline after the bottom border.
        CHECK_EQ(strip_ansi(rendered), expected + "\n");

        const auto lines = lines_of(rendered);
        CHECK_EQ_SZ(lines.size(), size_t{5}); // top, header, divider, row, bottom
        CHECK_TRUE(all_lines_fit(rendered, 80));
    }

    // --- A table too wide for the terminal wraps: it fits and nothing is
    //     clipped (no ellipsis), and every word is still present ------------
    {
        const std::string md =
            "| w1 | w2 | w3 |\n"
            "| --- | --- | --- |\n"
            "| a b c d | e f g h | i j k l |\n";
        const size_t max_width = 20;
        const std::string rendered = render_markdown(md, max_width);

        // Core invariant the wrapping path exists to hold: every line fits.
        CHECK_TRUE(all_lines_fit(rendered, max_width));

        // No clipping: the renderer wraps, it does not truncate with an ellipsis.
        CHECK_TRUE(!contains(rendered, "\xE2\x80\xA6")); // "…"

        // Content is preserved (words that wrap onto continuation lines keep
        // their text). Check the data cell's words survived.
        const std::string plain = strip_ansi(rendered);
        for (char c = 'a'; c <= 'l'; ++c)
            CHECK_TRUE(contains(plain, std::string(1, c)));
    }

    // --- Inline styling survives the wrap ----------------------------------
    // The data cell carries bold/italic/code; when it wraps, the styled words
    // must still carry their ANSI open codes (italic and code can only come
    // from the data cell, since the header is a plain word).
    {
        const std::string md =
            "| Item |\n"
            "| --- |\n"
            "| **bold** *ital* `code` plain extra words to force wrap |\n";
        const size_t max_width = 24;
        const std::string rendered = render_markdown(md, max_width);

        CHECK_TRUE(all_lines_fit(rendered, max_width));
        CHECK_TRUE(!contains(rendered, "\xE2\x80\xA6")); // no clipping
        // Italic and code styling made it through the wrap intact.
        CHECK_TRUE(contains(rendered, "\x1b[3m"));   // italic open
        CHECK_TRUE(contains(rendered, "\x1b[36m"));  // code open
        CHECK_TRUE(contains(rendered, "\x1b[1m"));   // bold open (header + bold word)
    }

    // --- Wide (CJK) cells are measured at two columns each ------------------
    // "日本語" is three codepoints, six columns. As a single column that fits,
    // the box is six columns of cell plus the two-column frame padding.
    {
        const std::string md =
            "| 日本語 |\n"
            "| --- |\n"
            "| abcde |";
        const size_t max_width = 80;
        const std::string rendered = render_markdown(md, max_width);

        CHECK_TRUE(all_lines_fit(rendered, max_width));
        const auto lines = lines_of(rendered);
        CHECK_EQ_SZ(lines.size(), size_t{5});
        // The header cell is six columns wide (three CJK glyphs, two each).
        // The row renders as │ <cell padded to 6> │ = 1 + 1 + 6 + 1 + 1.
        CHECK_EQ_SZ(viswidth(strip_ansi(lines[1])), size_t{10});
        CHECK_TRUE(contains(rendered, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E")); // 日本語
    }

    // --- A '|' in prose is not a table: only a header with a delimiter row
    //     under it starts one ------------------------------------------------
    {
        const std::string box = "\xE2\x94\x8C"; // ┌, the table's top-left corner
        // A sentence, a shell pipe, and lines with pipes but no delimiter row.
        const std::string prose =
            "Run `dir | findstr foo` to filter.\n"
            "a | b\n"
            "c | d\n";
        const std::string r1 = strip_ansi(render_markdown(prose, 80));
        CHECK_TRUE(!contains(r1, box));
        CHECK_TRUE(contains(r1, "a | b"));
        CHECK_TRUE(contains(r1, "c | d"));

        // A delimiter row with the wrong number of cells is no table either.
        const std::string r2 = strip_ansi(render_markdown("| a | b |\n| --- |\n", 80));
        CHECK_TRUE(!contains(r2, box));

        // A real table ends at a blank line (or one without a '|'); a pipe in
        // prose after it stays prose.
        const std::string md =
            "| A | B |\n"
            "|:--|--:|\n"
            "| 1 | x \\| y |\n"
            "\n"
            "Then pipe it: a | b";
        const std::string r3 = strip_ansi(render_markdown(md, 80));
        CHECK_TRUE(contains(r3, box));
        CHECK_TRUE(contains(r3, "x | y")); // \| is a pipe inside the cell
        CHECK_TRUE(contains(r3, "Then pipe it: a | b"));
        CHECK_EQ_SZ(lines_of(r3).size(), size_t{7}); // the box of 5, blank, the line
    }

    // --- Headless (--headless): the reply alone on stdout, as it came; -----
    // the tool calls and notices on stderr, a line each, none redrawn in
    // place. Last, since it switches the output for the rest of the run.
    {
        std::ostringstream out, err;
        std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
        std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());
        set_headless(true);
        begin_turn_output();
        set_status("Thinking", 1, 10);
        end_status();                      // a status the turn moves past: not written
        set_status("View note.txt", 2, 10);
        commit_status();                   // a tool call: written as a line
        emit_intermediate("Let me look.", false, true);
        print_line("(a notice)");
        print_reply("\n**The word** is `pelican`.\n\n");
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);

        CHECK_EQ(out.str(), "**The word** is `pelican`.\n");
        const std::string e = strip_ansi(err.str());
        CHECK_TRUE(!contains(e, "Thinking"));
        CHECK_TRUE(contains(e, "View note.txt\n"));
        CHECK_TRUE(contains(e, "Let me look.\n"));
        CHECK_TRUE(contains(e, "(a notice)\n"));
        CHECK_TRUE(!contains(err.str(), "\r"));       // nothing redrawn in place
        CHECK_TRUE(!contains(err.str(), "\x1b[?25")); // the cursor left alone
    }

    // --- The slash-command menu ----------------------------------------------
    // The commands a typed prefix matches, in name order, and the menu's rows:
    // one per command shown, the selected one marked, none wider than the
    // terminal allows (the editor counts on each taking exactly one row).
    {
        using tapto::ui::SlashCommand;
        using tapto::ui::slash_menu_lines;
        using tapto::ui::slash_menu_matches;
        const std::vector<SlashCommand> commands = {
            {"resume", "", "continue the saved conversation"},
            {"add-folder", "<path> [ro|rw]", "grant the model a folder"},
            {"clear", "", "clear the conversation"},
            {"add-command", "<name> <command...>", "allow-list a command"},
            {"cot", "[on|off|unset]", "show or set chain-of-thought output"},
        };

        // A prefix narrows, sorted by name; the bare '/' matches them all.
        auto names = [](const std::vector<SlashCommand>& cs) {
            std::string out;
            for (const auto& c : cs) out += (out.empty() ? "" : ",") + c.name;
            return out;
        };
        CHECK_EQ(names(slash_menu_matches(commands, "add")), "add-command,add-folder");
        CHECK_EQ(names(slash_menu_matches(commands, "c")), "clear,cot");
        CHECK_EQ(names(slash_menu_matches(commands, "")),
                 "add-command,add-folder,clear,cot,resume");
        CHECK_EQ(names(slash_menu_matches(commands, "clear")), "clear");
        // Longer than every name, or matching none: nothing to show.
        CHECK_TRUE(slash_menu_matches(commands, "clearer").empty());
        CHECK_TRUE(slash_menu_matches(commands, "x").empty());

        // All of them fit: a row each, the usage and the description in it,
        // and the marker on the selected row alone.
        const auto all = slash_menu_matches(commands, "");
        auto lines = slash_menu_lines(all, 1, 8, 80);
        CHECK_EQ_SZ(lines.size(), all.size());
        CHECK_TRUE(contains(strip_ansi(lines[1]), "/add-folder <path> [ro|rw]"));
        CHECK_TRUE(contains(strip_ansi(lines[1]), "grant the model a folder"));
        const std::string marker = "\xE2\x96\xB8"; // ▸
        CHECK_TRUE(contains(lines[1], marker));
        for (size_t i = 0; i < lines.size(); ++i)
            if (i != 1) CHECK_TRUE(!contains(lines[i], marker));
        // Every row ends in a reset, so no style leaks into the next one.
        for (const auto& l : lines)
            CHECK_TRUE(l.size() >= 4 && l.compare(l.size() - 4, 4, "\x1b[0m") == 0);
        // The descriptions line up: they start in the same column.
        const size_t col0 = strip_ansi(lines[0]).find("allow-list");
        const size_t col2 = strip_ansi(lines[2]).find("clear the");
        CHECK_EQ_SZ(col0, col2);

        // Narrow: every row is cut short of the last column, with an ellipsis.
        lines = slash_menu_lines(all, 0, 8, 20);
        for (const auto& l : lines) CHECK_TRUE(viswidth(strip_ansi(l)) < 20);
        CHECK_TRUE(contains(lines[0], "\xE2\x80\xA6"));

        // More than fit: the window scrolls to keep the selection in view, and
        // a last row counts where it is.
        lines = slash_menu_lines(all, 4, 2, 80);
        CHECK_EQ_SZ(lines.size(), size_t{3});
        CHECK_TRUE(contains(strip_ansi(lines[0]), "/cot"));
        CHECK_TRUE(contains(strip_ansi(lines[1]), "/resume"));
        CHECK_TRUE(contains(lines[1], marker));
        CHECK_TRUE(contains(strip_ansi(lines[2]), "(5 of 5)"));
        lines = slash_menu_lines(all, 0, 2, 80);
        CHECK_TRUE(contains(strip_ansi(lines[0]), "/add-command"));
        CHECK_TRUE(contains(strip_ansi(lines[2]), "(1 of 5)"));

        // A selection past the end is taken as the last; no matches, no rows.
        lines = slash_menu_lines(all, 99, 8, 80);
        CHECK_TRUE(contains(lines.back(), marker));
        CHECK_TRUE(slash_menu_lines({}, 0, 8, 80).empty());
    }

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
