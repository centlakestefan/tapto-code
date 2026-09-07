// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>
#include <vector>

#include "tapto/tool_registry.h"

namespace tapto {

// Returns the tools registered for chat: the str_replace text editor and a
// file-search tool, both operating on the local filesystem relative to the
// current working directory.
std::vector<ToolSpec> builtin_tools();

// True if `name` is a reserved, always-available built-in command (wc, head,
// tail, cat, ls, tree). These are implemented in-process (cross-platform) and
// take precedence over user allow-listed commands, so the names can't be
// reused for a user command.
bool is_builtin_command(const std::string& name);

} // namespace tapto
