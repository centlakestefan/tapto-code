// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

// The windebug plugin: Windows crash dumps, user-mode and kernel, opened with the
// Windows debugger engine (dbgeng.dll, the engine of WinDbg and cdb) and
// examined with WinDbg's own commands.
//
// Loaded with /add-plugin windebug, or `plugin.windebug = ro`. Its tools:
//
//   windebug_open {path}       opens a dump (the host checks the path is one the
//                              model may read), and says what it is: user or
//                              kernel, the last event, the bug check
//   windebug_command {command} runs debugger commands on it: !analyze -v, k, lm, ...
//   windebug_close {}          closes it
//
// One dump is open at a time, and it stays open between calls.
//
// --- Which engine -------------------------------------------------------------
//
// The DLLs come from one folder, since dbgeng loads dbghelp beside itself and
// dbghelp loads symsrv (symbol downloads) beside itself, and the extension
// commands (!analyze, !process) come from winext\ and winxp\ beside them. In
// order: `windebug-path`, the Debugging Tools for Windows of a Windows Kit, the
// engine shipped in windebug-engine\ beside this plugin, and System32 last.
//
// The shipped one is Microsoft's redistributable DbgEng and SymSrv (NuGet,
// cmake/WinDebugEngine.cmake): dumps open and symbols download, but it has no
// extension DLLs, so no !analyze and few kernel commands; the Debugging
// Tools have them, and a user who wants them installs those. Windows has a
// dbgeng.dll in System32 too, but no symsrv.dll and no extensions, so it
// reads a dump without symbols downloading or !analyze.
//
// The WinDbg app is not among them: Windows refuses any other program the
// DLLs of an app's package folder (ERROR_ACCESS_DENIED). A copy of its
// amd64 folder elsewhere loads, and windebug-path can name one.
//
// The newer engines bring an extension gallery that downloads extensions
// (and updates of ext.dll, at !analyze) from Microsoft's NuGet feed, and
// loads any from a folder in the user's profile. Both are turned off, with
// the engine's telemetry: what runs is what is in the engine's folder.
//
// --- What the model may run -----------------------------------------------------
//
// A debugger command line reaches far past the dump: .shell and !! run
// programs, .load and the script commands run code, .create and .attach start
// live debugging, .dump, .writemem and .logopen write files, .readmem and
// .opendump read files the path rules never saw, and dx reaches all of these
// through the data model. So the commands are allowed by name rather than
// refused by name: those that read the dump (stacks, memory, types, modules,
// registers, symbols, and a list of extension commands), each piece of a line
// split at ';' checked on its own, and nothing that defines or expands an
// alias, which could put back a command the check never saw. The engine
// refuses shell and execution commands on its own as well
// (DEBUG_ENGOPT_DISALLOW_SHELL_COMMANDS, _DISABLE_EXECUTION_COMMANDS).
// `windebug-allow-commands` names more, for the user who wants them.
//
// --- Threads --------------------------------------------------------------------
//
// dbgeng answers on the thread that created its client, and a command can run
// for minutes (symbols downloading, !analyze on a kernel dump). So the engine
// lives on a thread of its own, and the host's thread waits for each call,
// asking the host whether the user pressed ESC; when so, it interrupts the
// engine with SetInterrupt, the one call dbgeng takes from any thread.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <DbgEng.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "tapto/plugin.h"

using json = nlohmann::json;
using scom::SRESULT;
using tapto::TaptoString;

namespace {

// What one command may put in its result, before the host's overflow takes
// what is too long for the conversation to @tmp.
constexpr std::size_t kMaxOutputChars = 2 * 1024 * 1024;

// How long an interrupted command gets to stop before the call returns
// without it.
constexpr auto kInterruptGrace = std::chrono::seconds(20);

const char* const kSymbolServer = "https://msdl.microsoft.com/download/symbols";

SRESULT hand_out(std::string_view s, char*& out) {
    TaptoString t = TaptoString::copy(s);
    if (!t) return scom::ERR_NO_MEMORY;
    out = t.detach();
    return scom::OK;
}

void log(const std::string& line) {
    if (tapto::ITaptoHost* h = tapto::plugin_host()) h->Log(("[windebug] " + line).c_str());
}

void release_host() {
    if (tapto::ITaptoHost*& h = tapto::plugin_host()) {
        h->Release();
        h = nullptr;
    }
}

bool host_cancelled() {
    tapto::ITaptoHost* h = tapto::plugin_host();
    return h && h->IsCancelled() == scom::OK;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// The engine's banner at opening a dump, without the reports the model needs
// none of: each "************* <title> ****" block (the symbol path checked,
// the extension gallery prepared) with its indented lines and its table of
// paths ("Response ...", "Deferred ..."), the gallery's ">>>>" and "---->"
// lines, and runs of blank lines. A block ends at a blank line or at the
// first line that is none of those, which is kept.
std::string tidy_banner(const std::string& text) {
    std::string out;
    bool in_block = false, blank = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t first = line.find_first_not_of(' ');
        const std::string body = first == std::string::npos ? std::string() : line.substr(first);
        if (body.rfind("*************", 0) == 0) {
            in_block = true;
            continue;
        }
        if (in_block) {
            if (body.empty()) {
                in_block = false;
                continue;
            }
            if (line[0] == ' ' || body.rfind("Response ", 0) == 0 || body.rfind("Deferred ", 0) == 0 ||
                body.rfind("OK ", 0) == 0 || body.rfind("Failed ", 0) == 0)
                continue;
            in_block = false;
        }
        if (body.rfind(">>>>>>>>", 0) == 0 || body.rfind("---->", 0) == 0 ||
            body.rfind("-- Configuring repositories", 0) == 0)
            continue;
        if (body.empty()) {
            if (blank || out.empty()) continue;
            blank = true;
        } else {
            blank = false;
        }
        out += line + "\n";
    }
    return out;
}

std::string hresult(HRESULT hr) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08lx", static_cast<unsigned long>(hr));
    std::string text = buf;
    wchar_t* msg = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0,
                       nullptr) &&
        msg) {
        std::string m = narrow(msg);
        LocalFree(msg);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
        text += " (" + m + ")";
    }
    return text;
}

