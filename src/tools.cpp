// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/tools.h"
#include "tapto/commands.h"

#include "tapto/context.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace tapto {

namespace {

// --- small helpers --------------------------------------------------------

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur); // trailing segment (empty if text ended with '\n')
    return lines;
}

// --- line endings ---------------------------------------------------------
//
// Files are read and written byte-exact, and `view` renders each line with its
// CR stripped. The model therefore composes edits in LF terms while a file
// checked out on Windows holds CRLF, so an edit has to treat a line ending as a
// line ending rather than as two particular bytes — otherwise every multi-line
// str_replace on such a file fails to match.
//
// Files with a long Windows history can be mixed, which is why matching is a
// per-character scan rather than normalise-then-compare: every byte outside the
// edited span keeps whatever ending it already had.

// Length of the line ending at s[i], or 0 if there isn't one there. A lone CR
// is deliberately not an ending: in a file that is otherwise LF or CRLF it is
// far more likely to be data than a line break.
size_t eol_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') return 2;
    if (s[i] == '\n') return 1;
    return 0;
}

// Match `needle` against `content` starting at `at`, with any line ending
// matching any other. Returns the end offset of the match (which may differ
// from at + needle.size(), since the endings can be of different lengths), or
// npos if it doesn't match here.
size_t match_at(const std::string& content, const std::string& needle, size_t at) {
    size_t i = at, j = 0;
    while (j < needle.size()) {
        if (const size_t n = eol_len(needle, j)) {
            const size_t c = eol_len(content, i);
            if (c == 0) return std::string::npos;
            i += c;
            j += n;
        } else {
            if (i >= content.size() || content[i] != needle[j]) return std::string::npos;
            ++i;
            ++j;
        }
    }
    return i;
}

// Every non-overlapping match of `needle`, comparing line endings loosely.
std::vector<std::pair<size_t, size_t>> find_all_loose(const std::string& content,
                                                      const std::string& needle) {
    std::vector<std::pair<size_t, size_t>> hits;
    if (needle.empty()) return hits;
    for (size_t i = 0; i < content.size();) {
        const size_t end = match_at(content, needle, i);
        if (end == std::string::npos) {
            ++i;
        } else {
            hits.push_back({i, end});
            i = (end > i) ? end : i + 1;
        }
    }
    return hits;
}

// The ending style a stretch of text uses. Text with no ending in it at all
// inherits `fallback`, which is how a single-line replacement in a CRLF file
// still gets CRLF when it turns into several lines.
std::string eol_style(const std::string& s, const std::string& fallback) {
    size_t crlf = 0, lf = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t n = eol_len(s, i);
        if (n == 2) { ++crlf; i += 2; }
        else if (n == 1) { ++lf; ++i; }
        else ++i;
    }
    if (crlf == 0 && lf == 0) return fallback;
    return crlf >= lf ? "\r\n" : "\n";
}

// Rewrite every line ending in `text` as `eol`.
std::string with_eol(const std::string& text, const std::string& eol) {
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (size_t i = 0; i < text.size();) {
        if (const size_t n = eol_len(text, i)) {
            out += eol;
            i += n;
        } else {
            out += text[i++];
        }
    }
    return out;
}

// Byte offset where line `line` starts (0-based, counting line endings). Past
// the last line this is the end of the content, so inserting there appends.
size_t line_start_offset(const std::string& s, int line) {
    size_t off = 0;
    int seen = 0;
    while (seen < line && off < s.size()) {
        if (const size_t n = eol_len(s, off)) {
            off += n;
            ++seen;
        } else {
            ++off;
        }
    }
    return off;
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return out.good();
}

