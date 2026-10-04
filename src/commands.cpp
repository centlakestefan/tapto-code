// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/commands.h"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include "tapto/config.h"
#include "tapto/policy.h"

namespace tapto {

std::map<std::string, std::string> merged_commands() {
    std::map<std::string, std::string> out;
    for (const auto& e : effective_commands()) out[e.name] = e.command;
    return out;
}

std::vector<CommandEntry> effective_commands() {
    std::vector<CommandEntry> merged;
    auto apply = [&](const std::vector<Config::Entry>& entries, Level lvl) {
        for (const auto& e : entries) {
            bool found = false;
            for (auto& existing : merged) {
                if (existing.name == e.first) {
                    existing.command = e.second;
                    existing.origin = lvl;
                    found = true;
                    break;
                }
            }
            if (!found) merged.push_back({e.first, e.second, lvl});
        }
    };
    // The user's stores count only while policy lets them. Either way the
    // organization's commands come last and win, so a name it defines cannot
    // be redefined below it.
    if (policy_allows_user_commands()) {
        for (Level lvl : {Level::System, Level::Global, Level::Local}) {
            apply(Config::load(commands_path(lvl)).entries(), lvl);
        }
    }
    apply(policy_commands(), Level::Policy);
    return merged;
}

std::vector<CommandEntry> commands_in_scope(Level level) {
    std::vector<CommandEntry> out;
    const std::vector<Config::Entry> entries =
        (level == Level::Policy) ? policy_commands() : Config::load(commands_path(level)).entries();
    for (const auto& e : entries) {
        out.push_back({e.first, e.second, level});
    }
    return out;
}

std::string command_policy_refusal(const std::string& name) {
    if (!policy_allows_user_commands()) {
        return "your organization's policy allows only the commands it defines; '" + name +
               "' cannot be added";
    }
    for (const auto& e : policy_commands()) {
        if (e.first == name) {
            return "'" + name + "' is defined by your organization's policy and cannot be redefined";
        }
    }
    return "";
}

std::string command_line_refusal(const std::string& cmdline) {
    // Only outside double quotes: a quoted "a|b" is one literal argument, the
    // way the template tokenizer reads it.
#ifdef _WIN32
    const char* const shell = "|<>&`";
#else
    const char* const shell = "|<>&`;";
#endif
    bool in_quotes = false;
    for (char c : cmdline) {
        if (c == '"') { in_quotes = !in_quotes; continue; }
        // (strchr finds the terminator too, so a NUL is excluded by hand.)
        if (in_quotes || c == 0 || std::strchr(shell, c) == nullptr) continue;
        return std::string("the command line uses shell syntax ('") + c +
               "'), and commands are not run through a shell. Put the command line in a "
               "script in " + machine_script_folder().u8string() +
               (policy_allows_user_commands() ? " or " + user_script_folder().u8string() : "") +
               " and name the script instead";
    }
    return "";
}

std::filesystem::path machine_script_folder() {
    for (const auto& e : policy_entries()) {
        // Only an absolute path: a relative one would mean a different folder
        // in every project.
        if (e.first != "script-folder") continue;
        const auto p = std::filesystem::u8path(e.second);
        if (p.is_absolute()) return p;
    }
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
    const std::filesystem::path pf =
        (n > 0 && n < MAX_PATH) ? std::filesystem::path(std::wstring(buf, n))
                                : std::filesystem::path(L"C:\\Program Files");
    return pf / "Centlake" / "tapto" / "scripts";
#else
    return "/etc/tapto/scripts";
#endif
}

std::filesystem::path user_script_folder() {
    return global_dir() / "scripts";
}

void add_command(Level level, const std::string& name, const std::string& command) {
    if (level == Level::Policy) throw std::invalid_argument("the policy scope is read-only");
    auto path = commands_path(level);
    Config store = Config::load(path);
    store.set(name, command);
    store.save(path);
}

bool remove_command(Level level, const std::string& name) {
    if (level == Level::Policy) return false;
    auto path = commands_path(level);
    Config store = Config::load(path);
    if (!store.unset(name)) return false;
    store.save(path);
    return true;
}

} // namespace tapto