bool file_exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// --- The commands the model may run -----------------------------------------------

// Dot commands that read the dump or the engine's state, or set where in the
// dump the next command looks (a frame, a thread, a context).
const std::set<std::string> kDotCommands = {
    ".frame", ".f+", ".f-", ".cxr", ".ecxr", ".exr", ".exptr", ".lastevent", ".trap",
    ".thread", ".process", ".context", ".tss", ".reload", ".time", ".bugcheck", ".formats",
    ".echo", ".printf", ".lines", ".chain", ".effmach", ".kframes", ".dumpdebug", ".imgscan",
    ".fnent", ".fnret", ".asm", ".enable_unicode", ".enable_long_status",
    ".force_radix_output", ".jdinfo", ".sympath", ".symfix",
};
// Of those, the ones allowed only bare: with an argument they would point
// the engine at files or servers the user did not choose.
const std::set<std::string> kBareOnly = {".sympath", ".symfix"};

// Extension commands that read the dump (ext, kdexts, uext and exts).
const std::set<std::string> kBangCommands = {
    "analyze", "thread", "process", "peb", "teb", "locks", "cs", "handle", "heap", "address",
    "vadump", "vprot", "vm", "pte", "pool", "poolused", "poolfind", "irp", "devobj",
    "drvobj", "devstack", "devnode", "stacks", "running", "ready", "irql", "pcr", "prcb",
    "idt", "timer", "dpcs", "object", "error", "gle", "exchain", "uniqstack", "findstack",
    "obja", "token", "sd", "sid", "cpuinfo", "sysinfo", "verifier", "dh", "lmi", "dlls",
    "chkimg", "runaway", "avrf", "htrace", "memusage", "session", "pfn", "blackboxbsd",
    "blackboxpnp", "blackboxntfs", "bugdump", "sym", "filecache", "findthreads", "zombies",
    "vad", "ca", "pcitree", "drivers", "wdfkd.wdfldr", "mapped_file", "dumpstream", "triage",
};
// The extension DLLs a module-qualified command (!ext.analyze) may name.
const std::set<std::string> kExtensionModules = {"ext", "exts", "kdexts", "uext", "ntsdexts"};

// !analyze's own switches: -c, -load and -xml among the others run commands
// or read and write files.
bool analyze_args_ok(const std::string& args) {
    static const std::set<std::string> kSwitches = {"-v", "-vv", "-f", "-hang", "-show", "-nodb"};
    std::size_t at = 0;
    while (at < args.size()) {
        const std::size_t b = args.find_first_not_of(" \t", at);
        if (b == std::string::npos) break;
        std::size_t e = args.find_first_of(" \t", b);
        if (e == std::string::npos) e = args.size();
        const std::string tok = lower(args.substr(b, e - b));
        at = e;
        if (kSwitches.count(tok)) continue;
        std::string num = tok.rfind("0x", 0) == 0 ? tok.substr(2) : tok;
        if (!num.empty() && num.size() <= 16) {
            bool hex = true;
            for (char c : num) hex = hex && is_hex(c);
            if (hex) continue;
        }
        return false;
    }
    return true;
}

class CommandCheck {
public:
    // Names the user allowed besides the built-in ones (windebug-allow-commands):
    // ".foo", "!bar" or a plain command.
    void allow(const std::set<std::string>& extra) { m_extra = extra; }