// Glob match supporting '*' (any run) and '?' (single char). Iterative with
// backtracking so it stays O(n*m) without recursion.
bool wildcard_match(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool is_noise_dir(const std::string& name) {
    return name == ".git" || name == "build" || name == "node_modules" ||
           name == ".tapto" || name == ".vs" || name == ".vscode";
}

// --- path sandbox ---------------------------------------------------------

// The directory tapto-code was started in (canonicalized). All model-driven file
// access is confined to this subtree. Computed once on first use; the process
// never changes its working directory.
const fs::path& sandbox_root() {
    static const fs::path root = []() {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (ec) return fs::path(".");
        fs::path canon = fs::weakly_canonical(cwd, ec);
        return ec ? cwd : canon;
    }();
    return root;
}

// Resolve `input` (absolute, or relative to the sandbox root) and confirm it
// stays within the root subtree. weakly_canonical normalizes ".."/"." and
// resolves symlinks in the existing prefix, so attempts to escape via those are
// caught. On success fills `out` with the resolved absolute path and returns
// true; otherwise sets `error` and returns false.
bool resolve_in_sandbox(const std::string& input, fs::path& out, std::string& error) {
    const fs::path& root = sandbox_root();
    std::error_code ec;
    fs::path in_path(input);
    fs::path abs = in_path.is_absolute() ? in_path : (root / in_path);
    fs::path resolved = fs::weakly_canonical(abs, ec);
    if (ec) resolved = abs.lexically_normal();

    fs::path rel = resolved.lexically_relative(root);
    if (rel.empty() || *rel.begin() == fs::path("..")) {
        error = "ERROR: '" + input + "' is outside the working directory. "
                "tapto-code can only access the folder it was started in and its "
                "subdirectories.";
        return false;
    }
    out = resolved;
    return true;
}

// --- text editor tool -----------------------------------------------------

std::string execute_text_editor(Context& /*context*/, const json& in) {
    try {
        if (!in.contains("command")) return "ERROR: 'command' not present.";
        std::string command = in["command"];

        if (command == "view") {
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_in_sandbox(in["path"].get<std::string>(), path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            std::error_code ec;
            if (!fs::exists(path, ec)) return "ERROR: File not found: " + path.string();

            if (fs::is_directory(path, ec)) {
                std::vector<std::string> entries;
                for (const auto& e : fs::directory_iterator(path, ec)) {
                    std::string name = e.path().filename().string();
                    if (e.is_directory(ec)) name += "/";
                    entries.push_back(name);
                }
                std::sort(entries.begin(), entries.end());
                std::ostringstream out;
                for (const auto& name : entries) out << name << "\n";
                return out.str();
            }

            std::string content;
            if (!read_file(path, content)) return "ERROR: Failed to read " + path.string();
            auto lines = split_lines(content);

            int start = 1;
            int end = static_cast<int>(lines.size());
            if (in.contains("view_range")) {
                json range = in["view_range"];
                if (range.is_string()) {
                    try { range = json::parse(range.get<std::string>()); }
                    catch (const std::exception& e) {
                        return std::string("ERROR: 'view_range' could not be parsed: ") + e.what();
                    }
                }
                if (!range.is_array() || range.size() != 2 ||
                    !range[0].is_number_integer() || !range[1].is_number_integer()) {
                    return "ERROR: 'view_range' must be an array of two integers, e.g. [1, 50]";
                }
                start = range[0];
                end = range[1] < 0 ? static_cast<int>(lines.size()) : range[1].get<int>();
                if (start < 1 || end > static_cast<int>(lines.size()) || start > end) {
                    return "ERROR: Invalid line range";
                }
            }

            // Cap the rendered output so a single huge file can't blow the
            // context window / cost (in line with find_files and run_command).
            constexpr size_t kMaxViewBytes = 64000;
            std::ostringstream out;
            size_t emitted = 0;
            bool truncated = false;
            for (int i = start - 1; i < end && i < static_cast<int>(lines.size()); ++i) {
                std::string row = std::to_string(i + 1) + "|" + lines[i] + "\n";
                if (emitted + row.size() > kMaxViewBytes) {
                    truncated = true;
                    break;
                }
                out << row;
                emitted += row.size();
            }
            std::string result = out.str();
            if (truncated) {
                result += "... [truncated at " + std::to_string(kMaxViewBytes) +
                          " bytes; use view_range to see more]\n";
            }
            return result;
        }

        if (command == "create") {
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            if (!in.contains("file_text")) return "ERROR: 'file_text' required for create command.";
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_in_sandbox(in["path"].get<std::string>(), path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            std::error_code ec;
            if (fs::exists(path, ec)) {
                return "ERROR: File already exists: " + path.string() +
                       ". Use str_replace to modify it.";
            }
            if (!write_file(path, in["file_text"].get<std::string>())) {
                return "ERROR: Failed to write " + path.string();
            }
            return "OK";
        }

        if (command == "str_replace") {
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            if (!in.contains("old_str")) {
                return "ERROR: str_replace requires 'old_str'. To delete the matched text, pass new_str=\"\".";
            }
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_in_sandbox(in["path"].get<std::string>(), path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            std::string content;
            if (!read_file(path, content)) {
                return "ERROR: File not found: " + path.string() + ". Use the create command to create it.";
            }
            std::string old_str = in["old_str"].get<std::string>();
            std::string new_str = in.value("new_str", "");
            if (old_str.empty()) {
                return "ERROR: old_str cannot be empty. Use 'insert' to add new content.";
            }

            // Matching compares line endings as line endings throughout, not
            // just as a fallback when the byte-exact search misses. old_str is
            // composed from `view` output, which strips CR, so on a Windows file
            // a byte-exact search would miss every multi-line old_str — and,
            // just as important, two places that differ only in their endings
            // are indistinguishable to the model that asked for "the unique
            // occurrence", so both have to count towards ambiguity.
            const auto hits = find_all_loose(content, old_str);
            if (hits.empty()) return "ERROR: String not found: " + old_str;
            if (hits.size() > 1) {
                return "ERROR: Multiple occurrences found (" + std::to_string(hits.size()) +
                       "). Please provide a unique string.";
            }
            const size_t begin = hits[0].first;
            const size_t end = hits[0].second;

            // Write the replacement in whatever ending the text it replaces
            // used, so an edit inside a CRLF file — or inside the CRLF half of a
            // mixed one — doesn't leave an LF line behind it.
            const std::string style =
                eol_style(content.substr(begin, end - begin), eol_style(content, "\n"));
            content.replace(begin, end - begin, with_eol(new_str, style));
            if (!write_file(path, content)) return "ERROR: Failed to write " + path.string();
            return "OK";
        }

        if (command == "insert") {
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            if (!in.contains("insert_line") || !in.contains("new_str")) {
                return "ERROR: insert_line and new_str required.";
            }
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_in_sandbox(in["path"].get<std::string>(), path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            std::string content;
            if (!read_file(path, content)) return "ERROR: Failed to read " + path.string();

            int insert_line = in["insert_line"];
            std::string new_str = in["new_str"].get<std::string>();
            auto lines = split_lines(content);
            if (insert_line < 0 || insert_line > static_cast<int>(lines.size())) {
                return "ERROR: Invalid insert_line";
            }

            // Splice at a byte offset rather than rebuilding the file from split
            // lines: rebuilding rejoins with one ending and so rewrites every
            // line in the file, which turns a one-line insert into a whole-file
            // diff and normalises a mixed file.
            const std::string eol = eol_style(content, "\n");
            const size_t offset = line_start_offset(content, insert_line);
            const std::string piece = with_eol(new_str, eol);
            if (offset == content.size() && !content.empty() && content.back() != '\n') {
                // Appending to a file that doesn't end in a newline: start a new
                // line for the insert, and leave the file without one as before.
                content += eol + piece;
            } else {
                content.insert(offset, piece + eol);
            }
            if (!write_file(path, content)) return "ERROR: Failed to write " + path.string();
            return "Insertion successful at line " + std::to_string(insert_line);
        }

        return "ERROR: Unknown command: " + command;
    } catch (const std::exception& e) {
        return std::string("ERROR: text editor failed: ") + e.what();
    }
}

// --- file search tool -----------------------------------------------------

std::string execute_find_files(Context& /*context*/, const json& in) {
    try {
        if (!in.contains("filename")) return "ERROR: 'filename' not present.";
        std::string pattern = in["filename"].get<std::string>();
        std::string start = in.value("path", std::string("."));

        std::string query;
        bool has_query = in.contains("search_string") &&
                         in["search_string"].is_string() &&
                         !in["search_string"].get<std::string>().empty();
        if (has_query) query = in["search_string"].get<std::string>();

        fs::path base;
        {
            std::string sandbox_err;
            if (!resolve_in_sandbox(start, base, sandbox_err)) return sandbox_err;
        }
        std::error_code ec;
        if (!fs::exists(base, ec)) return "ERROR: Path not found: " + start;

        constexpr size_t kMaxFiles = 100;
        constexpr size_t kMaxFileBytes = 5 * 1024 * 1024;
        constexpr size_t kMaxLinesPerFile = 20;

        struct Match {
            std::string path;
            std::vector<std::pair<int, std::string>> lines;
        };
        std::vector<Match> results;

        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && results.size() < kMaxFiles; it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            // Never follow symlinks — they could point outside the sandbox.
            // (Recursive iteration does not descend into directory symlinks by
            // default; this also skips symlinked files for content grep.)
            if (it->is_symlink(ec)) continue;
            if (it->is_directory(ec)) {
                if (is_noise_dir(p.filename().string())) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (!wildcard_match(pattern, p.filename().string())) continue;

            Match m;
            // Report paths relative to the sandbox root so they match how the
            // model supplies paths and round-trip back into the other tools.
            // (The iterator yields absolute paths because `base` is absolute.)
            m.path = p.lexically_relative(sandbox_root()).generic_string();
            if (m.path.empty()) m.path = p.generic_string();

            if (has_query) {
                std::error_code sz_ec;
                auto size = fs::file_size(p, sz_ec);
                if (sz_ec || size > kMaxFileBytes) continue;
                std::string content;
                if (!read_file(p, content)) continue;
                if (content.find('\0') != std::string::npos) continue; // skip binary

                auto lines = split_lines(content);
                bool matched = false;
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (lines[i].find(query) != std::string::npos) {
                        matched = true;
                        if (m.lines.size() < kMaxLinesPerFile) {
                            m.lines.emplace_back(static_cast<int>(i + 1), lines[i]);
                        }
                    }
                }
                if (!matched) continue; // file name matched but content didn't
            }

            results.push_back(std::move(m));
        }

        if (results.empty()) {
            if (has_query) {
                return "No files matching '" + pattern + "' containing '" + query + "'";
            }
            return "No files matching '" + pattern + "'";
        }

        std::ostringstream out;
        out << "Found " << results.size() << " file(s):\n\n";
        for (const auto& m : results) {
            out << m.path << "\n";
            for (const auto& line : m.lines) {
                out << "  " << line.first << ": " << line.second << "\n";
            }
            if (!m.lines.empty()) out << "\n";
        }
        return out.str();
    } catch (const std::exception& e) {
        return std::string("ERROR: find_files failed: ") + e.what();
    }
}

// --- command tools (allow-listed) -----------------------------------------

// Run a command line through the OS shell, capturing stdout+stderr. The command
// itself is trusted (it came from the user's allow-list); the model only ever
// selects one by name, never supplies the command text.
std::string run_shell(const std::string& cmdline, int& exit_code) {
    std::string full = cmdline + " 2>&1";
#ifdef _WIN32
    FILE* pipe = _popen(full.c_str(), "r");
#else
    FILE* pipe = popen(full.c_str(), "r");
#endif
    if (!pipe) {
        exit_code = -1;
        return "ERROR: failed to start command";
    }

    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);

#ifdef _WIN32
    exit_code = _pclose(pipe);
#else
    int status = pclose(pipe);
    exit_code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : status;
#endif
    return out;
}

// Split a (trusted, author-written) command template into tokens, honoring
// simple double-quote grouping so a token may contain spaces.
std::vector<std::string> tokenize_template(const std::string& s) {
    std::vector<std::string> toks;
    std::string cur;
    bool in_quotes = false, have = false;
    for (char c : s) {
        if (c == '"') { in_quotes = !in_quotes; have = true; }
        else if (!in_quotes && (c == ' ' || c == '\t')) {
            if (have) { toks.push_back(cur); cur.clear(); have = false; }
        } else { cur.push_back(c); have = true; }
    }
    if (have) toks.push_back(cur);
    return toks;
}

// Parse a placeholder at s[i] (s[i] must be '%'). Recognizes %n, %*, %pn, %p*.
// Returns characters consumed (0 if not a placeholder). The 'p' type marks a
// path argument; either is_star or index (1-9) is set.
size_t parse_placeholder(const std::string& s, size_t i, bool& is_path, bool& is_star, int& index) {
    is_path = false;
    is_star = false;
    index = 0;
    size_t j = i + 1; // s[i] == '%'
    if (j >= s.size()) return 0;
    if (s[j] == 'p') { is_path = true; ++j; if (j >= s.size()) return 0; }
    if (s[j] == '*') { is_star = true; return j - i + 1; }
    if (s[j] >= '1' && s[j] <= '9') { index = s[j] - '0'; return j - i + 1; }
    return 0;
}

bool template_has_placeholder(const std::string& tpl) {
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (tpl[i] != '%') continue;
        bool p, star;
        int idx;
        if (parse_placeholder(tpl, i, p, star, idx) > 0) return true;
    }
    return false;
}

// Expand a template into an argv vector. The template's whitespace defines the
// argv boundaries; %n / %pn are replaced with model-supplied values as *literal*
// argv elements (never re-split), so no shell quoting is involved. The 'p'
// (path) type additionally requires the value to resolve inside the sandbox and
// substitutes the resolved absolute path. %* / %p* take all remaining values.
bool build_argv(const std::string& tpl, const std::vector<std::string>& args,
                std::vector<std::string>& argv, std::string& error) {
    // Highest positional index used; %* expands to the args beyond it.
    int max_idx = 0;
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (tpl[i] != '%') continue;
        bool p, star;
        int idx;
        if (parse_placeholder(tpl, i, p, star, idx) > 0 && !star) max_idx = std::max(max_idx, idx);
    }

    auto subst_path = [&](const std::string& value, std::string& out) -> bool {
        fs::path resolved;
        std::string perr;
        if (!resolve_in_sandbox(value, resolved, perr)) { error = perr; return false; }
        out = resolved.string();
        return true;
    };

    for (const std::string& tok : tokenize_template(tpl)) {
        if (tok == "%*" || tok == "%p*") {
            bool as_path = (tok == "%p*");
            for (size_t i = static_cast<size_t>(max_idx); i < args.size(); ++i) {
                if (as_path) {
                    std::string r;
                    if (!subst_path(args[i], r)) return false;
                    argv.push_back(r);
                } else {
                    argv.push_back(args[i]);
                }
            }
            continue;
        }

        std::string out;
        for (size_t i = 0; i < tok.size();) {
            if (tok[i] == '%') {
                bool is_path, is_star;
                int index;
                size_t consumed = parse_placeholder(tok, i, is_path, is_star, index);
                if (consumed > 0 && !is_star) {
                    if (static_cast<size_t>(index) > args.size()) {
                        error = "ERROR: command needs argument %" +
                                std::string(is_path ? "p" : "") + std::to_string(index) +
                                " but only " + std::to_string(args.size()) + " were provided.";
                        return false;
                    }
                    if (is_path) {
                        std::string r;
                        if (!subst_path(args[index - 1], r)) return false;
                        out += r;
                    } else {
                        out += args[index - 1];
                    }
                    i += consumed;
                    continue;
                }
            }
            out.push_back(tok[i]);
            ++i;
        }
        argv.push_back(out);
    }
    if (argv.empty()) { error = "ERROR: empty command"; return false; }
    return true;
}

std::string join_argv(const std::vector<std::string>& argv) {
    std::string s;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) s += ' '; s += argv[i]; }
    return s;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Quote one argument per the CommandLineToArgvW rules so the child receives it
