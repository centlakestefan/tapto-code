// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/commands.h"

#include <stdexcept>

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