    // Empty when `line` may run; otherwise why not, for the model.
    // `extension`, when given, is set when the line runs one of the built-in
    // extension commands (!analyze, !heap ...), which an engine without the
    // extension DLLs cannot; a command the user allowed is not counted, since
    // it may come from an extension of their own.
    std::string check(const std::string& line, bool* extension = nullptr) const {
        if (line.find_first_of("\r\n", 0) != std::string::npos || line.find('\0') != std::string::npos)
            return "one line only";
        const std::string lc = lower(line);
        // Aliases put text back into a command after this check saw it:
        // ${name}, and the fixed-name aliases $u0..$u9 (set through r $.u0).
        if (lc.find("${") != std::string::npos || lc.find("$u") != std::string::npos ||
            lc.find("$.u") != std::string::npos)
            return "aliases are not available";
        std::size_t from = 0;
        while (from <= line.size()) {
            std::size_t semi = line.find(';', from);
            if (semi == std::string::npos) semi = line.size();
            const std::string piece = trim(line.substr(from, semi - from));
            if (!piece.empty()) {
                const std::string why = check_one(piece, extension);
                if (!why.empty()) return why;
            }
            from = semi + 1;
        }
        return {};
    }

private:
    bool extra(const std::string& name) const { return m_extra.count(lower(name)) != 0; }

    std::string refuse(const std::string& name) const {
        return "'" + name + "' is not one of the commands that only read the dump";
    }

    std::string check_one(const std::string& cmd, bool* extension) const {
        const char c = cmd[0];

        // Thread and process prefixes: ~ lists threads, ~3s switches, ~*k
        // runs k on each; ~*e <command> runs any command on each, so that
        // command is checked too.
        if (c == '~' || c == '|') {
            std::size_t at = 1;
            if (c == '|' && at < cmd.size() && cmd[at] == '|') ++at; // || systems
            if (at < cmd.size() && (cmd[at] == '*' || cmd[at] == '.' || cmd[at] == '#')) {
                ++at;
            } else if (c == '~' && at < cmd.size() && cmd[at] == '~') {
                // ~~[tid]
                const std::size_t close = cmd.find(']', at);
                if (close == std::string::npos || at + 1 >= cmd.size() || cmd[at + 1] != '[')
                    return "a thread written as ~~[id] needs its ]";
                at = close + 1;
            } else {
                while (at < cmd.size() && is_digit(cmd[at])) ++at;
            }
            const std::string rest = trim(cmd.substr(at));
            if (rest.empty() || rest == "s" || rest == "S") return {};
            if ((rest[0] == 'e' || rest[0] == 'E') && (rest.size() == 1 || rest[1] == ' ' || rest[1] == '\t')) {
                const std::string inner = trim(rest.substr(1));
                if (inner.empty()) return "~e needs a command";
                return check_one(inner, extension);
            }
            return check_one(rest, extension);
        }

        if (c == '?') return {}; // ? and ?? evaluate expressions

        if (c == '$') return "scripts ($<, $$<, $$><) are not available";

        if (c == '.') {
            std::size_t e = 1;
            while (e < cmd.size() && (is_alpha(cmd[e]) || is_digit(cmd[e]) || cmd[e] == '_')) ++e;
            if (e < cmd.size() && (cmd[e] == '+' || cmd[e] == '-') && e == 2) ++e; // .f+ .f-
            const std::string name = lower(cmd.substr(0, e));
            const std::string args = trim(cmd.substr(e));
            if (extra(name)) return {};
            if (!kDotCommands.count(name)) return refuse(name);
            if (kBareOnly.count(name) && !args.empty())
                return "'" + name + "' only without arguments: the symbol path is the user's (windebug-symbol-path)";
            return {};
        }

        if (c == '!') {
            if (cmd.size() > 1 && cmd[1] == '!') return "'!!' runs a program; not available";
            std::size_t e = 1;
            while (e < cmd.size() && cmd[e] != ' ' && cmd[e] != '\t') ++e;
            std::string name = lower(cmd.substr(1, e - 1));
            const std::string args = trim(cmd.substr(e));
            if (extra("!" + name)) return {};
            if (const std::size_t dot = name.find('.'); dot != std::string::npos &&
                                                        !kBangCommands.count(name)) {
                if (!kExtensionModules.count(name.substr(0, dot))) return refuse("!" + name);
                name = name.substr(dot + 1);
            }
            if (!kBangCommands.count(name)) return refuse("!" + name);
            if (name == "analyze" && !analyze_args_ok(args))
                return "!analyze takes -v, -vv, -f, -hang, -show and numbers here";
            if (extension) *extension = true;
            return {};
        }

        // A plain command: its name is the letters it starts with.
        std::size_t e = 0;
        while (e < cmd.size() && is_alpha(cmd[e])) ++e;
        if (e == 0) return refuse(cmd.substr(0, 1));
        const std::string raw = cmd.substr(0, e);
        const std::string name = lower(raw);
        if (extra(name)) return {};
        if (name == "dx") return "dx reaches past the dump (it can run any command); not available";
        if (name[0] == 'k') return {};                                 // k, kb, kv, kP, kn ...
        if (name[0] == 'd' && name.size() <= 4) return {};             // d, dd, dq, dt, dv, dps ...
        if (name == "u" || name == "ub" || name == "uf" || name == "ur" || name == "ux") return {};
        if (name.rfind("lm", 0) == 0) return {};                       // lm, lmv, lmvm ...
        if (name == "ln" || name == "x" || name == "r" || name == "s" || name == "n" ||
            name == "vertarget" || name == "version")
            return {};
        return refuse(raw);
    }