// as a single, literal argv element.
std::string win_quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == '\\') { ++bs; ++i; }
        if (i == a.size()) { out.append(bs * 2, '\\'); break; }
        if (a[i] == '"') { out.append(bs * 2 + 1, '\\'); out.push_back('"'); }
        else { out.append(bs, '\\'); out.push_back(a[i]); }
    }
    out.push_back('"');
    return out;
}

// Run argv directly (no shell) and capture stdout+stderr.
// Launch one command line, capturing stdout+stderr. On success returns true and
// fills out/code; if the process couldn't be started returns false and sets err.
bool win_launch(const std::string& cmdline, std::string& out, int& code, DWORD& err) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { err = GetLastError(); return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};

    std::wstring wcmd = utf8_to_wide(cmdline);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { err = GetLastError(); CloseHandle(wr); CloseHandle(rd); return false; }
    CloseHandle(wr);

    out.clear();
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) out.append(chunk, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD c = 0;
    GetExitCodeProcess(pi.hProcess, &c);
    code = static_cast<int>(c);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

std::string exec_capture(const std::vector<std::string>& argv, int& exit_code) {
    std::string cmdline;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmdline += ' '; cmdline += win_quote_arg(argv[i]); }

    std::string out;
    DWORD err = 0;
    if (win_launch(cmdline, out, exit_code, err)) return out;

    // Batch wrappers (.cmd/.bat such as npm, npx, yarn) and shell builtins can't
    // be launched by CreateProcess directly; if the program wasn't found, retry
    // through cmd.exe, which resolves them via PATHEXT. (/s + surrounding quotes
    // makes cmd run the rest of the line verbatim.)
    if (err == ERROR_FILE_NOT_FOUND) {
        std::string viacmd = "cmd.exe /s /c \"" + cmdline + "\"";
        if (win_launch(viacmd, out, exit_code, err)) return out;
    }

    exit_code = -1;
    return "ERROR: failed to start '" + argv[0] + "' (CreateProcess error " +
           std::to_string(err) + ")";
}
#else
// Run argv directly (no shell) and capture stdout+stderr.
std::string exec_capture(const std::vector<std::string>& argv, int& exit_code) {
    int fds[2];
    if (pipe(fds) != 0) { exit_code = -1; return "ERROR: pipe failed"; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); exit_code = -1; return "ERROR: fork failed"; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> c;
        for (const auto& s : argv) c.push_back(const_cast<char*>(s.c_str()));
        c.push_back(nullptr);
        execvp(c[0], c.data());
        std::string e = "ERROR: failed to exec '" + argv[0] + "'\n";
        (void)!write(STDOUT_FILENO, e.data(), e.size());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char chunk[4096];
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0) out.append(chunk, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return out;
}
#endif

