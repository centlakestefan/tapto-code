// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/tools.h"
#include "tapto/commands.h"

#include "tapto/context.h"
#include "tapto/policy.h"

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

// The small helpers this file used to carry -- split_lines, read_file,
// wildcard_match, is_noise_dir -- come from tapto/fstools.h now, so the glob
// and the noise-directory list agree with the folder tools.

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

bool write_file(const fs::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return out.good();
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

// The folders granted with /add-folder, or null. Set by main through
// set_granted_folders(); read on every path resolution, so a grant made
// mid-session is seen at once.
const FolderSet* g_folders = nullptr;

// True if `path` is `root` or under it, both already canonical.
bool is_under(const fs::path& root, const fs::path& path) {
    const fs::path rel = path.lexically_relative(root);
    return !rel.empty() && *rel.begin() != fs::path("..");
}

// The granted folder that owns `resolved`, or null. `writable_only` narrows
// it to the ones the user marked rw.
const Folder* granted_owner(const fs::path& resolved, bool writable_only) {
    if (!g_folders) return nullptr;
    for (const auto& f : g_folders->folders()) {
        if ((f.writable || !writable_only) && is_under(f.root, resolved)) return &f;
    }
    return nullptr;
}

// What a path may reach beyond the working directory, which every scope has.
//
//   Read        every granted folder, read-only ones included: a read is a
//               read whichever spelling the model picks (the editor's view,
//               find_files, the built-in cat/ls/head/...).
//   Write       the granted folders marked rw: the editor's create and edit,
//               and the cwd and %p paths of an allow-listed shell command --
//               letting the model edit a folder and letting it run the
//               project's own build there is the same trust.
//   WorkingDir  the working directory alone.
enum class Scope { Read, Write, WorkingDir };

// Resolve `input` (absolute, or relative to `base`) and confirm it stays
// within the sandbox. `base` (default: the sandbox root itself) is the
// directory relative paths are resolved against — e.g. a `cwd` supplied to a
// command — so a caller can target a subfolder without ever escaping the root:
// the final check below still pins the result to the sandbox root.
// weakly_canonical normalizes ".."/"." and resolves symlinks in the existing
// prefix, so attempts to escape via those are caught. On success fills `out`
// with the resolved absolute path and returns true; otherwise sets `error` and
// returns false.
//
// A granted folder is reached either as "<label>/<rest>" or as an absolute
// path under its root. The label form is taken only when resolving from the
// working directory itself and it has no entry of that name, so a project's
// own subfolder always wins over a grant that happens to share its name; the
// absolute form is always unambiguous.
bool resolve_in_sandbox(const std::string& input, fs::path& out, std::string& error,
                        const fs::path& base = fs::path(), Scope scope = Scope::Read) {
    const fs::path& root = sandbox_root();
    const fs::path anchor = base.empty() ? root : base;
    const bool grants = scope != Scope::WorkingDir && g_folders != nullptr;
    const bool writes = scope == Scope::Write;
    std::error_code ec;
    fs::path in_path(input);

    auto refuse_read_only = [&](const Folder& f) {
        error = "ERROR: '" + input + "' is in '" + f.label + "', which the user granted "
                "read-only. Reading it is fine (read_file, view, cat); to change it or run "
                "a command there, ask the user to grant the folder read-write with "
                "/add-folder <path> rw.";
        return false;
    };

    if (grants && !in_path.is_absolute() && !in_path.empty() && anchor == root) {
        const std::string head = in_path.begin()->string();
        const Folder* f = g_folders->get(head);
        if (f && !fs::exists(anchor / head, ec)) {
            if (writes && !f->writable) return refuse_read_only(*f);
            fs::path rest;
            for (auto it = std::next(in_path.begin()); it != in_path.end(); ++it) rest /= *it;
            fs::path abs = f->root / rest;
            fs::path resolved = fs::weakly_canonical(abs, ec);
            if (ec) resolved = abs.lexically_normal();
            if (is_under(f->root, resolved)) {
                out = resolved;
                return true;
            }
        }
    }

    fs::path abs = in_path.is_absolute() ? in_path : (anchor / in_path);
    fs::path resolved = fs::weakly_canonical(abs, ec);
    if (ec) resolved = abs.lexically_normal();

    if (is_under(root, resolved)) {
        out = resolved;
        return true;
    }
    if (grants) {
        if (const Folder* f = granted_owner(resolved, /*writable_only=*/false)) {
            if (writes && !f->writable) return refuse_read_only(*f);
            out = resolved;
            return true;
        }
    }
    error = "ERROR: '" + input + "' is outside the working directory. "
            "tapto-code can only access the folder it was started in and its "
            "subdirectories" +
            std::string(grants && !g_folders->empty()
                            ? (writes ? ", plus any folder the user has granted read-write."
                                      : ", plus any folder the user has granted.")
                            : ".");
    return false;
}

// True if `resolved` (already inside the sandbox) is a repository's .git
// directory or anything under it. The model has no reason to write there, and
// a writable .git turns every allow-listed git command into code execution:
// .git/config can name a core.fsmonitor or core.hooksPath command that even
// `git status` runs, and .git/hooks/* run on commit. Checked against whichever
// root the path landed in, so a writable grant is covered too.
bool in_git_dir(const fs::path& resolved) {
    const Folder* owner = granted_owner(resolved, /*writable_only=*/true);
    const fs::path& root = owner ? owner->root : sandbox_root();
    for (const auto& part : resolved.lexically_relative(root)) {
        if (part == ".git") return true;
    }
    return false;
}

// Resolve a path the model wants to write to: sandboxed, and never under .git.
bool resolve_for_write(const std::string& input, fs::path& out, std::string& error) {
    if (!resolve_in_sandbox(input, out, error, fs::path(), Scope::Write)) return false;
    if (in_git_dir(out)) {
        error = "ERROR: '" + input + "' is inside .git, which tapto-code never "
                "modifies. Use an allow-listed git command instead.";
        return false;
    }
    return true;
}

// --- text editor tool -----------------------------------------------------

std::string execute_text_editor(Context& /*context*/, const json& in) {
    try {
        if (!in.contains("command")) return "ERROR: 'command' not present.";
        std::string command = in["command"];

        // file_text is the content field for create/write only. The two genuinely
        // misleading cases are str_replace and insert, where a stray file_text
        // would otherwise be silently ignored (str_replace would delete old_str
        // instead of writing the intended text). Fail loudly there, naming the
        // field to use, so the model self-corrects on the first retry.
        //
        // view and delete simply ignore file_text: it is irrelevant to them, and
        // rejecting it only trips the model up — it habitually emits
        // file_text: "" on view calls, sees the error, doesn't realise it's the
        // field it's adding, and loops (see tapto.log, the config.c session).
        if (in.contains("file_text") &&
            (command == "str_replace" || command == "insert")) {
            return "ERROR: 'file_text' is only for the 'create' and 'write' commands. "
                   "For str_replace or insert, pass the text as 'new_str' "
                   "(insert also takes 'insert_line').";
        }

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
                if (!resolve_for_write(in["path"].get<std::string>(), path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            std::error_code ec;
            if (fs::exists(path, ec)) {
                return "ERROR: File already exists: " + path.string() +
                       ". Use str_replace to edit part of it, or the 'write' command "
                       "to replace the whole file.";
            }
            if (!write_file(path, in["file_text"].get<std::string>())) {
                return "ERROR: Failed to write " + path.string();
            }
            return "OK";
        }

        if (command == "write") {
            // Full overwrite, for the "rewrite this whole file" intent that
            // str_replace (needs a unique old_str) and create (refuses existing
            // files) can't express. file_text is the entire new content.
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            if (!in.contains("file_text"))
                return "ERROR: 'file_text' required for the write command.";
            const std::string input = in["path"].get<std::string>();
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_for_write(input, path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            // The mirror of create's "already exists": write is for a file
            // that is there. Letting it create one would turn a mistyped path
            // into a quiet new file beside the one the model meant to rewrite.
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                return "ERROR: Not found: " + path.string() +
                       ". write replaces an existing file; use 'create' for a new one.";
            }
            if (fs::is_directory(path, ec)) {
                return "ERROR: '" + path.string() + "' is a directory.";
            }
            if (!write_file(path, in["file_text"].get<std::string>())) {
                return "ERROR: Failed to write " + path.string();
            }
            // Echo the path as the model gave it, as the other commands' errors
            // do not: the resolved one is absolute and machine-specific.
            return "OK: wrote " + input;
        }

        if (command == "delete") {
            // Remove a single file. The sandbox, the .git guard, and
            // read-only-grant refusal all come from resolve_for_write, so a
            // delete is exactly as reachable (and as protected) as a write.
            // Directories are refused: there is no recursive delete, so a
            // mis-targeted path can never take a tree (or a repo) with it.
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            const std::string input = in["path"].get<std::string>();
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_for_write(input, path, sandbox_err)) {
                    return sandbox_err;
                }
            }
            std::error_code ec;
            if (!fs::exists(path, ec))
                return "ERROR: Not found: " + path.string() + " (nothing to delete).";
            if (fs::is_directory(path, ec))
                return "ERROR: '" + path.string() + "' is a directory. "
                       "delete removes a single file only; there is no recursive delete. "
                       "Delete the files inside it one at a time instead.";
            if (!fs::remove(path, ec))
                return "ERROR: Failed to delete " + path.string() +
                       (ec ? (": " + ec.message()) : std::string());
            return "OK: deleted " + input;
        }

        if (command == "str_replace") {
            if (!in.contains("path")) return "ERROR: 'path' not present.";
            if (!in.contains("old_str")) {
                return "ERROR: str_replace requires 'old_str'. To delete the matched text, pass new_str=\"\".";
            }
            // To delete matched text the model must say so with an explicit
            // new_str:"" — a missing new_str is how a whole-file write sent as
            // file_text used to silently become a deletion of old_str.
            if (!in.contains("new_str")) {
                return "ERROR: str_replace requires 'new_str' (the replacement text). "
                       "Pass new_str=\"\" to delete the matched text. If you meant to "
                       "write a whole file, use the 'write' command with 'file_text'.";
            }
            fs::path path;
            {
                std::string sandbox_err;
                if (!resolve_for_write(in["path"].get<std::string>(), path, sandbox_err)) {
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
                if (!resolve_for_write(in["path"].get<std::string>(), path, sandbox_err)) {
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
// Content search goes through find_matching_lines (tapto/fstools.h), which
// streams the file so it can reach files far larger than the context window.

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
            // A hit inside a writable granted folder is shown as
            // "<label>/<rest>", which the editor accepts back.
            if (granted_owner(p, /*writable_only=*/false) && !is_under(sandbox_root(), p)) {
                m.path = g_folders->display(p);
            } else {
                m.path = p.lexically_relative(sandbox_root()).generic_string();
            }
            if (m.path.empty()) m.path = p.generic_string();

            if (has_query) {
                // Stream the file line-by-line (find_matching_lines) rather than
                // loading it whole: a content search must reach files far larger
                // than the context window (the old 5 MiB cap made find_files
                // report "no files" on any big log). The probe inside bails out
                // on binaries, and we keep at most kMaxLinesPerFile hits per file.
                auto lines = find_matching_lines(p, query, kMaxLinesPerFile);
                if (lines.empty()) continue;   // file name matched, content didn't
                for (const auto& lm : lines) m.lines.emplace_back(lm.line, std::move(lm.text));
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
// selects one by name, never supplies the command text. `cwd` (always inside the
// sandbox) is the directory the command runs in: because popen offers no way to
// name a working directory, we prepend a `cd` with platform-safe quoting.
std::string run_shell(const std::string& cmdline, const fs::path& cwd, int& exit_code) {
    const std::string dir = cwd.string();
    std::string full;
#ifdef _WIN32
    if (dir.find_first_of("\"%") != std::string::npos) {
        exit_code = -1;
        return "ERROR: the working directory '" + dir +
               "' contains a character (\" or %) cmd.exe would interpret. "
               "Use a plain directory name.";
    }
    full = "cd /d \"" + dir + "\" && " + cmdline + " 2>&1";
#else
    std::string cd = "cd '";
    for (char c : dir) {
        if (c == '\'') cd += "'\\''"; // close, escaped quote, reopen
        else cd += c;
    }
    cd += "'";
    full = cd + " && " + cmdline + " 2>&1";
#endif
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
    bool has_star = false;
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (tpl[i] != '%') continue;
        bool p, star;
        int idx;
        if (parse_placeholder(tpl, i, p, star, idx) > 0) {
            if (!star) max_idx = std::max(max_idx, idx);
            else has_star = true;
        }
    }

    // A template with no %* / %p* has a fixed number of slots; more supplied
    // values than slots is a model error (e.g. git-commit given ["-m","text"]
    // would stuff "-m" into %1 and silently drop "text"), so refuse it rather
    // than silently consume the wrong values.
    if (!has_star && static_cast<int>(args.size()) > max_idx) {
        error = "ERROR: command takes " + std::to_string(max_idx) +
                " argument" + (max_idx == 1 ? "" : "s") +
                " but " + std::to_string(args.size()) + " were provided. "
                "Extra values would be silently ignored, so the call was rejected.";
        return false;
    }

    auto subst_path = [&](const std::string& value, std::string& out) -> bool {
        fs::path resolved;
        std::string perr;
        if (!resolve_in_sandbox(value, resolved, perr, fs::path(), Scope::Write)) { error = perr; return false; }
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
bool win_launch(const std::string& cmdline, const std::wstring& cwd,
                std::string& out, int& code, DWORD& err) {
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
                             CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
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

std::string exec_capture(const std::vector<std::string>& argv, const fs::path& cwd, int& exit_code) {
    std::string cmdline;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmdline += ' '; cmdline += win_quote_arg(argv[i]); }

    std::wstring wcwd = utf8_to_wide(cwd.string());
    std::string out;
    DWORD err = 0;
    if (win_launch(cmdline, wcwd, out, exit_code, err)) return out;

    // Batch wrappers (.cmd/.bat such as npm, npx, yarn) and shell builtins can't
    // be launched by CreateProcess directly; if the program wasn't found, retry
    // through cmd.exe, which resolves them via PATHEXT. (/s + surrounding quotes
    // makes cmd run the rest of the line verbatim.)
    //
    // That line was quoted for CommandLineToArgvW, which cmd.exe does not speak:
    // it has no \" escape, so a quote inside a value toggles its quoting state
    // and whatever follows — `& del ...` — becomes live shell syntax, and %VAR%
    // is expanded even inside quotes. No quoting makes arbitrary text safe for
    // cmd.exe, so the only honest answer is to refuse values that contain its
    // metacharacters rather than silently hand the model a shell.
    if (err == ERROR_FILE_NOT_FOUND) {
        for (const std::string& a : argv) {
            if (a.find_first_of("\"&|<>^%\r\n") != std::string::npos) {
                exit_code = -1;
                return "ERROR: '" + argv[0] + "' is a batch wrapper that must run via "
                       "cmd.exe, and the argument '" + a + "' contains a character "
                       "(one of \" & | < > ^ % or a newline) that cmd.exe would "
                       "interpret. Use a value without those characters.";
            }
        }
        std::string viacmd = "cmd.exe /s /c \"" + cmdline + "\"";
        if (win_launch(viacmd, wcwd, out, exit_code, err)) return out;
    }

    exit_code = -1;
    return "ERROR: failed to start '" + argv[0] + "' (CreateProcess error " +
           std::to_string(err) + ")";
}
#else
// Run argv directly (no shell) and capture stdout+stderr. The child chdir's to
// `cwd` (inside the sandbox) before exec, so the parent's working directory is
// never touched.
std::string exec_capture(const std::vector<std::string>& argv, const fs::path& cwd, int& exit_code) {
    int fds[2];
    if (pipe(fds) != 0) { exit_code = -1; return "ERROR: pipe failed"; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); exit_code = -1; return "ERROR: fork failed"; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        const std::string dir = cwd.string();
        if (chdir(dir.empty() ? "." : dir.c_str()) != 0) {
            std::string e = "ERROR: failed to change to working directory '" + dir + "'\n";
            (void)!write(STDOUT_FILENO, e.data(), e.size());
            _exit(127);
        }
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

std::string builtin_wc(const std::vector<std::string>& args, const fs::path& base) {
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
    if (!resolve_in_sandbox(file, p, err, base)) return err;
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

std::string builtin_head_tail(bool head, const std::vector<std::string>& args, const fs::path& base) {
    size_t n = 10;
    std::string file;
    std::string perr = parse_head_tail(args, n, file);
    if (!perr.empty()) return perr + (head ? " (head)" : " (tail)");
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(file, p, err, base)) return err;
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

std::string builtin_cat(const std::vector<std::string>& args, const fs::path& base) {
    std::string file;
    for (const auto& a : args) {
        if (!a.empty() && a[0] == '-') return "ERROR: cat: unknown flag '" + a + "'";
        else if (file.empty()) file = a;
        else return "ERROR: cat: only one file is supported";
    }
    if (file.empty()) return "ERROR: cat: missing file operand";
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(file, p, err, base)) return err;
    std::error_code ec;
    if (fs::is_directory(p, ec)) return "ERROR: cat: " + file + " is a directory";
    std::string content;
    if (!read_file(p, content)) return "ERROR: cat: cannot read " + file;
    if (content.size() > kBuiltinMaxBytes)
        content = content.substr(0, kBuiltinMaxBytes) +
                  "\n... [truncated; use the editor 'view' command with view_range for more]";
    return content;
}

std::string builtin_ls(const std::vector<std::string>& args, const fs::path& base) {
    std::string path = ".";
    bool have_path = false;
    for (const auto& a : args) {
        if (!a.empty() && a[0] == '-') return "ERROR: ls: unknown flag '" + a + "'";
        else if (!have_path) { path = a; have_path = true; }
        else return "ERROR: ls: only one path is supported";
    }
    fs::path p;
    std::string err;
    if (!resolve_in_sandbox(path, p, err, base)) return err;
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

std::string builtin_tree(const std::vector<std::string>& args, const fs::path& base) {
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
    if (!resolve_in_sandbox(path, p, err, base)) return err;
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

// Built-in commands take a relative-to-base path: their target is resolved
// against `base` (the command's `cwd`) and pinned to the sandbox, so `ls`/`cat`/
// `tree` honour `cwd` the same way the shell-built commands do, and can also
// take a full/relative-from-root path directly.
std::string run_builtin_command(const std::string& name, const std::vector<std::string>& args,
                                const fs::path& base) {
    if (name == "wc")   return builtin_wc(args, base);
    if (name == "head") return builtin_head_tail(true, args, base);
    if (name == "tail") return builtin_head_tail(false, args, base);
    if (name == "cat")  return builtin_cat(args, base);
    if (name == "ls")   return builtin_ls(args, base);
    if (name == "tree") return builtin_tree(args, base);
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
        out << (policy_allows_user_commands()
                    ? "\nNo user commands are configured. The user can add them with: "
                      "tapto-code command add <name> <command...>\n"
                    : "\nNo commands are allow-listed: the organization's policy defines "
                      "none and allows no user commands.\n");
    } else {
        out << "\nAllow-listed commands:\n";
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
        } else if (in.contains("path") && in["path"].is_string()) {
            // Graceful fallback: the model sometimes passes the target as a
            // top-level `path` (confusing run_command with the text editor,
            // which uses `path`). Treat it as the sole argument.
            args.push_back(in["path"].get<std::string>());
        }

        // Base directory for the command (default: the sandbox root). Resolved
        // relative to the sandbox and confined to it, so a build can be pointed
        // at any subfolder — but never out of the tree. This is shared by BOTH
        // the built-ins (they resolve their relative path against it) and the
        // shell-built commands (they run with it as their working dir), so a
        // `cwd` means the same thing in either case. What "the tree" includes
        // differs: a built-in only reads, so any granted folder will do; a
        // shell command may do anything, so only one granted read-write.
        fs::path base = sandbox_root();
        if (in.contains("cwd")) {
            if (!in["cwd"].is_string()) return "ERROR: 'cwd' must be a string.";
            const std::string raw = in["cwd"].get<std::string>();
            std::string err;
            const Scope scope = is_builtin_command(name) ? Scope::Read : Scope::Write;
            if (!resolve_in_sandbox(raw, base, err, fs::path(), scope)) return err;
            std::error_code ec;
            if (!fs::is_directory(base, ec)) {
                return "ERROR: cwd '" + raw + "' is not an existing directory. "
                       "Create it first (e.g. with an allow-listed build command or "
                       "str_replace create) before running a command in it.";
            }
        }

        // Built-in cross-platform commands are reserved names and take
        // precedence over the user allow-list, so they behave the same on
        // every OS (and work on pure Windows where wc/head/etc. are absent).
        // Their target path resolves against `base` (they also accept a
        // relative-from-root path directly).
        if (is_builtin_command(name)) return run_builtin_command(name, args, base);

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
            output = exec_capture(argv, base, exit_code);
        } else {
            // No placeholders: the command takes no arguments, so any supplied
            // ones are a model error — reject rather than silently drop them
            // (build_argv's arity check can't catch this path, as it runs
            // parameterized templates only).
            if (!args.empty()) {
                return "ERROR: command takes no arguments but " +
                       std::to_string(args.size()) + " were provided. "
                       "Extra values would be silently ignored, so the call was rejected.";
            }
            // Run through the shell (allows pipes/redirection).
            display = tpl;
            output = run_shell(tpl, base, exit_code);
        }

        constexpr size_t kMaxBytes = 16000;
        if (output.size() > kMaxBytes) output = output.substr(0, kMaxBytes) + "\n... [output truncated]";

        std::ostringstream r;
        fs::path rel = base.lexically_relative(sandbox_root());
        if (!rel.empty() && !rel.filename().empty()) {
            r << "working dir: " << rel.string() << "\n";
        }
        r << "$ " << display << "\n" << output;
        if (!output.empty() && output.back() != '\n') r << "\n";
        r << "[exit code: " << exit_code << "]";
        return r.str();
    } catch (const std::exception& e) {
        return std::string("ERROR: run_command failed: ") + e.what();
    }
}

// --- Status-line labels -----------------------------------------------------
//
// What the terminal shows while a tool runs, and what it commits to the
// transcript afterwards: "Edit src/foo.cpp" rather than the tool name and a
// JSON blob. Attached to each ToolSpec as its `display` hook; the backends
// call it through getToolDisplayName() and fall back to the raw name if it
// throws or is unset.
//
// str_replace_based_edit_tool:
//   view (file, range)        -> "View src/foo.cpp:10-50"
//   view (file)               -> "View src/foo.cpp"
//   create                    -> "Create src/foo.cpp"
//   write                     -> "Write src/foo.cpp"
//   delete                    -> "Delete src/foo.cpp"
//   str_replace               -> "Edit src/foo.cpp"
//   insert                    -> "Insert src/foo.cpp"
//   unknown sub-command       -> the path
//
// find_files:
//   with search_string        -> "Search *.cpp ~\"query\""
//   without                   -> "Find *.cpp"
//
// list_commands               -> "List commands"
//
// run_command:
//   with args                 -> "Run build arg1 arg2"
//   without args              -> "Run build"

std::string display_text_editor(const json& input) {
    if (!input.is_object()) return "str_replace_based_edit_tool";
    std::string cmd  = input.contains("command") && input["command"].is_string()
                       ? input["command"].get<std::string>() : "";
    std::string path = input.contains("path") && input["path"].is_string()
                       ? input["path"].get<std::string>() : "";

    if (cmd == "view") {
        std::string label = "View " + path;
        // view_range: show line numbers when present
        if (input.contains("view_range")) {
            const auto& vr = input["view_range"];
            // Accept both a JSON array and a string-encoded array.
            auto try_range = [&](const json& r) -> std::string {
                if (r.is_array() && r.size() == 2 &&
                    r[0].is_number_integer() && r[1].is_number_integer()) {
                    int end = r[1].get<int>();
                    return ":" + std::to_string(r[0].get<int>()) +
                           "-" + (end < 0 ? "EOF" : std::to_string(end));
                }
                return "";
            };
            if (vr.is_string()) {
                try {
                    label += try_range(json::parse(vr.get<std::string>()));
                } catch (...) {}
            } else {
                label += try_range(vr);
            }
        }
        return label;
    }
    if (cmd == "create")      return "Create " + path;
    if (cmd == "write")       return "Write "  + path;
    if (cmd == "delete")      return "Delete " + path;
    if (cmd == "str_replace") return "Edit "   + path;
    if (cmd == "insert")      return "Insert " + path;
    // Unknown sub-command: fall back to path only.
    return path.empty() ? "str_replace_based_edit_tool" : path;
}

std::string display_find_files(const json& input) {
    if (!input.is_object()) return "find_files";
    std::string pattern = input.contains("filename") && input["filename"].is_string()
                          ? input["filename"].get<std::string>() : "*";
    bool has_query = input.contains("search_string") &&
                     input["search_string"].is_string() &&
                     !input["search_string"].get<std::string>().empty();
    if (has_query)
        return "Search " + pattern + " ~\"" + input["search_string"].get<std::string>() + "\"";
    return "Find " + pattern;
}

std::string display_list_commands(const json&) {
    return "List commands";
}

std::string display_run_command(const json& input) {
    if (!input.is_object()) return "run_command";
    std::string name = input.contains("name") && input["name"].is_string()
                       ? input["name"].get<std::string>() : "?";
    std::string label = "Run " + name;
    if (input.contains("args") && input["args"].is_array()) {
        for (const auto& a : input["args"]) {
            if (a.is_string()) label += " " + a.get<std::string>();
        }
    } else if (input.contains("path") && input["path"].is_string()) {
        label += " " + input["path"].get<std::string>();
    }
    return label;
}

} // namespace

bool is_builtin_command(const std::string& name) {
    return name == "wc" || name == "head" || name == "tail" ||
           name == "cat" || name == "ls" || name == "tree";
}

void set_granted_folders(const FolderSet* folders) {
    g_folders = folders;
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
        "- write: overwrite an entire existing file with file_text (use for full rewrites).\n"
        "- delete: remove a single file (directories are not deleted).\n"
        "- str_replace: replace the unique occurrence of old_str with new_str.\n"
        "- insert: insert new_str after line insert_line (0 = beginning).\n"
        "file_text is the content field for create and write only; str_replace and "
        "insert take their text in new_str.";
    editor.claude_builtin_type = "text_editor_20250728";
    editor.parameters = {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"enum", {"view", "create", "write", "delete", "str_replace", "insert"}},
                {"description", "The edit command to run."}
            }},
            {"path", {{"type", "string"}, {"description", "File or directory path."}}},
            {"file_text", {{"type", "string"},
                           {"description", "Full content for the create and write commands. "
                                           "NOT used by str_replace or insert."}}},
            {"old_str", {{"type", "string"}, {"description", "Text to replace (str_replace); must be unique."}}},
            {"new_str", {{"type", "string"},
                         {"description", "Replacement text (str_replace) or inserted text (insert). "
                                          "Required for both; pass an empty string to delete in str_replace."}}},
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
    editor.display = display_text_editor;
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
    find.display = display_find_files;
    tools.push_back(std::move(find));

    // list_commands: lets the model discover the allow-listed commands.
    ToolSpec list_cmds;
    list_cmds.name = "list_commands";
    list_cmds.description =
        "List the commands available to run via run_command (their names and the "
        "underlying command lines). Only these pre-approved commands can be run.";
    list_cmds.parameters = {{"type", "object"}, {"properties", json::object()}};
    list_cmds.executor = execute_list_commands;
    list_cmds.display = display_list_commands;
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
        "shell commands are NOT allowed. ALL command-specific arguments (file paths, "
        "flags, values for %1/%2 placeholders) go in the 'args' array, in order — "
        "there is no separate 'path' parameter; e.g. list a directory with "
        "{\"name\":\"ls\",\"args\":[\"driver\"]} or pass a file to cat with "
        "{\"name\":\"cat\",\"args\":[\"src/main.cpp\"]}. Pass 'cwd' to run the "
        "command in a subdirectory of the working folder (e.g. to build in a "
        "subfolder); it is confined to the working directory. For the built-in "
        "commands it also sets the folder their path is resolved from, so 'ls' with "
        "args ['foo'] and cwd 'sub' lists 'sub/foo'. Returns the command's output.";
    run.parameters = {
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}, {"description", "Name of the built-in or configured command to run."}}},
            {"args", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Arguments for the command (built-in flags/paths, or values for %1, %2, ... placeholders), in order."}
            }},
            {"cwd", {
                {"type", "string"},
                {"description", "Optional directory to run the command in, relative to (or absolute within) the working directory. Must exist and stay inside the working directory. Use to build in subfolders, or as the folder a built-in command's relative path is resolved from. Defaults to the working directory."}
            }},
        }},
        {"required", {"name"}},
    };
    run.executor = execute_run_command;
    run.display = display_run_command;
    tools.push_back(std::move(run));

    return tools;
}

} // namespace tapto
