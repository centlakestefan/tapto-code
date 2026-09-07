// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>
#include <vector>

#include "tapto/fstools.h"
#include "tapto/tool_registry.h"

namespace tapto {

// Returns the tools registered for chat: the str_replace text editor and a
// file-search tool, both operating on the local filesystem relative to the
// current working directory.
std::vector<ToolSpec> builtin_tools();

// The folders the user has granted with /add-folder. The editor and
// find_files reach into the ones marked writable, by "<label>/<path>" or by
// absolute path, as if they were part of the working directory; read-only
// ones stay the province of the library's read_file and friends. The set must
// outlive the tools, and may change after this call: it is consulted on every
// resolution. Null (the default) means no grants.
void set_granted_folders(const FolderSet* folders);

// True if `name` is a reserved, always-available built-in command (wc, head,
// tail, cat, ls, tree). These are implemented in-process (cross-platform) and
// take precedence over user allow-listed commands, so the names can't be
// reused for a user command.
bool is_builtin_command(const std::string& name);

} // namespace tapto
