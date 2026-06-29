// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <map>
#include <string>
#include <vector>

#include "tapto/paths.h"

namespace tapto {

// A user-defined, allow-listed command: a short name mapped to a command line,
// plus the scope it came from.
struct CommandEntry {
    std::string name;
    std::string command;
    Level origin;
};

// All commands merged across scopes (local overrides global overrides system),
// keyed by name.
std::map<std::string, std::string> merged_commands();

// Effective commands with their originating scope, first-seen order preserved.
std::vector<CommandEntry> effective_commands();

// Commands defined in a single scope.
std::vector<CommandEntry> commands_in_scope(Level level);

// Add or update a command in the given scope. Throws on I/O failure.
void add_command(Level level, const std::string& name, const std::string& command);

// Remove a command from the given scope; returns true if it existed.
bool remove_command(Level level, const std::string& name);

} // namespace tapto
