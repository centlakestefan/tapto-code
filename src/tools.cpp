// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/tools.h"

#include "tapto/commands.h"
#include "tapto/context.h"
#include "tapto/editor.h"
#include "tapto/pathrules.h"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace tapto {

namespace {

// --- path rules -----------------------------------------------------------
//
// The rules are libtapto's (tapto/pathrules.h), over the folders main grants,
// and so are the tools: the editor and find_files (tapto/editor.h), and the
// allow-listed commands with their built-ins (tapto/commands.h). What stays
// here is which folders those are, and what a refusal says.

// The directory tapto-code was started in (canonicalized). Computed once on
// first use; the process never changes its working directory.
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

// The set the rules apply to: main's, or without one (the tests that run the
// tools bare) the working directory alone, as its home root.
const FolderSet& folder_set() {
    static const FolderSet cwd_only = [] {
        FolderSet f;
        f.set_home(sandbox_root().u8string(), /*writable=*/true);
        return f;
    }();
    return g_folders ? *g_folders : cwd_only;
}

// No write reaches the command scripts, from a plugin either, which may come
// before the command tools are made. A refusal to write in a read-only grant
// names tapto-code's ways to read there: read_file, view, cat and the [ro]
// commands.
void set_advice_once() {
    static const bool once = [] {
        protect_folder(machine_script_folder());
        protect_folder(user_script_folder());
        set_read_only_advice(
            "Reading it is fine (read_file, view, cat, and the commands list_commands marks "
            "[ro]); to change it or run another command there, ask the user to grant the "
            "folder read-write with /add-folder <path> rw.");
        return true;
    }();
    (void)once;
}

// The tools `make` builds over the set, each calling the tool built for the
// set in force when it runs: main grants a folder after the tools are made,
// and the tests set one.
void add_late(std::vector<ToolSpec>& tools,
              const std::function<std::vector<ToolSpec>(const FolderSet&)>& make) {
    for (ToolSpec t : make(folder_set())) {
        t.executor = [make, name = t.name](Context& ctx, const json& in) -> std::string {
            for (const ToolSpec& now : make(folder_set())) {
                if (now.name == name) return now.executor(ctx, in);
            }
            return "ERROR: no tool " + name;
        };
        tools.push_back(std::move(t));
    }
}

} // namespace

void set_granted_folders(const FolderSet* folders) {
    g_folders = folders;
}

bool resolve_plugin_path(const std::string& input, bool write, fs::path& out,
                         std::string& display, std::string& error) {
    set_advice_once();
    return PathRules(folder_set()).resolve_plugin_path(input, write, out, display, error);
}

std::vector<ToolSpec> builtin_tools() {
    set_advice_once();
    std::vector<ToolSpec> tools;
    add_late(tools, [](const FolderSet& set) { return editor_tools(set); });
    add_late(tools, [](const FolderSet& set) { return command_tools(set); });
    return tools;
}

} // namespace tapto