// --- built-in virtual commands --------------------------------------------
//
// A small, fixed set of read-only shell-style utilities implemented in C++ so
// they behave identically on every platform — in particular they work on pure
// Windows, where wc/head/tail/etc. are absent. They resolve paths inside the
// sandbox and never shell out. The names are reserved: run_command dispatches
// to these before consulting the user allow-list.

constexpr size_t kBuiltinMaxBytes = 64000;

// Real content lines: split on '\n' and drop the synthetic trailing empty
// segment split_lines yields when the file ends with a newline.
std::vector<std::string> content_lines(const std::string& content) {
    if (content.empty()) return {};
    auto lines = split_lines(content);
    if (content.back() == '\n' && !lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

std::string cap_output(std::string s) {
    if (s.size() > kBuiltinMaxBytes)
        s = s.substr(0, kBuiltinMaxBytes) + "\n... [output truncated]";
    return s;
}

std::string builtin_wc(const std::vector<std::string>& args) {
    bool l = false, w = false, c = false;
    std::string file;
    for (const auto& a : args) {
        if (a == "-l") l = true;
        else if (a == "-w") w = true;
        else if (a == "-c") c = true;
        else if (!a.empty() && a[0] == '-') return "ERROR: wc: unknown flag '" + a + "' (use -l, -w, -c)";
        else if (file.empty()) file = a;
        else return "ERROR: wc: only one file is supported";
    }
    if (file.empty()) return "ERROR: wc: missing file operand";
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(file, p, err)) return err;
    std::string content;
    if (!read_file(p, content)) return "ERROR: wc: cannot read " + file;

    size_t lines = 0, words = 0, bytes = content.size();
    bool in_word = false;
    for (char ch : content) {
        if (ch == '\n') ++lines;
        bool sp = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v');
        if (!sp && !in_word) { in_word = true; ++words; }
        else if (sp) in_word = false;
    }

    bool none = !l && !w && !c;
    std::ostringstream out;
    if (none || l) out << lines << " ";
    if (none || w) out << words << " ";
    if (none || c) out << bytes << " ";
    out << file;
    return out.str();
}

// Parse the leading count for head/tail from "-n N", "-nN", or "-N".
// Returns "" on success (filling n/file), or an error string.
std::string parse_head_tail(const std::vector<std::string>& args, size_t& n, std::string& file) {
    n = 10;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto to_count = [&](const std::string& digits) -> bool {
            try { long v = std::stol(digits); n = v < 0 ? 0 : static_cast<size_t>(v); return true; }
            catch (const std::exception&) { return false; }
        };
        if (a == "-n") {
            if (i + 1 >= args.size()) return "ERROR: -n requires a number";
            if (!to_count(args[++i])) return "ERROR: -n: invalid number '" + args[i] + "'";
        } else if (a.rfind("-n", 0) == 0 && a.size() > 2) {
            if (!to_count(a.substr(2))) return "ERROR: -n: invalid number in '" + a + "'";
        } else if (a.size() > 1 && a[0] == '-' && std::isdigit(static_cast<unsigned char>(a[1]))) {
            if (!to_count(a.substr(1))) return "ERROR: invalid count '" + a + "'";
        } else if (!a.empty() && a[0] == '-') {
            return "ERROR: unknown flag '" + a + "' (use -n N)";
        } else if (file.empty()) {
            file = a;
        } else {
            return "ERROR: only one file is supported";
        }
    }
    if (file.empty()) return "ERROR: missing file operand";
    return "";
}

