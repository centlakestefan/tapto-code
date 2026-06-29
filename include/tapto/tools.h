// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <vector>

#include "tool_registry.h"

namespace tapto {

// Returns the tools registered for chat: the str_replace text editor and a
// file-search tool, both operating on the local filesystem relative to the
// current working directory.
std::vector<ToolSpec> builtin_tools();

} // namespace tapto
