// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tapto/provider.h"

namespace tapto {

// Build the system prompt from the config store.
//
// The base prompt is `system-prompt-file` (a text file) or `system-prompt` (an
// inline value), else `builtin`. When both are set, the one from the more
// specific scope wins, and at the same scope the file does. The config format
// is one line per key, so a long or multi-line prompt belongs in a file.
//
// `system-prompt-append` and then `system-prompt-append-file` are added after
// the base, each separated by a blank line, so a project can add its own rules
// without copying the built-in prompt's instructions about the tools. When the
// base prompt is set by policy, only the policy's own -append keys apply.
//
// A relative file path resolves against the folder of the config file that
// set it; for the local (per-project) scope, whose file lives under the user's
// home rather than in the project, it resolves against `working_dir`. A policy
// path must be absolute. A file that can't be read, or that is binary, is
// skipped with a message in `problems` rather than failing the chat: the base
// falls back to an inline value from the same scope, else the built-in prompt.
std::string compose_system_prompt(const std::vector<EffectiveEntry>& config,
                                  const std::filesystem::path& working_dir,
                                  const std::string& builtin,
                                  std::vector<std::string>& problems);

} // namespace tapto