std::string builtin_head_tail(bool head, const std::vector<std::string>& args) {
    size_t n = 10;
    std::string file;
    std::string perr = parse_head_tail(args, n, file);
    if (!perr.empty()) return perr + (head ? " (head)" : " (tail)");
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(file, p, err)) return err;
    std::string content;
    if (!read_file(p, content)) return "ERROR: cannot read " + file;

    auto lines = content_lines(content);
    std::ostringstream out;
    if (head) {
        for (size_t i = 0; i < lines.size() && i < n; ++i) out << lines[i] << "\n";
    } else {
        size_t start = lines.size() > n ? lines.size() - n : 0;
        for (size_t i = start; i < lines.size(); ++i) out << lines[i] << "\n";
    }
    return cap_output(out.str());
}

std::string builtin_cat(const std::vector<std::string>& args) {
    std::string file;
    for (const auto& a : args) {
        if (!a.empty() && a[0] == '-') return "ERROR: cat: unknown flag '" + a + "'";
        else if (file.empty()) file = a;
        else return "ERROR: cat: only one file is supported";
    }
    if (file.empty()) return "ERROR: cat: missing file operand";
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(file, p, err)) return err;
    std::error_code ec;
    if (fs::is_directory(p, ec)) return "ERROR: cat: " + file + " is a directory";
    std::string content;
    if (!read_file(p, content)) return "ERROR: cat: cannot read " + file;
    if (content.size() > kBuiltinMaxBytes)
        content = content.substr(0, kBuiltinMaxBytes) +
                  "\n... [truncated; use the editor 'view' command with view_range for more]";
    return content;
}

