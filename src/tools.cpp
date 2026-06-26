#include "minicode/tools.hpp"
#include "minicode/commands.hpp"

#include "ai/context.h"

#include <algorithm>
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

namespace minicode {

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

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
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
           name == ".minicode" || name == ".vs" || name == ".vscode";
}

// --- path sandbox ---------------------------------------------------------

// The directory mini-code was started in (canonicalized). All model-driven file
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
                "mini-code can only access the folder it was started in and its "
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

            std::ostringstream out;
            for (int i = start - 1; i < end && i < static_cast<int>(lines.size()); ++i) {
                out << (i + 1) << "|" << lines[i] << "\n";
            }
            return out.str();
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

            size_t pos = content.find(old_str);
            if (pos == std::string::npos) return "ERROR: String not found: " + old_str;

            size_t count = 0, scan = 0;
            while ((scan = content.find(old_str, scan)) != std::string::npos) {
                ++count;
                scan += old_str.length();
            }
            if (count > 1) {
                return "ERROR: Multiple occurrences found (" + std::to_string(count) +
                       "). Please provide a unique string.";
            }

            content.replace(pos, old_str.length(), new_str);
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
            lines.insert(lines.begin() + insert_line, new_str);
            if (!write_file(path, join_lines(lines))) return "ERROR: Failed to write " + path.string();
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
            m.path = p.generic_string();

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

bool template_has_placeholder(const std::string& tpl) {
    for (size_t i = 0; i + 1 < tpl.size(); ++i) {
        if (tpl[i] == '%' && (tpl[i + 1] == '*' || (tpl[i + 1] >= '1' && tpl[i + 1] <= '9'))) {
            return true;
        }
    }
    return false;
}

// Expand a template into an argv vector. The template's whitespace defines the
// argv boundaries; %1..%9 and %* are replaced with model-supplied values as
// *literal* argv elements (never re-split), so no shell quoting is involved.
bool build_argv(const std::string& tpl, const std::vector<std::string>& args,
                std::vector<std::string>& argv, std::string& error) {
    // Highest positional placeholder used; %* expands to the args beyond it.
    int max_idx = 0;
    for (size_t i = 0; i + 1 < tpl.size(); ++i) {
        if (tpl[i] == '%' && tpl[i + 1] >= '1' && tpl[i + 1] <= '9') {
            max_idx = std::max(max_idx, tpl[i + 1] - '0');
        }
    }

    for (const std::string& tok : tokenize_template(tpl)) {
        if (tok == "%*") {
            for (size_t i = static_cast<size_t>(max_idx); i < args.size(); ++i) argv.push_back(args[i]);
            continue;
        }
        std::string out;
        for (size_t i = 0; i < tok.size(); ++i) {
            if (tok[i] == '%' && i + 1 < tok.size() && tok[i + 1] >= '1' && tok[i + 1] <= '9') {
                size_t idx = static_cast<size_t>(tok[i + 1] - '0');
                if (idx > args.size()) {
                    error = "ERROR: command needs argument %" + std::to_string(idx) +
                            " but only " + std::to_string(args.size()) + " were provided.";
                    return false;
                }
                out += args[idx - 1];
                ++i; // skip the digit
            } else {
                out.push_back(tok[i]);
            }
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
std::string exec_capture(const std::vector<std::string>& argv, int& exit_code) {
    std::string cmdline;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmdline += ' '; cmdline += win_quote_arg(argv[i]); }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { exit_code = -1; return "ERROR: CreatePipe failed"; }
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
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        exit_code = -1;
        return "ERROR: failed to start '" + argv[0] + "' (CreateProcess error " +
               std::to_string(GetLastError()) + ")";
    }

    std::string out;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) out.append(chunk, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
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

std::string execute_list_commands(Context& /*context*/, const json& /*in*/) {
    auto cmds = merged_commands();
    if (cmds.empty()) {
        return "No commands are configured. The user can add them with: "
               "mini-code command add <name> <command...>";
    }
    std::ostringstream out;
    out << "Available commands:\n";
    for (const auto& [name, cmdline] : cmds) {
        out << "- " << name << ": " << cmdline << "\n";
    }
    return out.str();
}

std::string execute_run_command(Context& /*context*/, const json& in) {
    try {
        if (!in.contains("name")) return "ERROR: 'name' not present.";
        std::string name = in["name"].get<std::string>();

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

        std::vector<std::string> args;
        if (in.contains("args")) {
            if (!in["args"].is_array()) return "ERROR: 'args' must be an array of strings.";
            for (const auto& a : in["args"]) {
                args.push_back(a.is_string() ? a.get<std::string>() : a.dump());
            }
        }

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
        "Run one of the project's pre-approved commands by name. Arbitrary shell "
        "commands are NOT allowed; only commands configured via "
        "'mini-code command add' can be run. Call list_commands first to see what "
        "is available, including any %1, %2, ... placeholders a command takes. "
        "Provide values for those placeholders via 'args' (in order; %* receives "
        "all remaining values). Returns the command's combined output and exit code.";
    run.parameters = {
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}, {"description", "Name of the configured command to run."}}},
            {"args", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Values for the command's %1, %2, ... placeholders, in order."}
            }},
        }},
        {"required", {"name"}},
    };
    run.executor = execute_run_command;
    tools.push_back(std::move(run));

    return tools;
}

} // namespace minicode
