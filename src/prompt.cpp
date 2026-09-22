// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/prompt.h"

#include <optional>

#include "tapto/fstools.h"
#include "tapto/paths.h"

namespace fs = std::filesystem;

namespace tapto {

namespace {

// The effective entry for `key`, if it is set to something. An empty value
// counts as unset, as it does for get_effective().
std::optional<EffectiveEntry> find_entry(const std::vector<EffectiveEntry>& config,
                                         const std::string& key) {
    for (const auto& e : config) {
        if (e.key == key) {
            if (e.value.empty()) return std::nullopt;
            return e;
        }
    }
    return std::nullopt;
}

// Where a relative path in `origin`'s config resolves. Empty for policy: an
// organization's prompt must not depend on the folder a user starts in.
fs::path base_dir(Level origin, const fs::path& working_dir) {
    if (origin == Level::Local) return working_dir;
    if (origin == Level::Policy) return fs::path();
    return config_path(origin).parent_path();
}

// Read the prompt file `entry` names into `out`: BOM dropped, CRLF folded to
// LF, and trailing blank space trimmed, since the parts are joined with a
// blank line of their own. False, with a message in `problems`, if it can't
// be used.
bool load_prompt_file(const EffectiveEntry& entry, const fs::path& working_dir,
                      std::string& out, std::vector<std::string>& problems) {
    fs::path path(entry.value);
    if (path.is_relative()) {
        const fs::path base = base_dir(entry.origin, working_dir);
        if (base.empty()) {
            problems.push_back(entry.key + " (" + level_name(entry.origin) +
                               ") must be an absolute path: " + entry.value);
            return false;
        }
        path = base / path;
    }
    std::string text;
    if (!read_file(path, text)) {
        problems.push_back(entry.key + ": cannot read " + path.string());
        return false;
    }
    if (looks_binary(text)) {
        problems.push_back(entry.key + ": " + path.string() + " is not a text file");
        return false;
    }
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    out.clear();
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        out += text[i];
    }
    const size_t last = out.find_last_not_of(" \t\r\n");
    out.erase(last == std::string::npos ? 0 : last + 1);
    return true;
}

} // namespace

std::string compose_system_prompt(const std::vector<EffectiveEntry>& config,
                                  const fs::path& working_dir,
                                  const std::string& builtin,
                                  std::vector<std::string>& problems) {
    const auto inline_prompt = find_entry(config, "system-prompt");
    const auto prompt_file = find_entry(config, "system-prompt-file");

    std::string prompt = inline_prompt ? inline_prompt->value : builtin;
    if (prompt_file && (!inline_prompt || prompt_file->origin >= inline_prompt->origin)) {
        // On failure `prompt` keeps the fallback. The inline value is only a
        // fallback from the same scope: one from a less specific scope was
        // overridden, and falling back to it would let a user's own prompt
        // stand in for a policy file that failed to load.
        if (inline_prompt && inline_prompt->origin != prompt_file->origin) prompt = builtin;
        std::string text;
        if (load_prompt_file(*prompt_file, working_dir, text, problems)) {
            if (!text.empty()) prompt = std::move(text);
            else problems.push_back("system-prompt-file: " + prompt_file->value + " is empty");
        }
    }

    // A prompt the organization sets is the whole prompt: user text appended
    // after it could contradict it, and an admin cannot lock the -append keys
    // out by leaving them empty, since an empty policy value is no policy at
    // all. Only the policy's own -append keys still apply.
    const bool policy_prompt = (inline_prompt && inline_prompt->origin == Level::Policy) ||
                               (prompt_file && prompt_file->origin == Level::Policy);
    auto usable = [&](const EffectiveEntry& e) {
        if (!policy_prompt || e.origin == Level::Policy) return true;
        problems.push_back(e.key + " (" + level_name(e.origin) +
                           ") is ignored: the system prompt is set by policy");
        return false;
    };

    if (const auto extra = find_entry(config, "system-prompt-append"); extra && usable(*extra)) {
        prompt += "\n\n" + extra->value;
    }
    if (const auto extra_file = find_entry(config, "system-prompt-append-file");
        extra_file && usable(*extra_file)) {
        std::string text;
        if (load_prompt_file(*extra_file, working_dir, text, problems) && !text.empty()) {
            prompt += "\n\n" + text;
        }
    }
    return prompt;
}

} // namespace tapto