std::string builtin_ls(const std::vector<std::string>& args) {
    std::string path = ".";
    bool have_path = false;
    for (const auto& a : args) {
        if (!a.empty() && a[0] == '-') return "ERROR: ls: unknown flag '" + a + "'";
        else if (!have_path) { path = a; have_path = true; }
        else return "ERROR: ls: only one path is supported";
    }
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(path, p, err)) return err;
    std::error_code ec;
    if (!fs::exists(p, ec)) return "ERROR: ls: path not found: " + path;
    if (!fs::is_directory(p, ec)) return p.filename().string() + "\n"; // a plain file

    std::vector<std::string> entries;
    for (const auto& e : fs::directory_iterator(p, ec)) {
        std::string name = e.path().filename().string();
        if (e.is_directory(ec)) name += "/";
        entries.push_back(name);
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream out;
    for (const auto& name : entries) out << name << "\n";
    return cap_output(out.str());
}

// Box-drawing connectors for tree output.
constexpr const char* kTreeTee = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "; // "├── "
constexpr const char* kTreeEnd = "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "; // "└── "
constexpr const char* kTreeBar = "\xe2\x94\x82   ";                       // "│   "
constexpr const char* kTreeGap = "    ";

void tree_walk(const fs::path& dir, const std::string& prefix, int depth_left,
               size_t& count, size_t max_count, std::ostream& out, bool& truncated) {
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        entries.push_back(e);
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().filename().string() < b.path().filename().string();
              });

    for (size_t i = 0; i < entries.size(); ++i) {
        if (count >= max_count) { truncated = true; return; }
        const auto& e = entries[i];
        std::error_code dec;
        bool is_dir = e.is_directory(dec);
        std::string name = e.path().filename().string();
        bool last = (i + 1 == entries.size());
        out << prefix << (last ? kTreeEnd : kTreeTee) << name << (is_dir ? "/" : "") << "\n";
        ++count;
        if (is_dir && !is_noise_dir(name) && depth_left > 1) {
            tree_walk(e.path(), prefix + (last ? kTreeGap : kTreeBar),
                      depth_left - 1, count, max_count, out, truncated);
            if (truncated) return;
        }
    }
}

