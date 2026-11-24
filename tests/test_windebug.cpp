// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// The windebug plugin (plugins/windebug) from the host's side: the commands it refuses,
// and a real dump opened and read. The dump is of a child process (this test,
// run with --sleep), written by another child (--dump), so dbghelp.dll is
// never loaded here before the plugin loads its own beside dbgeng.
//
//   test-windebug <windebug module> [<engine folder>]
//   test-windebug --sleep                (the process the dump is of)
//   test-windebug --dump <pid> <file>    (writes it)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <DbgHelp.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "tapto/plugin.h"

using json = nlohmann::json;
using scom::SRESULT;
using tapto::TaptoString;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check_true(int line, const char* what, bool cond, const std::string& context = {}) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
    if (!context.empty()) std::cout << "  got:\n" << context << "\n";
}

// Evaluates its text once: it may be a tool call.
void check_in(int line, const std::string& text, const char* part, bool want) {
    const std::string what = std::string(want ? "contains '" : "does not contain '") + part + "'";
    check_true(line, what.c_str(), (text.find(part) != std::string::npos) == want, text);
}

#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))
#define CHECK_IN(text, part) check_in(__LINE__, (text), part, true)
#define CHECK_NOT_IN(text, part) check_in(__LINE__, (text), part, false)

class TestHost : public tapto::ITaptoHost {
public:
    SRESULT SCOM_CALL QueryInterface(const scom::GUID& riid, void** ppv) override {
        if (!ppv) return scom::ERR_BAD_ARGUMENT;
        if (scom::IsEqualGUID(riid, scom::IID_IBase) || scom::IsEqualGUID(riid, tapto::IID_ITaptoHost)) {
            *ppv = static_cast<tapto::ITaptoHost*>(this);
            AddRef();
            return scom::OK;
        }
        *ppv = nullptr;
        return scom::ERR_NOINTERFACE;
    }
    uint32_t SCOM_CALL AddRef() override { return static_cast<uint32_t>(++refs); }
    uint32_t SCOM_CALL Release() override { return static_cast<uint32_t>(--refs); }
    void* SCOM_CALL MemAlloc(size_t size) override { return std::malloc(size ? size : 1); }
    void SCOM_CALL MemFree(void* p) override { std::free(p); }
    SRESULT SCOM_CALL Log(const char* line) override {
        log += line ? line : "";
        log += '\n';
        return scom::OK;
    }
    SRESULT SCOM_CALL IsCancelled() override {
        if (!in_call) return scom::ERR_BAD_CONTEXT;
        return cancelled ? scom::OK : scom::ST_FALSE;
    }
    SRESULT SCOM_CALL WriteTemp(const uint8_t*, size_t, const char*, char*&) override {
        return scom::ERR_NOT_FOUND;
    }
    SRESULT SCOM_CALL AttachImage(const uint8_t*, size_t, const char*) override {
        return scom::ERR_NOT_FOUND;
    }

    int refs = 0;
    bool in_call = false;
    bool cancelled = false;
    std::string log;
};

std::wstring self_path() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

HANDLE start_self(const std::wstring& args) {
    std::wstring line = L"\"" + self_path() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int write_dump(DWORD pid, const std::wstring& file) {
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return 2;
    using WriteFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto write = reinterpret_cast<WriteFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!write) return 3;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 4;
    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return 5;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithHandleData |
                                                 MiniDumpWithFullMemoryInfo);
    const BOOL ok = write(process, pid, out, type, nullptr, nullptr, nullptr);
    CloseHandle(out);
    CloseHandle(process);
    return ok ? 0 : 6;
}

class Plugin {
public:
    Plugin(TestHost& host, tapto::ITaptoPlugin* p) : m_host(host), m_p(p) {}

    std::string call(const std::string& tool, const json& args) {
        TaptoString out;
        m_host.in_call = true;
        const SRESULT r = m_p->CallTool(tool.c_str(), args.dump().c_str(), out.out());
        m_host.in_call = false;
        if (r != scom::OK) return "(status " + std::to_string(r) + ")";
        return out.str();
    }
    std::string command(const std::string& line) { return call("windebug_command", {{"command", line}}); }

private:
    TestHost& m_host;
    tapto::ITaptoPlugin* m_p;
};