    std::set<std::string> m_extra;
};

// --- Finding the engine ---------------------------------------------------------

#if defined(_M_ARM64)
const wchar_t* const kKitsArch = L"arm64";
#else
const wchar_t* const kKitsArch = L"x64";
#endif

// Whether an engine's folder has the extension DLLs (!analyze and the rest):
// the Debugging Tools do, the shipped engine and System32's do not.
bool has_extensions(const std::wstring& folder) {
    return file_exists(folder + L"\\winext\\ext.dll") || file_exists(folder + L"\\ext.dll");
}

// Where an engine's folder falls short, for the user; empty when it has all.
std::string shortfall(const std::wstring& folder) {
    std::string missing;
    if (!file_exists(folder + L"\\symsrv.dll")) missing = "symbols do not download (no symsrv.dll)";
    if (!has_extensions(folder))
        missing += std::string(missing.empty() ? "" : ", and ") +
                   "!analyze and the other extension commands are missing (no winext\\ext.dll), so read "
                   "the dump with the built-in commands: .ecxr, .lastevent, k, dv, r, lm, u, d*";
    if (missing.empty()) return {};
    return "The debugger engine in " + narrow(folder) + " is not a full one: " + missing +
           ". Install the Debugging Tools for Windows (a feature of the Windows SDK installer), or "
           "set windebug-path to a full debugger folder, such as a copy of WinDbg's amd64 folder.\n";
}

// The Debugging Tools of the installed Windows Kit, or empty.
std::wstring kits_folder() {
    wchar_t root[MAX_PATH] = {};
    DWORD size = sizeof root;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
                     L"KitsRoot10", RRF_RT_REG_SZ, nullptr, root, &size) !=
        ERROR_SUCCESS)
        return {};
    std::wstring dir = root;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"Debuggers\\";
    dir += kKitsArch;
    return file_exists(dir + L"\\dbgeng.dll") ? dir : std::wstring();
}

// The engine shipped beside this plugin (plugins\windebug-engine), or empty.
std::wstring shipped_folder() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&shipped_folder), &self))
        return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(path, n);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir = dir.substr(0, slash) + L"\\windebug-engine";
    return file_exists(dir + L"\\dbgeng.dll") ? dir : std::wstring();
}

std::wstring system_folder() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return n ? std::wstring(dir, n) : std::wstring();
}

using DebugCreateFn = HRESULT(STDAPICALLTYPE*)(REFIID, PVOID*);

// --- The engine's output --------------------------------------------------------

class OutputSink final : public IDebugOutputCallbacksWide {
public:
    STDMETHOD(QueryInterface)(REFIID iid, PVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IDebugOutputCallbacksWide))) {
            *ppv = static_cast<IDebugOutputCallbacksWide*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Owned by the engine thread's state, never freed through these.
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(Output)(ULONG, PCWSTR text) override {
        if (!text || m_cut) return S_OK;
        const std::size_t n = wcslen(text);
        if (m_text.size() + n > kMaxOutputChars) {
            m_text.append(text, kMaxOutputChars - m_text.size());
            m_cut = true;
        } else {
            m_text.append(text, n);
        }
        return S_OK;
    }

    // What came since the last take, as UTF-8.
    std::string take() {
        std::string s = narrow(m_text);
        if (m_cut) s += "\n[The output stops here: it passed " + std::to_string(kMaxOutputChars) + " characters.]\n";
        m_text.clear();
        m_cut = false;
        return s;
    }

private:
    std::wstring m_text;
    bool m_cut = false;
};

// --- The engine thread ----------------------------------------------------------

// One thread that owns everything of dbgeng's, and runs one job at a time.
class EngineThread {
public:
    enum class Outcome { Done, Interrupted, Stuck };

    EngineThread() : m_thread([this] { loop(); }) {}
    ~EngineThread() { stop(); }