std::string builtin_tree(const std::vector<std::string>& args) {
    std::string path;
    bool have_path = false;
    int depth = 1000000; // effectively unlimited unless -L is given
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto to_depth = [&](const std::string& digits) -> bool {
            try { int v = std::stoi(digits); depth = v < 1 ? 1 : v; return true; }
            catch (const std::exception&) { return false; }
        };
        if (a == "-L") {
            if (i + 1 >= args.size()) return "ERROR: tree: -L requires a number";
            if (!to_depth(args[++i])) return "ERROR: tree: invalid -L value '" + args[i] + "'";
        } else if (a.rfind("-L", 0) == 0 && a.size() > 2) {
            if (!to_depth(a.substr(2))) return "ERROR: tree: invalid -L value in '" + a + "'";
        } else if (!a.empty() && a[0] == '-') {
            return "ERROR: tree: unknown flag '" + a + "' (use -L depth)";
        } else if (!have_path) {
            path = a; have_path = true;
        } else {
            return "ERROR: tree: only one path is supported";
        }
    }
    if (!have_path) path = ".";
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(path, p, err)) return err;
    std::error_code ec;
    if (!fs::exists(p, ec)) return "ERROR: tree: path not found: " + path;
    if (!fs::is_directory(p, ec)) return path + "\n";

    constexpr size_t kMaxTreeEntries = 300;
    std::ostringstream out;
    out << path << "\n";
    size_t count = 0;
    bool truncated = false;
    tree_walk(p, "", depth, count, kMaxTreeEntries, out, truncated);
    if (truncated)
        out << "... [truncated at " << kMaxTreeEntries
            << " entries; narrow with a path or -L depth]\n";
    return cap_output(out.str());
}

std::string run_builtin_command(const std::string& name, const std::vector<std::string>& args) {
    if (name == "wc")   return builtin_wc(args);
    if (name == "head") return builtin_head_tail(true, args);
    if (name == "tail") return builtin_head_tail(false, args);
    if (name == "cat")  return builtin_cat(args);
    if (name == "ls")   return builtin_ls(args);
    if (name == "tree") return builtin_tree(args);
    return "ERROR: unknown built-in command '" + name + "'";
}

std::string execute_list_commands(Context& /*context*/, const json& /*in*/) {
    std::ostringstream out;
    out << "Built-in commands (always available, cross-platform):\n"
           "- wc [-l|-w|-c] <file>: count lines/words/bytes\n"
           "- head [-n N] <file>: first N lines (default 10)\n"
           "- tail [-n N] <file>: last N lines (default 10)\n"
           "- cat <file>: print a file's contents\n"
           "- ls [path]: list a directory\n"
           "- tree [path] [-L depth]: show a directory tree\n";

    auto cmds = merged_commands();
    if (cmds.empty()) {
        out << "\nNo user commands are configured. The user can add them with: "
               "tapto-code command add <name> <command...>\n";
    } else {
        out << "\nUser commands:\n";
        for (const auto& [name, cmdline] : cmds) {
            out << "- " << name << ": " << cmdline << "\n";
        }
    }
    return out.str();
}

std::string execute_run_command(Context& /*context*/, const json& in) {
    try {
        if (!in.contains("name")) return "ERROR: 'name' not present.";
        std::string name = in["name"].get<std::string>();

        std::vector<std::string> args;
        if (in.contains("args")) {
            if (!in["args"].is_array()) return "ERROR: 'args' must be an array of strings.";
            for (const auto& a : in["args"]) {
                args.push_back(a.is_string() ? a.get<std::string>() : a.dump());
            }
        }

        // Built-in cross-platform commands are reserved names and take
        // precedence over the user allow-list, so they behave the same on
        // every OS (and work on pure Windows where wc/head/etc. are absent).
        if (is_builtin_command(name)) return run_builtin_command(name, args);

        auto cmds = merged_commands();
        auto it = cmds.find(name);
        if (it == cmds.end()) {
            std::string msg = "ERROR: Unknown command '" + name +
                              "'. Use list_commands to see what is available. Configured:";
            if (cmds.empty()) msg += " (none)";
            for (const auto& [n, c] : cmds) msg += " " + n;
            return msg;
        }
        const std::string& tpl = it->second;

        int exit_code = 0;
        std::string output;
        std::string display;
        if (template_has_placeholder(tpl)) {
            // Parameterized: expand to argv and exec directly — no shell, so the
            // model-supplied values are passed literally (no quoting needed).
            std::vector<std::string> argv;
            std::string err;
            if (!build_argv(tpl, args, argv, err)) return err;
            display = join_argv(argv);
            output = exec_capture(argv, exit_code);
        } else {
            // No placeholders: run through the shell (allows pipes/redirection).
            display = tpl;
            output = run_shell(tpl, exit_code);
        }

        constexpr size_t kMaxBytes = 16000;
        if (output.size() > kMaxBytes) output = output.substr(0, kMaxBytes) + "\n... [output truncated]";

        std::ostringstream r;
        r << "$ " << display << "\n" << output;
        if (!output.empty() && output.back() != '\n') r << "\n";
        r << "[exit code: " << exit_code << "]";
        return r.str();
    } catch (const std::exception& e) {
        return std::string("ERROR: run_command failed: ") + e.what();
    }
}

} // namespace

