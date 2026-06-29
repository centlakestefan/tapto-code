// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/commands.h"

#include "tapto/config.h"

namespace tapto {

std::map<std::string, std::string> merged_commands() {
    std::map<std::string, std::string> out;
    for (Level lvl : {Level::System, Level::Global, Level::Local}) {
        Config store = Config::load(commands_path(lvl));
        for (const auto& e : store.entries()) {
            out[e.first] = e.second; // later scopes override earlier ones
        }
    }
    return out;
}

std::vector<CommandEntry> effective_commands() {
    std::vector<CommandEntry> merged;
    auto apply = [&](Level lvl) {
        Config store = Config::load(commands_path(lvl));
        for (const auto& e : store.entries()) {
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
    apply(Level::System);
    apply(Level::Global);
    apply(Level::Local);
    return merged;
}

std::vector<CommandEntry> commands_in_scope(Level level) {
    std::vector<CommandEntry> out;
    Config store = Config::load(commands_path(level));
    for (const auto& e : store.entries()) {
        out.push_back({e.first, e.second, level});
    }
    return out;
}

void add_command(Level level, const std::string& name, const std::string& command) {
    auto path = commands_path(level);
    Config store = Config::load(path);
    store.set(name, command);
    store.save(path);
}

bool remove_command(Level level, const std::string& name) {
    auto path = commands_path(level);
    Config store = Config::load(path);
    if (!store.unset(name)) return false;
    store.save(path);
    return true;
}

} // namespace tapto
