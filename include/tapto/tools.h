// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tapto/fstools.h"
#include "tapto/tool_registry.h"

namespace tapto {

// Returns the tools registered for chat, libtapto's over the granted folders:
// the str_replace text editor and file search (tapto/editor.h), and
// list_commands and run_command (tapto/commands.h).
std::vector<ToolSpec> builtin_tools();

// The folders the user has granted with /add-folder. The editor and
// find_files reach into the ones marked writable, by "<label>/<path>" or by
// absolute path, as if they were part of the working directory; read-only
// ones stay the province of the library's read_file and friends. The set must
// outlive the tools, and may change after this call: it is consulted on every
// resolution. Null (the default) means no grants. When the set has a home
// root (FolderSet::set_home) it must be the working directory; its label is
// then accepted like a grant's.
void set_granted_folders(const FolderSet* folders);

// Resolve a path argument of a plugin tool (a schema property marked
// "x-tapto-path"), so no plugin applies the path rules itself. `write` false
// reads as the editor reads: the working directory and every folder in the
// set. `write` true writes as a command's %p does: the working directory and
// the rw grants, never a git directory or ~/.tapto, and not @memory, which
// only the editor writes. On success `out` is the checked absolute path and
// `display` the path as tool results print it ("<label>/<rest>" in a
// granted folder, else the model's own spelling); on failure `error` is the
// tool result to return.
bool resolve_plugin_path(const std::string& input, bool write, std::filesystem::path& out,
                         std::string& display, std::string& error);

} // namespace tapto