void test_refused(Plugin& p) {
    // Each of these reaches past the dump: refused before anything runs.
    for (const char* line : {
             ".shell dir", "!!dir", "dx @$curprocess", ".load c:\\x\\evil.dll", ".loadby sos clr",
             "k; .load evil", "kb;.writemem c:\\x 0 10", "~*e .shell dir", "~*e !!dir",
             "$$>< c:\\script.txt", "$<c:\\script.txt", "as foo .shell", "r $.u0=1; dd $u0",
             "dd ${foo}", ".foreach (x {k}) {.echo x}", ".block {k}", ".dump /ma c:\\x.dmp",
             ".logopen c:\\x.txt", ".readmem c:\\x 0 10", ".opendump c:\\x.dmp", ".create notepad",
             ".attach 1234", ".sympath+ \\\\evil\\share", ".symfix+ c:\\x", ".srcpath c:\\x",
             "!analyze -c -load c:\\x.xml", "!evil.cmd", "!ext.for_each_module k", "!list -x k",
             "bp ntdll!NtCreateFile", "g", "q", "e 0 1", "j 1 '.shell'", ".extpath c:\\x",
             "k\n.shell dir",
         }) {
        const std::string r = p.command(line);
        check_true(__LINE__, line, r.rfind("ERROR: ", 0) == 0 && r.find("no dump is open") == std::string::npos, r);
    }
    // These read the dump: allowed, so with no dump open they say just that.
    for (const char* line : {
             "k", "kb 20", "kP", "~", "~*k", "~3s", "~~[1a2b]s", "~*e kb", "|", "||", "lm",
             "lmvm ntdll", "ln 0", "x ntdll!*Create*", "dt ntdll!_PEB", "dv", "dps @rsp",
             "dd @rsp L10", "u @rip", "ub @rip", "uf ntdll!NtWaitForSingleObject", "r", "? 1+2",
             "?? sizeof(int)", ".frame 1", ".f+", ".cxr", ".ecxr", ".exr -1", ".lastevent",
             ".reload /f", ".sympath", ".symfix", ".bugcheck", "!analyze -v", "!analyze -v -hang",
             "!thread", "!process 0 0", "!peb", "!teb", "!locks", "!cs -l", "!handle 0 f",
             "!ext.analyze -v", "!kdexts.pool", "vertarget", "k; lm; !peb",
         }) {
        const std::string r = p.command(line);
        check_true(__LINE__, line, r.find("no dump is open") != std::string::npos, r);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wcscmp(argv[1], L"--sleep") == 0) {
        Sleep(120 * 1000);
        return 0;
    }
    if (argc >= 4 && std::wcscmp(argv[1], L"--dump") == 0)
        return write_dump(static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10)), argv[3]);
    if (argc < 2) {
        std::cerr << "usage: test-windebug <windebug module> [<engine folder>]\n";
        return 2;
    }

    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::cerr << "cannot load the windebug module\n";
        return 2;
    }
    auto get = reinterpret_cast<tapto::GetPluginFn>(GetProcAddress(module, tapto::kGetPluginExport));
    auto can_unload = reinterpret_cast<tapto::CanUnloadNowFn>(GetProcAddress(module, tapto::kCanUnloadNowExport));
    if (!get || !can_unload) {
        std::cerr << "the module has no plugin exports\n";
        return 2;
    }

    TestHost host;
    tapto::plugin_host() = &host; // TaptoString's heap
    tapto::ITaptoPlugin* plugin = nullptr;
    CHECK_TRUE(get(&host, tapto::IID_ITaptoPlugin, reinterpret_cast<void**>(&plugin)) == scom::OK && plugin);
    if (!plugin) return 1;

    {
        TaptoString d;
        CHECK_TRUE(plugin->Describe(d.out()) == scom::OK);
        const json desc = json::parse(d.str(), nullptr, false);
        CHECK_TRUE(desc.value("name", "") == "windebug");
        std::string names;
        for (const auto& t : desc["tools"]) names += t.value("name", "") + " ";
        CHECK_IN(names, "windebug_open ");
        CHECK_IN(names, "windebug_command ");
        CHECK_IN(names, "windebug_close ");
    }

    const fs::path dir = fs::temp_directory_path() / ("tapto-test-windebug-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir / "symbols");
    {
        // A local symbol folder of nothing: the test never downloads.
        json config = {{"windebug-symbol-path", (dir / "symbols").u8string()}};
        // An engine folder to test instead of the one the plugin would find
        // (the shipped engine, System32): windebug-path.
        if (argc >= 3) config["windebug-path"] = fs::path(argv[2]).u8string();
        CHECK_TRUE(plugin->Configure(config.dump().c_str()) == scom::OK);
    }

    Plugin p(host, plugin);
    test_refused(p);
    CHECK_IN(p.call("windebug_open", json::object()), "ERROR: windebug_open needs");

    // A dump of a sleeping process.
    HANDLE sleeper = start_self(L"--sleep");
    CHECK_TRUE(sleeper != nullptr);
    if (sleeper) {
        Sleep(500);
        const fs::path dump = dir / "sleeper.dmp";
        HANDLE writer = start_self(L"--dump " + std::to_wstring(GetProcessId(sleeper)) + L" \"" + dump.wstring() + L"\"");
        DWORD code = 1;
        if (writer) {
            WaitForSingleObject(writer, 60 * 1000);
            GetExitCodeProcess(writer, &code);
            CloseHandle(writer);
        }
        TerminateProcess(sleeper, 0);
        CloseHandle(sleeper);
        CHECK_TRUE(code == 0);

        std::string r = p.call("windebug_open", {{"path", dump.u8string()}});
        CHECK_IN(r, "a user-mode dump");
        CHECK_NOT_IN(r, "ERROR");

        r = p.command("lm");
        CHECK_IN(r, "ntdll");
        r = p.command("~");
        CHECK_IN(r, "Id:");
        r = p.command("k");
        CHECK_NOT_IN(r, "ERROR");
        CHECK_IN(r, "ntdll");
        r = p.command("|| ; ? 1+2");
        CHECK_IN(r, "dump");
        CHECK_IN(r, "= 00000000`00000003");
        r = p.command(".shell dir");
        CHECK_IN(r, "ERROR");
        // An extension command: run by a full engine, and by one without the
        // extension DLLs (the shipped engine) answered in one line that says why.
        // Which one is told by that line alone: a full engine's !analyze writes
        // ERROR lines of its own, for the symbols this test does not have.
        r = p.command("!analyze -v");
        const bool full = r.find("not in this debugger engine") == std::string::npos;
        if (full) {
            CHECK_IN(r, "> !analyze -v");
            CHECK_IN(r, "Exception Analysis");
        } else {
            CHECK_IN(r, "not in this debugger engine");
            CHECK_IN(p.command("k; !ext.heap -s"), "not in this debugger engine");
            CHECK_IN(p.command("~*e !teb"), "not in this debugger engine");
        }

        // A second open replaces the first.
        r = p.call("windebug_open", {{"path", dump.u8string()}});
        CHECK_IN(r, "a user-mode dump");
        CHECK_IN(p.command("lm"), "ntdll");

        CHECK_IN(p.call("windebug_close", json::object()), "Closed");
        CHECK_IN(p.command("k"), "no dump is open");
        CHECK_IN(p.call("windebug_close", json::object()), "No dump is open");

        // Not a dump.
        const fs::path not_dump = dir / "not.dmp";
        std::ofstream(not_dump, std::ios::binary) << "hello";
        CHECK_IN(p.call("windebug_open", {{"path", not_dump.u8string()}}), "ERROR");
    }

    tapto::ITaptoPluginUnload* unload = nullptr;
    if (plugin->QueryInterface(tapto::IID_ITaptoPluginUnload, reinterpret_cast<void**>(&unload)) == scom::OK) {
        CHECK_TRUE(unload->Shutdown() == scom::OK);
        unload->Release();
    }
    plugin->Release();
    CHECK_TRUE(can_unload() == scom::OK);

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures) {
        std::cout << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "windebug: all " << g_checks << " checks passed\n";
    return 0;
}