    // Runs `job` on the engine's thread and waits for it, polling the host
    // for ESC; on ESC, `interrupt` (callable from this thread) asks the
    // engine to stop, and the job gets kInterruptGrace to do so.
    Outcome run(std::function<void()> job, const std::function<void()>& interrupt) {
        auto state = std::make_shared<Job>();
        state->fn = std::move(job);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_current && !m_current->done) return Outcome::Stuck;
            m_current = state;
        }
        m_cv.notify_all();

        bool interrupted = false;
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto until_grace_ends = [&] { return std::chrono::steady_clock::now() + kInterruptGrace; };
        auto deadline = std::chrono::steady_clock::time_point::max();
        while (!state->done) {
            m_done_cv.wait_for(lock, std::chrono::milliseconds(100));
            if (state->done) break;
            if (!interrupted) {
                lock.unlock();
                const bool cancel = host_cancelled();
                if (cancel) interrupt();
                lock.lock();
                if (cancel) {
                    interrupted = true;
                    deadline = until_grace_ends();
                }
            } else if (std::chrono::steady_clock::now() > deadline) {
                return Outcome::Stuck;
            }
        }
        return interrupted ? Outcome::Interrupted : Outcome::Done;
    }

    // Whether a job that did not stop in time is still running.
    bool busy() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_current && !m_current->done;
    }

    // Runs `last` (the clean-up) when nothing is running, and ends the thread.
    void stop(std::function<void()> last = {}) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
            m_last = std::move(last);
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    struct Job {
        std::function<void()> fn;
        bool done = false;
    };

    void loop() {
        // dbgeng is COM: the thread that holds it is in an apartment.
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_cv.wait(lock, [&] { return m_stopping || (m_current && !m_current->done && !m_running); });
            if (m_current && !m_current->done && !m_running) {
                std::shared_ptr<Job> job = m_current;
                m_running = true;
                lock.unlock();
                try {
                    job->fn();
                } catch (...) {
                    // a job reports its own failures; nothing may end the thread
                }
                lock.lock();
                job->done = true;
                m_running = false;
                m_done_cv.notify_all();
                continue;
            }
            if (m_stopping) break;
        }
        std::function<void()> last = std::move(m_last);
        lock.unlock();
        if (last) {
            try {
                last();
            } catch (...) {
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    std::mutex m_mutex;
    std::condition_variable m_cv, m_done_cv;
    std::shared_ptr<Job> m_current;
    bool m_running = false;
    bool m_stopping = false;
    std::function<void()> m_last;
    std::thread m_thread; // last: started once the rest is ready
};

// --- The plugin -----------------------------------------------------------------

const char* const kCommandHelp =
    "Runs WinDbg commands on the open dump and returns their output; several may be "
    "joined with ';'. Only commands that read the dump are allowed: stacks (k, kb, kv, kP, "
    "~*k), threads and frames (~, ~Ns, .frame, .cxr, .ecxr, .exr, .trap, .thread, "
    ".process), memory and types (d*, dt, dv, dps, s, u, ub, uf), modules and symbols (lm, "
    "lmvm, ln, x, .reload), registers (r), expressions (?, ??), and extension commands "
    "such as !analyze -v, !thread, !process, !peb, !teb, !locks, !cs, !handle, !heap, "
    "!address, !vm, !pte, !pool, !irp, !devobj, !drvobj, !stacks, !running, !error, "
    "!exchain, !uniqstack, !findstack, !chkimg, !lmi, !dh. Not available: dx, scripts, "
    "aliases, .shell, .load, and anything that writes files or runs code. Long output is "
    "saved whole to a file, with its start and end shown.";

class WinDebugPlugin final : public scom::Base,
                        public tapto::ITaptoPlugin,
                        public tapto::ITaptoPluginUnload {
public:
    SCOM_IMPLEMENT_BASE_EX(WinDebugPlugin, tapto::ITaptoPlugin)
    SCOM_BEGIN_INTERFACE_MAP(WinDebugPlugin)
        SCOM_INTERFACE(tapto::ITaptoPlugin)
        SCOM_INTERFACE(tapto::ITaptoPluginUnload)
    SCOM_END_INTERFACE_MAP()

    SRESULT SCOM_CALL Describe(char*& json_out) override {
        const json d = {
            {"name", "windebug"},
            {"version", "1.0.0"},
            {"config_keys", {"windebug-path", "windebug-symbol-path", "windebug-allow-commands"}},
            {"tools", json::array({
                {
                    {"name", "windebug_open"},
                    {"description",
                     "Opens a Windows crash dump (.dmp: a user-mode minidump or full dump, "
                     "or a kernel dump such as MEMORY.DMP) in the Windows debugger engine, "
                     "closing the one open before, and says what it is: user or kernel, "
                     "the last event, the bug check. Symbols download as needed, so the "
                     "first open of a kernel dump can take a while. Then use windebug_command, "
                     "usually starting with !analyze -v."},
                    {"schema", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"},
                                      {"description", "The dump file."},
                                      {"x-tapto-path", "read"}}},
                        }},
                        {"required", {"path"}},
                    }},
                    {"writes", false},
                },
                {
                    {"name", "windebug_command"},
                    {"description", kCommandHelp},
                    {"schema", {
                        {"type", "object"},
                        {"properties", {
                            {"command", {{"type", "string"},
                                         {"description", "The command line, e.g. '!analyze -v' or '~*k'."}}},
                        }},
                        {"required", {"command"}},
                    }},
                    {"writes", false},
                },
                {
                    {"name", "windebug_close"},
                    {"description", "Closes the open dump."},
                    {"schema", {{"type", "object"}, {"properties", json::object()}}},
                    {"writes", false},
                },
            })},
        };
        return hand_out(d.dump(), json_out);
    }

    SRESULT SCOM_CALL Configure(const char* json_config) override {
        try {
            const json c = json::parse(json_config ? json_config : "{}", nullptr, false);
            const auto str = [&](const char* key) {
                return c.is_object() && c.contains(key) && c[key].is_string() ? c[key].get<std::string>()
                                                                               : std::string();
            };
            m_path_setting = trim(str("windebug-path"));
            m_symbol_setting = trim(str("windebug-symbol-path"));
            std::set<std::string> extra;
            const std::string list = str("windebug-allow-commands");
            std::size_t at = 0;
            while (at < list.size()) {
                const std::size_t b = list.find_first_not_of(" ,\t", at);
                if (b == std::string::npos) break;
                std::size_t e = list.find_first_of(" ,\t", b);
                if (e == std::string::npos) e = list.size();
                extra.insert(lower(list.substr(b, e - b)));
                at = e;
            }
            m_check.allow(extra);
        } catch (const std::exception& e) {
            log(std::string("Configure failed: ") + e.what());
            return scom::ERR_BAD_ARGUMENT;
        }
        return scom::OK;
    }

    SRESULT SCOM_CALL CallTool(const char* name, const char* json_args, char*& result_out) override {
        std::string result;
        try {
            const std::string tool = name ? name : "";
            const json args = json::parse(json_args ? json_args : "{}", nullptr, false);
            if (tool == "windebug_open") {
                result = open(args.is_object() ? args.value("path", std::string()) : std::string());
            } else if (tool == "windebug_command") {
                result = command(args.is_object() ? args.value("command", std::string()) : std::string());
            } else if (tool == "windebug_close") {
                result = close_dump();
            } else {
                result = "ERROR: the windebug plugin has no tool " + tool;
            }
        } catch (const std::exception& e) {
            result = std::string("ERROR: ") + e.what();
        }
        return hand_out(result, result_out);
    }

    SRESULT SCOM_CALL Shutdown() override {
        shutdown();
        release_host();
        return scom::OK;
    }