bool is_builtin_command(const std::string& name) {
    return name == "wc" || name == "head" || name == "tail" ||
           name == "cat" || name == "ls" || name == "tree";
}

std::vector<ToolSpec> builtin_tools() {
    std::vector<ToolSpec> tools;

    // str_replace text editor. Declared as Claude's built-in for Anthropic,
    // and with an explicit schema for OpenAI/Gemini.
    ToolSpec editor;
    editor.name = "str_replace_based_edit_tool";
    editor.description =
        "View, create, and edit files on the local filesystem. Commands:\n"
        "- view: show a file (with line numbers) or list a directory. Optional view_range [start,end].\n"
        "- create: create a new file with file_text (fails if it already exists).\n"
        "- str_replace: replace the unique occurrence of old_str with new_str.\n"
        "- insert: insert new_str after line insert_line (0 = beginning).";
    editor.claude_builtin_type = "text_editor_20250728";
    editor.parameters = {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"enum", {"view", "create", "str_replace", "insert"}},
                {"description", "The edit command to run."}
            }},
            {"path", {{"type", "string"}, {"description", "File or directory path."}}},
            {"file_text", {{"type", "string"}, {"description", "Content for the create command."}}},
            {"old_str", {{"type", "string"}, {"description", "Text to replace (str_replace); must be unique."}}},
            {"new_str", {{"type", "string"}, {"description", "Replacement text (str_replace) or inserted text (insert)."}}},
            {"insert_line", {{"type", "integer"}, {"description", "Line number to insert after (insert)."}}},
            {"view_range", {
                {"type", "array"},
                {"items", {{"type", "integer"}}},
                {"description", "Optional [start, end] line range for view (1-based; end -1 = EOF)."}
            }},
        }},
        {"required", {"command", "path"}},
    };
    editor.executor = execute_text_editor;
    tools.push_back(std::move(editor));

    // File search (find + optional content grep).
    ToolSpec find;
    find.name = "find_files";
    find.description =
        "Find files under a directory by filename pattern, optionally grepping their contents. "
        "Supports wildcards: * (any sequence) and ? (single character).";
    find.parameters = {
        {"type", "object"},
        {"properties", {
            {"filename", {{"type", "string"}, {"description", "Filename pattern, e.g. '*.cpp', 'test?.txt'."}}},
            {"path", {{"type", "string"}, {"description", "Starting directory. Defaults to '.'."}}},
            {"search_string", {{"type", "string"}, {"description", "Optional text to search for inside matching files."}}},
        }},
        {"required", {"filename"}},
    };
    find.executor = execute_find_files;
    tools.push_back(std::move(find));

    // list_commands: lets the model discover the allow-listed commands.
    ToolSpec list_cmds;
    list_cmds.name = "list_commands";
    list_cmds.description =
        "List the commands available to run via run_command (their names and the "
        "underlying command lines). Only these pre-approved commands can be run.";
    list_cmds.parameters = {{"type", "object"}, {"properties", json::object()}};
    list_cmds.executor = execute_list_commands;
    tools.push_back(std::move(list_cmds));

    // run_command: runs one allow-listed command by name. The model cannot
    // supply arbitrary shell text — only choose a configured command.
    ToolSpec run;
    run.name = "run_command";
    run.description =
        "Run a command by name. Two kinds are available (call list_commands to see "
        "them): (1) built-in cross-platform utilities that always work, including on "
        "Windows: wc [-l|-w|-c] <file>, head [-n N] <file>, tail [-n N] <file>, "
        "cat <file>, ls [path], tree [path] [-L depth]; and (2) the project's "
        "pre-approved commands configured via 'tapto-code command add'. Arbitrary "
        "shell commands are NOT allowed. Pass a command's arguments via 'args' (in "
        "order). For configured commands with %1, %2, ... placeholders, args fill "
        "them (%* receives all remaining values); a %p1-style placeholder is a path "
        "argument that must stay inside the working directory. Returns the command's "
        "output.";
    run.parameters = {
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}, {"description", "Name of the built-in or configured command to run."}}},
            {"args", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Arguments for the command (built-in flags/paths, or values for %1, %2, ... placeholders), in order."}
            }},
        }},
        {"required", {"name"}},
    };
    run.executor = execute_run_command;
    tools.push_back(std::move(run));

    return tools;
}

} // namespace tapto