private:
    ~WinDebugPlugin() {
        shutdown();
        release_host();
    }

    // Everything of dbgeng's; touched on the engine thread only. The control
    // is also reached through set_interrupt()/interrupt(), for SetInterrupt,
    // which dbgeng takes from any thread.
    struct Engine {
        HMODULE module = nullptr;
        std::wstring folder;
        std::string note; // what the user should know about this engine
        bool extensions = false; // has ext.dll, so the built-in ! commands run
        IDebugClient5* client = nullptr;
        IDebugControl4* control = nullptr;
        IDebugSymbols3* symbols = nullptr;
        OutputSink sink;
        bool open = false;
        std::string dump; // the open dump's path
        bool kernel = false;

        void release() {
            if (client) {
                client->SetOutputCallbacksWide(nullptr);
                if (open) client->EndSession(DEBUG_END_PASSIVE);
            }
            if (symbols) symbols->Release();
            if (control) control->Release();
            if (client) client->Release();
            symbols = nullptr;
            control = nullptr;
            client = nullptr;
            open = false;
            dump.clear();
        }
    };

    void shutdown() {
        if (!m_thread) return;
        if (m_thread->busy()) interrupt(DEBUG_INTERRUPT_EXIT);
        m_thread->stop([this] { release_engine(); });
        m_thread.reset();
    }

    // Under the lock, so the engine thread never releases the control while
    // the host's thread is in SetInterrupt.
    void interrupt(ULONG how = DEBUG_INTERRUPT_ACTIVE) {
        std::lock_guard<std::mutex> lock(m_interrupt_mutex);
        if (m_interrupt) m_interrupt->SetInterrupt(how);
    }
    void set_interrupt(IDebugControl4* control) {
        std::lock_guard<std::mutex> lock(m_interrupt_mutex);
        m_interrupt = control;
    }
    // On the engine thread: the dump closed and the client let go.
    void release_engine() {
        set_interrupt(nullptr);
        m_engine.release();
    }

    EngineThread& thread() {
        if (!m_thread) m_thread = std::make_unique<EngineThread>();
        return *m_thread;
    }

    // Runs `job` on the engine thread; "" when it ran, otherwise the text the
    // model gets instead (or after what the job wrote, when interrupted).
    std::string run(const std::function<void()>& job) {
        if (m_thread && m_thread->busy())
            return "ERROR: the debugger is still busy with a command that did not stop when "
                   "interrupted. Try again in a while.";
        switch (thread().run(job, [this] { interrupt(); })) {
        case EngineThread::Outcome::Done: return {};
        case EngineThread::Outcome::Interrupted: return "\n[Stopped: the user pressed ESC.]";
        case EngineThread::Outcome::Stuck:
            return "\n[Stopped: the user pressed ESC, and the debugger is still finishing the "
                   "command.]";
        }
        return {};
    }

    // On the engine thread: loads dbgeng (once) and makes a client.
    std::string create_client() {
        if (!m_engine.module) {
            std::vector<std::pair<std::wstring, std::string>> places;
            if (!m_path_setting.empty()) {
                places.emplace_back(widen(m_path_setting), "windebug-path");
            } else {
                places.emplace_back(kits_folder(), "the Windows Kit's Debugging Tools");
                places.emplace_back(shipped_folder(), "the engine shipped with the plugin");
                places.emplace_back(system_folder(), "Windows (System32)");
            }
            for (const auto& [folder, what] : places) {
                if (folder.empty() || !file_exists(folder + L"\\dbgeng.dll")) continue;
                // dbghelp first, from the same folder: dbgeng takes the one
                // already loaded, and an older one fails it.
                if (HMODULE loaded = GetModuleHandleW(L"dbghelp.dll")) {
                    wchar_t have[MAX_PATH] = {};
                    GetModuleFileNameW(loaded, have, MAX_PATH);
                    const std::wstring want = folder + L"\\dbghelp.dll";
                    if (_wcsicmp(have, want.c_str()) != 0)
                        m_engine.note += "This program had already loaded " + narrow(have) +
                                         ", which the engine uses instead of its own.\n";
                } else {
                    LoadLibraryExW((folder + L"\\dbghelp.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                }
                m_engine.module = LoadLibraryExW((folder + L"\\dbgeng.dll").c_str(), nullptr,
                                                 LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!m_engine.module) continue;
                m_engine.folder = folder;
                log("engine from " + what + ": " + narrow(folder));
                m_engine.note += shortfall(folder);
                m_engine.extensions = has_extensions(folder);
                break;
            }
            if (!m_engine.module) {
                return !m_path_setting.empty()
                           ? "ERROR: no dbgeng.dll could be loaded from windebug-path (" + m_path_setting + ")"
                           : "ERROR: no Windows debugger engine (dbgeng.dll) was found. Install the "
                             "Debugging Tools for Windows, or set windebug-path to a folder with dbgeng.dll.";
            }
        }
        auto create = reinterpret_cast<DebugCreateFn>(GetProcAddress(m_engine.module, "DebugCreate"));
        if (!create) return "ERROR: " + narrow(m_engine.folder) + "\\dbgeng.dll has no DebugCreate";
        HRESULT hr = create(__uuidof(IDebugClient5), reinterpret_cast<void**>(&m_engine.client));
        if (FAILED(hr)) return "ERROR: the debugger engine did not start: " + hresult(hr);
        hr = m_engine.client->QueryInterface(__uuidof(IDebugControl4), reinterpret_cast<void**>(&m_engine.control));
        if (SUCCEEDED(hr))
            hr = m_engine.client->QueryInterface(__uuidof(IDebugSymbols3), reinterpret_cast<void**>(&m_engine.symbols));
        if (FAILED(hr)) {
            release_engine();
            return "ERROR: this dbgeng.dll is too old (" + hresult(hr) +
                   "); install the Debugging Tools for Windows";
        }
        m_engine.client->SetOutputCallbacksWide(&m_engine.sink);
        m_engine.control->AddEngineOptions(DEBUG_ENGOPT_DISALLOW_SHELL_COMMANDS |
                                           DEBUG_ENGOPT_DISABLE_EXECUTION_COMMANDS |
                                           DEBUG_ENGOPT_DISABLESQM);
        // The extension gallery off: nothing downloaded, nothing loaded from
        // the user's profile. An engine without .settings (System32's) says
        // so to no one.
        for (const wchar_t* setting : {
                 L".settings set Extensions.ExtensionRepository=\"Disable\"",
                 L".settings set Extensions.ExtensionRepositories.UserExtensions.IsEnabled=false",
                 L".settings set Extensions.AllowNugetExeUpdate=false",
                 L".settings set Extensions.AllowNugetMSCredentialProviderInstall=false",
                 L".settings set EngineInitialization.SendTelemetry=false",
             }) {
            m_engine.control->ExecuteWide(DEBUG_OUTCTL_IGNORE, setting,
                                          DEBUG_EXECUTE_NOT_LOGGED | DEBUG_EXECUTE_NO_REPEAT);
        }
        // What the model reads, without the prompts and the engine's chatter.
        m_engine.client->SetOutputMask(DEBUG_OUTPUT_NORMAL | DEBUG_OUTPUT_ERROR | DEBUG_OUTPUT_WARNING |
                                       DEBUG_OUTPUT_EXTENSION_WARNING);

        // Symbols: the user's setting, else _NT_SYMBOL_PATH (the engine reads
        // it itself), else Microsoft's symbol server through a cache of ours.
        std::wstring sympath = widen(m_symbol_setting);
        if (sympath.empty() && !GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", nullptr, 0)) {
            wchar_t local[MAX_PATH] = {};
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            if (n && n < MAX_PATH) {
                const std::wstring cache = std::wstring(local) + L"\\tapto\\symbols";
                sympath = L"srv*" + cache + L"*" + widen(kSymbolServer);
            }
        }
        if (!sympath.empty()) {
            m_engine.symbols->SetSymbolPathWide(sympath.c_str());
            m_engine.symbols->SetImagePathWide(sympath.c_str());
        }
        // No source path: a PDB's source-server commands never run.
        m_engine.symbols->SetSourcePathWide(L"");
        set_interrupt(m_engine.control);
        m_engine.sink.take(); // what setting the paths printed
        return {};
    }

    std::string open(const std::string& path) {
        if (path.empty()) return "ERROR: windebug_open needs the dump's path";
        auto out = std::make_shared<std::string>();
        const std::string stop = run([this, path, out] {
            std::string& result = *out;
            release_engine();
            const std::string error = create_client();
            if (!error.empty()) {
                result = error;
                return;
            }
            HRESULT hr = m_engine.client->OpenDumpFileWide(widen(path).c_str(), 0);
            if (SUCCEEDED(hr)) hr = m_engine.control->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
            std::string banner = tidy_banner(m_engine.sink.take());
            if (FAILED(hr)) {
                result = "ERROR: " + path + " could not be opened as a dump: " + hresult(hr) + "\n" + banner;
                release_engine();
                return;
            }
            m_engine.open = true;
            m_engine.dump = path;
            ULONG cls = 0, qualifier = 0;
            m_engine.control->GetDebuggeeType(&cls, &qualifier);
            m_engine.kernel = cls == DEBUG_CLASS_KERNEL;

            std::string summary;
            for (const char* cmd : {".lastevent", m_engine.kernel ? ".bugcheck" : ""}) {
                if (!*cmd) continue;
                m_engine.control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, widen(cmd).c_str(),
                                              DEBUG_EXECUTE_NOT_LOGGED | DEBUG_EXECUTE_NO_REPEAT);
                summary += "> " + std::string(cmd) + "\n" + m_engine.sink.take();
            }
            result = "Opened " + path + ", a " + (m_engine.kernel ? "kernel" : "user-mode") +
                     " dump. Next: windebug_command '!analyze -v'.\n" + m_engine.note + "\n" + banner +
                     "\n" + summary;
        });
        return *out + stop;
    }

    std::string command(const std::string& line) {
        const std::string cmd = trim(line);
        if (cmd.empty()) return "ERROR: windebug_command needs a command";
        bool extension = false;
        if (const std::string why = m_check.check(cmd, &extension); !why.empty())
            return "ERROR: " + why + ". " +
                   "Only commands that read the dump run here; see the tool's description. "
                   "The user can allow more with windebug-allow-commands.";
        auto out = std::make_shared<std::string>();
        const std::string stop = run([this, cmd, extension, out] {
            std::string& result = *out;
            if (!m_engine.open) {
                result = "ERROR: no dump is open; open one with windebug_open";
                return;
            }
            // Said here rather than left to the engine, whose answer ("not
            // extension gallery command", a LoadLibrary error) does not say why.
            if (extension && !m_engine.extensions) {
                result = "ERROR: the ! extension commands (!analyze, !heap, ...) are not in this "
                         "debugger engine; it has no extension DLLs. Read the dump with the built-in "
                         "commands instead: .ecxr, .lastevent, .exr -1, k, kP, dv, r, lm, u, d*. "
                         "The user gets them by installing the Debugging Tools for Windows.";
                return;
            }
            const HRESULT hr = m_engine.control->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT, widen(cmd).c_str(),
                                                             DEBUG_EXECUTE_NOT_LOGGED | DEBUG_EXECUTE_NO_REPEAT);
            std::string text = m_engine.sink.take();
            if (FAILED(hr) && text.empty()) text = "(the command failed: " + hresult(hr) + ")\n";
            if (text.empty()) text = "(no output)\n";
            result = "> " + cmd + "\n" + text;
        });
        return *out + stop;
    }

    std::string close_dump() {
        if (!m_thread) return "No dump is open.";
        auto out = std::make_shared<std::string>();
        const std::string stop = run([this, out] {
            *out = m_engine.open ? "Closed " + m_engine.dump + "." : "No dump is open.";
            release_engine();
        });
        return *out + stop;
    }

    std::string m_path_setting;
    std::string m_symbol_setting;
    CommandCheck m_check;
    Engine m_engine;
    std::mutex m_interrupt_mutex;
    IDebugControl4* m_interrupt = nullptr;
    std::unique_ptr<EngineThread> m_thread;
    tapto::PluginObjectCount m_count;
};

} // namespace

TAPTO_PLUGIN_EXPORT SRESULT SCOM_CALL TaptoGetPlugin(tapto::ITaptoHost* host,
                                                     const scom::GUID& riid, void** ppv) {
    if (!host || !ppv) return scom::ERR_BAD_ARGUMENT;
    *ppv = nullptr;
    if (!tapto::plugin_host()) {
        host->AddRef();
        tapto::plugin_host() = host;
    }
    auto* p = new WinDebugPlugin;
    const SRESULT r = p->QueryInterface(riid, ppv);
    p->Release();
    return r;
}

TAPTO_PLUGIN_EXPORT SRESULT SCOM_CALL TaptoCanUnloadNow() {
    return tapto::plugin_objects() == 0 ? scom::OK : scom::ST_FALSE;
}
