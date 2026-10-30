// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/commands.h"
#include "tapto/chat_commands.h"
#include "tapto/program.h"
#include "tapto/config.h"
#include "tapto/fstools.h"
#include "tapto/overflow.h"
#include "tapto/paths.h"
#include "tapto/plugins.h"
#include "tapto/policy.h"
#include "tapto/prompt.h"
#include "tapto/provider.h"
#include "tapto/secret.h"
#include "tapto/session.h"
#include "tapto/tools.h"

#include "tapto/claude.h"
#include "tapto/openai.h"
#include "tapto/gemini.h"
#include "tapto/aiconfig.h"
#include "tapto/cancel.h"
#include "tapto/log.h"
#include "tapto/termui.h"
#include "tapto/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <csignal>
#endif

using namespace tapto;

namespace {

// On Windows, make the console interpret ANSI/VT escape sequences (colors,
// cursor) and treat output as UTF-8 so Unicode in model replies (box-drawing
// table borders, em-dashes, emoji) renders instead of turning into mojibake.
// Safe no-op when output is redirected to a file or pipe.
void enable_console_features() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

// The color the user's own prompt is echoed in. It is the only input this
// program colors, so a hue the app uses for user-facing notices (yellow) keeps
// the lines the user typed standing out from the model's plain markdown in the
// scrollback. Painted by the chat loop (the > marker), by the POSIX editor's
// echo, and inherited by the Windows console's echo; a reset follows every
// read so it never leaks into the model's reply or the status line. Shared
// with ui.cpp, which shows earlier prompts in it on /resume.
using ui::kPromptColor;
using ui::kPromptReset;

const char* kUsage =
    "tapto-code - a small cross-platform CLI\n"
    "\n"
    "Usage:\n"
    "  tapto-code [--provider <name>] [--resume]     Start an interactive chat (default)\n"
    "  tapto-code [--system|--global|--local|--policy] config <command> [args]\n"
    "  tapto-code [--system|--global|--local|--policy] command <add|remove|list> ...\n"
    "\n"
    "Config commands:\n"
    "  set <key> <value>   Set a config value (default scope: local)\n"
    "  get <key>           Print the effective value of a key\n"
    "  unset <key>         Remove a key (default scope: local)\n"
    "  list                List config values\n"
    "\n"
    "Command commands (allow-listed commands the chat agent may run):\n"
    "  command add [--ro] [--note \"...\"] <name> <command...>\n"
    "                                    Allow-list a command (default scope: local);\n"
    "                                    --ro: it only reads, so it may also use\n"
    "                                    folders granted read-only; --note: a hint\n"
    "                                    shown to the model about how to use the\n"
    "                                    command (stored as a trailing '#', not run)\n"
    "  command remove <name>             Remove a command\n"
    "  command list                      List configured commands\n"
    "\n"
    "Other commands:\n"
    "  version             Print version info as JSON\n"
    "\n"
    "Chat config keys: provider (which provider block to use), max-output-tokens\n"
    "  (optional), max-tool-iterations (optional, default 200), print-cot\n"
    "  (optional, default true), system-prompt or system-prompt-file (replaces the\n"
    "  built-in prompt), system-prompt-append or system-prompt-append-file (added\n"
    "  after it), trace-file, connection-timeout\n"
    "  (seconds, default 30), read-timeout (seconds to wait for the whole answer,\n"
    "  default 300; raise it for a slow local model), and per provider\n"
    "  <name>-reasoning-effort (openai dialect only: low, medium, high, ...)\n"
    "\n"
    "A provider is a named block of keys, so several backends -- including two\n"
    "local servers speaking the same API -- coexist in one config store:\n"
    "\n"
    "  qwen36-provider-type = openai         gemma4-provider-type = openai\n"
    "  qwen36-provider-url  = http://a:8000  gemma4-provider-url  = http://b:8081\n"
    "  qwen36-model         = Qwen3-VL-30B   gemma4-model         = gemma-3-27b\n"
    "  qwen36-api-key       = local          gemma4-api-key       = local\n"
    "\n"
    "Then: tapto-code --provider gemma4, or 'provider = gemma4' for the default.\n"
    "-provider-type names the API shape to speak, not the model; claude, openai\n"
    "and gemini work as names with no config at all. The unscoped model,\n"
    "provider-url and api-key apply only to the default provider, so a local\n"
    "endpoint's URL is never sent to a hosted one, or its key to a local one.\n"
    "\n"
    "An api-key may say where the key lives instead of holding it:\n"
    "  env:ANTHROPIC_API_KEY        an environment variable\n"
    "  cmd:pass show anthropic      first line of a command's output\n"
    "  wincred:tapto/work-claude    Windows Credential Manager (cmdkey /generic:)\n"
    "\n"
    "Scope flags:\n"
    "  --system   machine-wide config\n"
    "  --global   current user's config (~/.tapto)\n"
    "  --local    per-folder config, kept under ~/.tapto/projects rather than in\n"
    "             the folder itself, so a cloned repo can't ship one; the default\n"
    "             for writes\n"
    "  --policy   what your organization mandates (read-only; see below)\n"
    "\n"
    "Options:\n"
    "  --provider <name>  chat with this provider instead of the configured default\n"
    "  --resume           continue the conversation last held in this folder\n"
    "  --show-origin      with 'list', prefix each entry with its scope\n"
    "  -h, --help         show this help\n"
    "\n"
    "Precedence (highest wins): policy > local > global > system\n"
    "\n"
    "Policy is config an administrator sets through Group Policy (registry\n"
    "values under HKLM or HKCU\\SOFTWARE\\Policies\\Centlake\\tapto, from the\n"
    "tapto.admx template) or, elsewhere, in /etc/tapto/policy. A key set there\n"
    "wins over every user scope and cannot be changed with 'config set'; a key\n"
    "not set there is left to the user. Policy may also confine --provider:\n"
    "  allowed-providers = work, review    only these names\n"
    "  allow-user-providers = 0            only providers the policy defines\n"
    "and allow-list commands of its own (the 'commands' subkey, or\n"
    "/etc/tapto/policy-commands), listed by 'command list' as policy and\n"
    "defined above the user's; with\n"
    "  allow-user-commands = 0             they are the only ones\n"
    "and confine plugins:\n"
    "  allowed-plugins = image, jira       only these may be loaded\n"
    "  allow-user-plugins = 0              only those its own plugin.<name> load\n";

struct Args {
    std::optional<Level> level;
    bool show_origin = false;
    std::vector<std::string> positional; // [0]=subcommand, [1]=key, [2]=value
};

// True if `key` ends with `suffix`, with at least one character before it.
bool has_suffix(const std::string& key, const std::string& suffix) {
    return key.size() > suffix.size() &&
           key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The keys a provider block owns, each written as `<name>-<key>`.
const char* kProviderBlockKeys[] = {"provider-type", "api-key", "provider-url", "model",
                                    "reasoning-effort"};

// A provider name has to survive a round trip through the `key = value` config
// format, so it is limited to characters that can't be mistaken for syntax.
bool is_valid_provider_name(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

// If `key` is a provider-block key, the block's name; otherwise empty.
// `provider-type` alone is not one: it is the legacy unscoped key.
std::string provider_name_of_key(const std::string& key, const std::string& block_key) {
    const std::string suffix = "-" + block_key;
    if (!has_suffix(key, suffix)) return "";
    return key.substr(0, key.size() - suffix.size());
}

// Config keys tapto-code understands; `config set` rejects anything else.
// Besides the unscoped keys there is one set per provider block, under a
// free-form name: qwen36-provider-type, qwen36-model, qwen36-api-key, ...
bool is_supported_config_key(const std::string& key) {
    static const char* kKeys[] = {
        "provider", "provider-type", "api-key", "provider-url", "model",
        "max-output-tokens", "max-tool-iterations", "system-prompt", "system-prompt-file",
        "system-prompt-append", "system-prompt-append-file", "trace-file", "print-cot",
        "connection-timeout", "read-timeout",
        "reasoning-effort",
    };
    for (const char* k : kKeys) {
        if (key == k) return true;
    }
    // plugin.<name> = ro | rw | off: which plugins load at start.
    if (const std::string name = plugin_of_key(key); !name.empty())
        return name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") ==
                   std::string::npos &&
               name[0] >= 'a' && name[0] <= 'z';
    for (const char* k : kProviderBlockKeys) {
        if (is_valid_provider_name(provider_name_of_key(key, k))) return true;
    }
    return false;
}

const char* kSupportedKeysHelp =
    "provider, provider-url, model, api-key, "
    "max-output-tokens, max-tool-iterations, system-prompt, system-prompt-file, "
    "system-prompt-append, system-prompt-append-file, trace-file, print-cot, "
    "connection-timeout, read-timeout, plugin.<name> (ro, rw or off), "
    "per provider <name>-provider-type, <name>-provider-url, <name>-model, "
    "<name>-api-key, <name>-reasoning-effort, "
    "and the keys of an installed plugin, <plugin>-... (jira-url, jira-api-token, ...); "
    "any of them for one program alone after its name: tapto-code.print-cot, "
    "tapto-word.plugin.mcp, ...";

// True for the unscoped api-key and for any provider block's own key, so both
// are masked when listed and both warn about plaintext storage when written.
bool is_api_key_key(const std::string& key) {
    return key == "api-key" || has_suffix(key, "-api-key");
}

int cmd_set(const Args& a) {
    if (a.positional.size() < 3) {
        ui::print_error("'set' requires <key> <value>");
        return 2;
    }
    // A key may be one program's ("tapto-word.print-cot"): what follows the
    // program's name is checked like any key.
    const std::string base_key = split_program_key(a.positional[1]);
    // A key tapto-code does not know may be an installed plugin's.
    std::optional<PluginConfigKey> plugin_key;
    if (!is_supported_config_key(base_key)) {
        std::string why;
        plugin_key = plugin_config_key(base_key, why);
        if (!plugin_key) {
            ui::print_error("unknown config key '" + a.positional[1] + "'" +
                            (why.empty() ? std::string() : " (" + why + ")") +
                            ".\n  supported keys: " + kSupportedKeysHelp);
            return 2;
        }
    }
    // plugin.<name>: a mode, and for one that loads, a plugin that is there
    // and that the policy lets the user load. "off" may name any plugin, so a
    // project can turn off one the user's own config loads.
    if (const std::string name = plugin_of_key(base_key); !name.empty()) {
        const auto mode = parse_plugin_mode(a.positional[2]);
        if (!mode) {
            ui::print_error(a.positional[1] + " takes ro, rw or off, not '" + a.positional[2] +
                            "'");
            return 2;
        }
        if (*mode != PluginMode::Off) {
            const auto installed = available_plugins();
            if (std::find(installed.begin(), installed.end(), name) == installed.end()) {
                std::string list;
                for (const auto& n : installed) list += (list.empty() ? "" : ", ") + n;
                ui::print_error("there is no plugin '" + name + "' in " +
                                plugin_directory().u8string() +
                                (list.empty() ? std::string(" (none is installed)")
                                              : " (installed: " + list + ")"));
                return 2;
            }
            if (std::string why = plugin_policy_refusal(name, false, policy_entries());
                !why.empty()) {
                ui::print_error("plugin '" + name + "' cannot be loaded: " + why);
                return 2;
            }
        }
    }
    Level level = a.level.value_or(Level::Local);
    if (level == Level::Policy) {
        ui::print_error("the policy scope is read-only; it is set by your organization in " +
                        policy_source());
        return 2;
    }
    // Writing it would succeed and change nothing, since policy is applied
    // last -- worse than refusing, because the user would think it took.
    if (is_policy_managed(a.positional[1])) {
        ui::print_error("'" + a.positional[1] + "' is managed by your organization's policy (" +
                        policy_source() + ") and cannot be changed here");
        return 2;
    }
    auto path = config_path(level);
    Config cfg = Config::load(path);
    cfg.set(a.positional[1], a.positional[2]);
    try {
        cfg.save(path);
    } catch (const std::exception& e) {
        ui::print_error(e.what());
        return 1;
    }
    // A reference stores no secret, so it needs no warning — it is the remedy
    // the warning points at.
    if (is_api_key_key(a.positional[1]) && !is_secret_reference(a.positional[2])) {
        ui::print_warning(a.positional[1] + " stored in plaintext at " + path.string() +
                          "; set the provider's API key env var (e.g. ANTHROPIC_API_KEY), "
                          "or use env: / cmd: / wincred: to keep it out of the file");
    } else if (plugin_key && plugin_key->secret && !is_secret_reference(a.positional[2])) {
        ui::print_warning(a.positional[1] + " stored in plaintext at " + path.string() +
                          "; use env: / cmd: / wincred: to keep it out of the file");
    }
    return 0;
}

int cmd_get(const Args& a) {
    if (a.positional.size() < 2) {
        ui::print_error("'get' requires <key>");
        return 2;
    }
    const std::string& key = a.positional[1];

    if (a.level == Level::Policy) {
        for (const auto& e : policy_entries()) {
            if (e.first == key) {
                ui::print_line(e.second);
                return 0;
            }
        }
        return 1; // not found
    }
    if (a.level) {
        Config cfg = Config::load(config_path(*a.level));
        if (auto value = cfg.get(key)) {
            ui::print_line(*value);
            return 0;
        }
        return 1; // not found
    }

    for (const auto& entry : effective_config()) {
        if (entry.key == key) {
            ui::print_line(entry.value);
            return 0;
        }
    }
    return 1; // not found
}

int cmd_unset(const Args& a) {
    if (a.positional.size() < 2) {
        ui::print_error("'unset' requires <key>");
        return 2;
    }
    Level level = a.level.value_or(Level::Local);
    if (level == Level::Policy) {
        ui::print_error("the policy scope is read-only; it is set by your organization in " +
                        policy_source());
        return 2;
    }
    auto path = config_path(level);
    Config cfg = Config::load(path);
    if (!cfg.unset(a.positional[1])) {
        ui::print_error("key not found: " + a.positional[1]);
        return 1;
    }
    try {
        cfg.save(path);
    } catch (const std::exception& e) {
        ui::print_error(e.what());
        return 1;
    }
    return 0;
}

// Mask a secret for display: keep a short prefix and last 4 chars so the entry
// is identifiable without revealing the value. `config get` still shows it in full.
std::string mask_secret(const std::string& v) {
    if (v.size() <= 12) return "****";
    return v.substr(0, 6) + "..." + v.substr(v.size() - 4);
}

// A reference (env:/cmd:/wincred:) says where the key lives rather than being
// it, so it is shown in full — masking it would hide the one part of the entry
// worth reading.
//
// A plugin's key is masked when the plugin declares it secret. One no
// installed plugin vouches for (its plugin removed, or a sibling program's
// key) is masked when its name looks like a secret's.
bool is_secret_key(const std::string& key) {
    if (is_api_key_key(key)) return true;
    if (is_supported_config_key(key)) return false;
    static std::map<std::string, bool> known; // a module is loaded once per key
    if (auto it = known.find(key); it != known.end()) return it->second;
    std::string why;
    bool secret = false;
    if (auto pk = plugin_config_key(key, why)) {
        secret = pk->secret;
    } else {
        for (const char* s : {"-token", "-password", "-secret", "-key"})
            secret = secret || has_suffix(key, s);
    }
    return known[key] = secret;
}

std::string list_value(const std::string& key, const std::string& value) {
    return (is_secret_key(key) && !is_secret_reference(value)) ? mask_secret(value) : value;
}

int cmd_list(const Args& a) {
    if (a.level) {
        // The policy scope is not a file on Windows, so it is read through its
        // own reader; the others are the files they always were.
        const std::vector<Config::Entry> entries = (*a.level == Level::Policy)
            ? policy_entries()
            : Config::load(config_path(*a.level)).entries();
        for (const auto& entry : entries) {
            ui::print_config_entry(a.show_origin ? level_name(*a.level) : "",
                                   entry.first,
                                   list_value(entry.first, entry.second));
        }
        return 0;
    }

    for (const auto& entry : effective_config()) {
        ui::print_config_entry(a.show_origin ? level_name(entry.origin) : "",
                               entry.key,
                               list_value(entry.key, entry.value));
    }
    return 0;
}

int cmd_version() {
    nlohmann::json info;
    info["name"] = "tapto-code";
    info["version"] = TAPTO_CODE_VERSION;
    // The commit the binary was built from, which the version alone does not
    // pin down: a release is tagged once and built many times, and "-dirty"
    // marks one that had uncommitted changes and cannot be reproduced from the
    // hash.
    info["commit"] = TAPTO_CODE_COMMIT;
    ui::print_line(info.dump(2));
    return 0;
}

// Handles the `command` subcommand (add / remove / list). `rest` is the raw
// argument list after `command`, taken verbatim so `add` can capture a command
// line containing tokens like --build or --config.
int cmd_command(std::optional<Level> level, std::vector<std::string> rest) {
    if (rest.empty()) {
        ui::print_usage("usage: tapto-code [--scope] command <add|remove|list> ...\n");
        return 2;
    }
    const std::string& sub = rest[0];

    if (sub == "add") {
        // --ro marks the command [ro]: it only reads (tapto/commands.h).
        bool reads_only = false;
        if (rest.size() > 1 && rest[1] == "--ro") {
            reads_only = true;
            rest.erase(rest.begin() + 1);
        }
        // --note "..." is a hint for the model, stored as a trailing "#". It
        // must come after --ro, so a note that begins with --ro is not lost.
        std::string note;
        for (std::size_t i = 1; i + 1 < rest.size(); ++i) {
            if (rest[i] == "--note") {
                note = rest[i + 1];
                rest.erase(rest.begin() + std::ptrdiff_t(i), rest.begin() + std::ptrdiff_t(i) + 2);
                break;
            }
        }
        if (rest.size() < 3) {
            ui::print_error("'command add' requires [--ro] [--note \"...\"] <name> <command...>");
            return 2;
        }
        const std::string& name = rest[1];
        if (name.find_first_of(" \t=") != std::string::npos) {
            ui::print_error("command name must not contain spaces or '='");
            return 2;
        }
        if (tapto::is_builtin_command(name)) {
            ui::print_error("'" + name + "' is a reserved built-in command and can't be redefined");
            return 2;
        }
        std::string cmdline;
        for (std::size_t i = 2; i < rest.size(); ++i) {
            if (i > 2) cmdline += ' ';
            cmdline += rest[i];
        }
        if (reads_only && !command_reads_only(cmdline)) cmdline = std::string(kReadOnlyMark) + " " + cmdline;
        if (!note.empty()) cmdline += " # " + note;
        Level lvl = level.value_or(Level::Local);
        if (lvl == Level::Policy) {
            ui::print_error("the policy scope is read-only; it is set by your organization in " +
                            policy_source());
            return 2;
        }
        if (std::string why = command_policy_refusal(name); !why.empty()) {
            ui::print_error(why);
            return 2;
        }
        if (std::string why = command_line_refusal(cmdline); !why.empty()) {
            ui::print_error(why);
            return 2;
        }
        try {
            add_command(lvl, name, cmdline);
        } catch (const std::exception& e) {
            ui::print_error(e.what());
            return 1;
        }
        ui::print_command_added(name, level_name(lvl), cmdline);
        return 0;
    }

    if (sub == "remove" || sub == "rm") {
        if (rest.size() < 2) {
            ui::print_error("'command remove' requires <name>");
            return 2;
        }
        Level lvl = level.value_or(Level::Local);
        if (lvl == Level::Policy) {
            ui::print_error("the policy scope is read-only; it is set by your organization in " +
                            policy_source());
            return 2;
        }
        if (!remove_command(lvl, rest[1])) {
            ui::print_error("command not found in " + std::string(level_name(lvl)) +
                            " scope: " + rest[1]);
            return 1;
        }
        ui::print_command_removed(rest[1], level_name(lvl));
        return 0;
    }

    if (sub == "list") {
        if (level) {
            for (const auto& e : commands_in_scope(*level)) {
                ui::print_command_entry("", e.name, e.command);
            }
        } else {
            for (const auto& e : effective_commands()) {
                ui::print_command_entry(level_name(e.origin), e.name, e.command);
            }
        }
        return 0;
    }

    ui::print_error("unknown command subcommand '" + sub + "'");
    return 2;
}

const char* kDefaultSystemPrompt =
    "You are an experienced fullstack developer in a chat with a user. "
    "The user has started a session in a working folder. You have tools to "
    "view and edit files in that folder, search files, and run a set of "
    "pre-approved commands (use list_commands to see them and run_command to "
    "run them).";

// The effective system prompt: whatever is configured (system-prompt or
// system-prompt-file, plus the -append keys; see tapto/prompt.h), else the
// built-in default used in-memory. Chat never persists anything — persisting
// the default is the job of `tapto-code install`.
//
// The prompt is rebuilt on every folder grant, so a problem with a prompt file
// is reported once per session rather than on each rebuild.
std::string resolve_system_prompt() {
    static std::vector<std::string> reported;
    std::vector<std::string> problems;
    std::error_code ec;
    std::string prompt = compose_system_prompt(effective_config(), std::filesystem::current_path(ec),
                                               kDefaultSystemPrompt, problems);
    for (const auto& p : problems) {
        if (std::find(reported.begin(), reported.end(), p) != reported.end()) continue;
        ui::print_warning(p);
        reported.push_back(p);
    }
    return prompt;
}

// Trim surrounding whitespace (incl. a trailing CR from piped CRLF input).
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// A slash-command argument with its surrounding quotes removed, so a path
// with spaces can be pasted as the shell shows it.
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}


// Write a key to the global (user) config scope.
// /list-plugins: what is loaded, in what mode, and what else is installed.
std::string list_plugins_text(const Plugins& plugins) {
    std::ostringstream out;
    const auto loaded = plugins.loaded();
    if (loaded.empty()) {
        out << "No plugin is loaded.";
    } else {
        out << "Plugins loaded:";
        for (const auto& e : loaded) {
            out << "\n  " << e.name << (e.version.empty() ? "" : " " + e.version) << "  "
                << (e.writable ? "rw" : "ro") << "  " << e.tools
                << (e.tools == 1 ? " tool" : " tools");
            if (e.write_tools)
                out << ", " << e.write_tools << " that change things"
                    << (e.writable ? "" : " (not offered)");
            if (!e.folder.empty()) out << ", its folder " << e.folder;
        }
    }
    std::string rest;
    for (const auto& n : available_plugins()) {
        bool have = false;
        for (const auto& e : loaded) have = have || e.name == n;
        if (!have) rest += (rest.empty() ? "" : ", ") + n;
    }
    out << "\n" << (rest.empty() ? std::string("No other plugin is installed")
                                 : "Installed, not loaded: " + rest)
        << " (in " << plugin_directory().u8string() << ").";
    if (!rest.empty()) out << " Load one with /add-plugin <name> [ro|rw].";
    return out.str();
}

bool set_global(const std::string& key, const std::string& value) {
    if (is_policy_managed(key)) {
        ui::print_error("'" + key + "' is managed by your organization's policy and cannot be set");
        return false;
    }
    try {
        auto path = config_path(Level::Global);
        Config cfg = Config::load(path);
        cfg.set(key, value);
        cfg.save(path);
        return true;
    } catch (const std::exception& e) {
        ui::print_error(e.what());
        return false;
    }
}

// First-run setup: when the essentials aren't configured, interactively prompt
// for a provider and its API key and store them in the global config. Returns
// false if the user aborts (EOF) or gives invalid input.
//
// What it writes is a named provider block — `provider = claude` plus
// `claude-api-key` — which is the shape tapto-vnc reads from the same store, and
// leaves room for a second provider later without the two sharing one key.
bool first_run_setup() {
    ui::print_setup_welcome();

    std::string name = default_provider_name().value_or("");
    if (name.empty()) {
        ui::print_setup_provider_prompt();
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        name = trim(line);
        // Setup only offers the three dialects by name; a block under a name of
        // its own is something to write into the config by hand afterwards.
        if (!is_dialect(name)) {
            ui::print_error("provider must be claude, openai, or gemini");
            return false;
        }
        if (!set_global("provider", name)) return false;
    }

    const std::string dialect = provider_dialect(name);
    const char* keyvar = api_key_env_var(dialect);
    const Secret existing = resolve_api_key(name, dialect);
    // A key that is configured but unreadable is a problem to report, not one to
    // prompt over: writing a second key would leave the broken reference in place.
    if (!existing.error.empty()) {
        ui::print_error(existing.error);
        return false;
    }
    // Only prompt for a key if one isn't already available (env var included).
    if (!existing.ok()) {
        ui::print_setup_apikey_prompt(keyvar);
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        std::string key = trim(line);
        if (!key.empty()) {
            if (!set_global(name + "-api-key", key)) return false;
            // A reference (env:/cmd:/wincred:) is not a secret, so it earns no
            // warning — storing one is the point of having them.
            if (!is_secret_reference(key)) {
                std::string warn = name + "-api-key stored in plaintext at " +
                                   config_path(Level::Global).string();
                if (keyvar && *keyvar)
                    warn += "; set " + std::string(keyvar) + " to avoid storing it on disk";
                ui::print_warning(warn);
            }
        }
        // A blank entry means the user intends to use the environment variable.
    }

    ui::print_setup_saved(config_path(Level::Global).string());
    return true;
}

#ifdef _WIN32
// Read one cooked line from the console (line editing / backspace still work).
// Returns false on read failure; sets got=false on EOF (Ctrl-Z). The returned
// UTF-8 string has any trailing CR/LF stripped.
bool win_read_console_line(HANDLE in, std::string& utf8, bool& got) {
    got = false;
    std::wstring wline;
    wchar_t buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadConsoleW(in, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), &n, nullptr))
            return false;
        if (n == 0) return true;          // EOF: got stays false
        wline.append(buf, n);
        if (!wline.empty() && wline.back() == L'\n') break; // full line read
    }
    got = true;
    while (!wline.empty() && (wline.back() == L'\n' || wline.back() == L'\r'))
        wline.pop_back();
    utf8.clear();
    if (wline.empty()) return true;
    int len = WideCharToMultiByte(CP_UTF8, 0, wline.data(), (int)wline.size(),
                                  nullptr, 0, nullptr, nullptr);
    utf8.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, wline.data(), (int)wline.size(),
                        &utf8[0], len, nullptr, nullptr);
    return true;
}

// True when the console input queue already holds a typed/pasted character,
// i.e. more of a paste is waiting. A paste dumps all of its lines into the
// queue at once, so this lets us tell a multi-line paste apart from a human
// pressing Enter. Stray key-up / control records (e.g. the Enter key-up left
// after the line we just read) are ignored so we never block waiting for input
// that isn't coming.
bool win_char_input_pending(HANDLE in) {
    DWORD avail = 0;
    if (!GetNumberOfConsoleInputEvents(in, &avail) || avail == 0) return false;
    std::vector<INPUT_RECORD> recs(avail);
    DWORD read = 0;
    if (!PeekConsoleInput(in, recs.data(), avail, &read)) return false;
    for (DWORD i = 0; i < read; ++i) {
        const INPUT_RECORD& r = recs[i];
        if (r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown &&
            r.Event.KeyEvent.uChar.UnicodeChar != 0)
            return true;
    }
    return false;
}
#endif

// RAII terminal guard: unbuffered, no-echo stdin.
//
// Used in two places, both of which do their own input handling after a
// select() and therefore need no line discipline at all:
//
//  * The ESC interrupt poll during a chat() call: the read must not wait for
//    a full line, so canonical (line) mode is disabled.
//  * Editing a prompt line (posix_read_line): the program does the echo and
//    line editing itself (arrows, history, pastes), so echo and canonical
//    mode are disabled there as well. ISIG stays on, so Ctrl-C still raises
//    SIGINT from the prompt exactly as before.
//
// Ctrl-C (or a kill) ends the program without running destructors, so the
// state to put back is also kept where a signal handler can reach it: the
// handler restores the terminal and then dies of the same signal, as the
// program did before. Without it the shell is left with echo and line mode
// off. The two guards are never active at once, so one saved state does.
// ---------------------------------------------------------------------------
#ifndef _WIN32
termios g_tty_saved{};
volatile sig_atomic_t g_tty_raw = 0;

void restore_tty_and_reraise(int sig) {
    if (g_tty_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
        // The prompt color off and the cursor shown (it is hidden while the
        // model works), and the shell's prompt on a line of its own.
        static const char kReset[] = "\x1b[0m\x1b[?25h\n";
        ssize_t ignored = write(STDOUT_FILENO, kReset, sizeof(kReset) - 1);
        (void)ignored;
    }
    raise(sig); // SA_RESETHAND has put the default action back
}

void install_tty_restore() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    struct sigaction sa{};
    sa.sa_handler = restore_tty_and_reraise;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) {
        struct sigaction old{};
        // A signal the program was started with ignored stays ignored.
        if (sigaction(sig, nullptr, &old) == 0 && old.sa_handler == SIG_IGN) continue;
        sigaction(sig, &sa, nullptr);
    }
}
#endif

struct RawTerminal {
    RawTerminal() {
#ifndef _WIN32
        if (tcgetattr(STDIN_FILENO, &m_saved) != 0) return;
        m_raw = m_saved;
        m_raw.c_lflag &= ~(ICANON | ECHO);
        m_raw.c_cc[VMIN]  = 0;
        m_raw.c_cc[VTIME] = 0;
        install_tty_restore();
        g_tty_saved = m_saved;
        g_tty_raw = 1;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &m_raw) == 0)
            m_active = true;
        else
            g_tty_raw = 0;
#else
        (void)this; // _kbhit works in cooked mode; nothing to do
#endif
    }
    ~RawTerminal() {
#ifndef _WIN32
        if (m_active) tcsetattr(STDIN_FILENO, TCSANOW, &m_saved);
        g_tty_raw = 0;
#else
        (void)this;
#endif
    }
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;
#ifndef _WIN32
    termios m_saved{};
    termios m_raw{};
    bool    m_active{false};
#else
    int m_dummy;
#endif
};

// ---------------------------------------------------------------------------
// Reading an interactive prompt line on a POSIX TTY, with the program doing
// its own line editing (echo off).
//
// A terminal sends the arrow keys as ESC sequences (ESC [ A / D / B / C for
// up / left / down / right), so reading the line the generic way (getline)
// records them as the characters "[A[D[B[C" and sends them on to the model.
// This editor consumes those sequences as cursor moves and history recall
// instead, and echoes the line itself as it is typed. Multi-line pastes are
// captured through the bracketed-paste markers (ESC [ 200~ ... ESC [ 201~)
// that most terminals send, and land in the line as newlines. A typed line
// break is made by ending the line in a backslash and pressing Enter (as in
// a shell): no terminal sends a distinct key for Shift-Enter, so Enter and
// Shift-Enter arrive as the same byte and only a typed marker can tell the
// two apart.
//
// Returns false on EOF, or when stdin is not a terminal (read_user_input
// then falls back to getline).
// ---------------------------------------------------------------------------
#ifndef _WIN32
// History of the previous submissions, oldest first (back() is newest).
// Session-scoped on purpose: prompts from a previous session are noise here.
std::vector<std::string>& posix_history() {
    static std::vector<std::string> history;
    return history;
}

// Remove the C1 control characters (U+0080 to U+009F, bytes C2 80 to C2 9F
// in UTF-8) from `s`. Some terminals act on them the way they act on an ESC
// sequence, so text the editor echoes must not carry them.
void drop_c1_controls(std::string& s) {
    std::string kept;
    kept.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == 0xC2 && i + 1 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) < 0xA0) {
            ++i; // the control and its lead byte both go
            continue;
        }
        kept += s[i];
    }
    s = std::move(kept);
}

// The columns a terminal gives the UTF-8 character of `adv` bytes at s[i]: 0
// for combining marks and joiners, 2 for East Asian wide characters and
// emoji, 1 for the rest. A table of the common ranges rather than wcwidth(),
// which follows the C locale this program never sets, and so says -1 for
// everything past ASCII.
size_t display_cols(const std::string& s, size_t i, size_t adv) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
    uint32_t cp = byte(0);
    if (adv == 2) cp = ((cp & 0x1F) << 6) | (byte(1) & 0x3F);
    else if (adv == 3) cp = ((cp & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) | (byte(2) & 0x3F);
    else if (adv == 4)
        cp = ((cp & 0x07) << 18) | ((byte(1) & 0x3F) << 12) | ((byte(2) & 0x3F) << 6) |
             (byte(3) & 0x3F);
    struct Range { uint32_t lo, hi; };
    static const Range kZero[] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
        {0x064B, 0x065F}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
        {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF},
    };
    static const Range kWide[] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x2614, 0x2615},
        {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
        {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F5}, {0x26FA, 0x26FD},
        {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
        {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
        {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
        {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
        {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004},
        {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251},
        {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F900, 0x1F9FF},
        {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
    };
    for (const Range& r : kZero) {
        if (cp >= r.lo && cp <= r.hi) return 0;
    }
    for (const Range& r : kWide) {
        if (cp >= r.lo && cp <= r.hi) return 2;
    }
    return 1;
}

// Read one line from a terminal, with the program doing its own line
// editing (echo off). The arrow keys' ESC sequences are consumed as cursor
// moves and history recall instead of being recorded as "[A" and friends.
// Bracketed pastes land in the line with their newlines, and Enter at the
// end of a line ending in a backslash starts the next row instead of
// sending. Returns false on EOF, or when stdin is not a terminal.
bool posix_read_line(const std::string& prompt_text, std::string& out) {
    if (!isatty(STDIN_FILENO)) return false; // not a terminal; caller uses getline
    RawTerminal raw; // unbuffered, no echo; ISIG stays on, so Ctrl-C still
                     // raises SIGINT exactly as it did before this existed

    std::vector<std::string>& history = posix_history();
    size_t hist_pos = 0; // 0: a fresh line; 1: newest ... history.size(): oldest

    std::string line; // the line being edited
    size_t pos = 0;   // cursor, in bytes inside `line`, always a char boundary
    std::string paste;
    bool in_paste = false;

    // Columns the echo can use before it has to wrap.
    int term_cols = 80;
    {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_cols = ws.ws_col;
    }

    // Advance one UTF-8 character (bytes) from index i.
    auto next_char = [&](const std::string& s, size_t i) -> size_t {
        if (i >= s.size()) return i;
        unsigned char b = static_cast<unsigned char>(s[i]);
        size_t adv = 1;
        if ((b & 0xE0) == 0xC0) adv = 2;
        else if ((b & 0xF0) == 0xE0) adv = 3;
        else if ((b & 0xF8) == 0xF0) adv = 4;
        if (adv > s.size() - i) adv = s.size() - i;
        return i + adv;
    };
    // Start of the UTF-8 character that ends at `p` (a cursor position): back
    // over its continuation bytes to its lead byte.
    auto prev_char_start = [&](size_t p) -> size_t {
        if (p == 0) return 0;
        size_t start = p - 1;
        while (start > 0 && (static_cast<unsigned char>(line[start]) & 0xC0) == 0x80) --start;
        return start;
    };

    // The screen row the cursor is on, counted from the block's first row (the
    // prompt's), so a reprint can move back up to the top before clearing.
    size_t cursor_row = 0;
    // The chat loop shows the cursor at the prompt (\x1b[?25h) and hides it
    // while the model works, so a reprint must leave it visible: an erase
    // that leaves the cursor where it hid would hide it again. Starts shown
    // because the prompt is printed with the cursor visible.
    static bool cursor_shown = true;
    // Redraw the prompt and the whole line, then put the cursor at the edit
    // position. The block is cleared from its first row down before it is
    // drawn, so no stale character survives a redraw.
    auto reprint = [&]() {
        if (cursor_row) std::cout << "\x1b[" << cursor_row << "A";
        std::cout << "\r\x1b[J";
        if (!cursor_shown) {
            std::cout << "\x1b[?25h";
            cursor_shown = true;
        }
        size_t used = 0; // columns used on the current row
        size_t row = 0;  // the current row, from the block's first
        size_t cur_row = 0, cur_col = 0; // where the edit position is drawn
        auto start_line = [&]() {
            // The marker and the typed text are echoed in the prompt color,
            // so what the user writes stands out from the model's plain
            // markdown. Width bookkeeping below uses the plain prompt's byte
            // length, so the escapes don't skew the wrap math. Each segment is
            // color + reset, so a reprint can break out early (cursor before
            // the line's end) without leaving the terminal colored.
            std::cout << kPromptColor << prompt_text << kPromptReset;
            used = prompt_text.size();
        };
        // The row is full: move down to the next one, already empty (cleared
        // above), and start it with the prompt again.
        auto wrap = [&]() {
            std::cout << "\n";
            ++row;
            start_line();
        };
        const size_t cols = static_cast<size_t>(term_cols);
        start_line();
        size_t i = 0;
        while (i < line.size()) {
            size_t adv = next_char(line, i) - i;
            if (line[i] == '\n') { // a pasted line break: next row, plain (no prompt)
                if (i == pos) { cur_row = row; cur_col = used; }
                std::cout << "\n";
                ++row;
                used = 0;
                i += adv;
                continue;
            }
            size_t w = display_cols(line, i, adv);
            if (used + w > cols) wrap();
            if (i == pos) { cur_row = row; cur_col = used; }
            std::cout << kPromptColor << line.substr(i, adv) << kPromptReset;
            used += w;
            i += adv;
        }
        if (pos >= line.size()) {
            // At the end of a full row the terminal holds the cursor in the
            // last column until the next character; wrap now instead, so the
            // cursor is where the next character will go.
            if (used >= cols) wrap();
            cur_row = row;
            cur_col = used;
        }
        // From the end of the drawing back to the edit position.
        if (row > cur_row) std::cout << "\x1b[" << (row - cur_row) << "A";
        std::cout << "\r";
        if (cur_col) std::cout << "\x1b[" << cur_col << "C";
        cursor_row = cur_row;
        std::cout << std::flush;
    };

    // One byte. `block`: wait for one; otherwise peek with a short timeout —
    // the bytes of a keystroke or a paste arrive in a burst, so the timeout
    // separates one keystroke from the next without ever blocking on input
    // that is not coming. A read pulls the whole burst that is ready and keeps
    // the tail in `pend`; `pend` is static so bytes that overran a submitted
    // line are not lost — they are still the user's input for the next prompt.
    static std::vector<unsigned char> pend;
    auto read_byte = [&](bool block) -> int {
        if (!pend.empty()) {
            unsigned char b = pend.front();
            pend.erase(pend.begin());
            return b;
        }
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv { 0, block ? 0 : 20000 };
            timeval* tvp = block ? nullptr : &tv;
            int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, tvp);
            if (r < 0) {
                if (errno == EINTR) continue; // interrupted; not EOF
                return -1;
            }
            if (r == 0 && !block) return -1; // peek timed out: no byte ready
            unsigned char buf[64];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n == -1) {
                if (errno == EINTR) continue; // interrupted; try again
                return -1;
            }
            if (n == 0) return -1; // EOF
            for (ssize_t i = 1; i < n; ++i) pend.push_back(buf[i]);
            return buf[0];
        }
    };

    // True when more input is already there, without waiting for any.
    auto input_ready = [&]() -> bool {
        if (!pend.empty()) return true;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 0 };
        return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
    };

    // Move the cursor to a character boundary (for insert/backspace).
    auto del_char_at = [&](size_t at) {
        size_t start = prev_char_start(at);
        line.erase(start, at - start);
        pos = start;
    };

    // Submit the current line: into the history if it is new, out to the
    // caller. `hist_pos` resets with it: the line being edited is no longer
    // a recall from history. The cursor goes to the end of the block and on
    // to a new line, as the terminal's own echo of Enter would, so what
    // prints next (a command's output, the next prompt) starts below the
    // whole line, not over the part after the cursor.
    auto submit = [&]() -> bool {
        if (pos != line.size()) {
            pos = line.size();
            reprint();
        }
        std::cout << "\r\n" << std::flush;
        if (!line.empty() && (history.empty() || history.back() != line))
            history.push_back(line);
        hist_pos = 0;
        cursor_row = 0;
        out = std::move(line);
        return true;
    };

    auto odd_trailing_backslashes = [&]() -> bool {
        size_t n = 0;
        while (n < line.size() && line[line.size() - 1 - n] == '\\') ++n;
        return n % 2 == 1;
    };

    for (;;) {
        // The terminal delivers the first byte of a character or sequence
        // when it is pressed, so a blocking read waits for exactly that.
        int b = read_byte(true);
        if (b < 0) break; // EOF
        if (b == 0x1B) {
            // An ESC: a bare interrupt key, or the start of a sequence. The
            // follow-up bytes, if any, arrive in the same burst.
            std::string seq(1, static_cast<char>(b));
            int c = read_byte(false);
            if (c >= 0) {
                seq += static_cast<char>(c);
                if (c == '[' || c == 'O') {
                    for (;;) {
                        int d = read_byte(false);
                        if (d < 0) break;
                        seq += static_cast<char>(d);
                        if (c == 'O' || (d >= 0x40 && d <= 0x7E)) break; // final byte
                    }
                }
            }
            if (in_paste) {
                // The end marker closes the paste. A real one is the last
                // thing the terminal sends for it: one with more input right
                // behind it came from the pasted text itself, written to end
                // the paste early and type the rest (Enter included) as if
                // from the keyboard. That one is dropped and the paste goes on.
                if (seq == "\x1b[201~" && !input_ready()) {
                    drop_c1_controls(paste);
                    line.insert(pos, paste);
                    pos += paste.size();
                    paste.clear();
                    in_paste = false;
                    reprint();
                }
                // Any other sequence in a paste is dropped: the editor echoes
                // the line, and echoed, it would drive the terminal.
                continue;
            }
            if (seq == "\x1b[200~") { in_paste = true; continue; }
            // Editing keys. None of them may leak into the line.
            if (seq == "\x1b[A") { // up: recall the older entry
                if (hist_pos < history.size()) {
                    size_t np = hist_pos + 1;
                    hist_pos = np;
                    line = history[history.size() - np];
                    pos = line.size();
                    reprint();
                }
            } else if (seq == "\x1b[B") { // down: the newer entry (0: fresh)
                if (hist_pos > 0) {
                    size_t np = hist_pos - 1;
                    hist_pos = np;
                    line = np ? history[history.size() - np] : "";
                    pos = line.size();
                    reprint();
                }
            } else if (seq == "\x1b[D" || seq == "\x1b[1D") { // left
                if (pos > 0) { pos = prev_char_start(pos); reprint(); }
            } else if (seq == "\x1b[C" || seq == "\x1b[1C") { // right
                if (pos < line.size()) { pos = next_char(line, pos); reprint(); }
            } else if (seq == "\x1b[H" || seq == "\x1b[1H") { // home
                pos = 0;
                reprint();
            } else if (seq == "\x1b[F" || seq == "\x1b[1F") { // end
                pos = line.size();
                reprint();
            }
            // Anything else (a bare ESC, function keys, other arrow forms)
            // is dropped.
            continue;
        }
        if (in_paste) {
            // The paste arrives unedited, newlines and tabs and all, but no
            // other control character: the editor echoes it.
            if (b == '\r' || b == '\n') paste += '\n';
            else if (b == '\t' || (b >= 0x20 && b != 0x7f)) paste += static_cast<char>(b);
            continue;
        }
        if (b == '\n' || b == '\r') {
            if (odd_trailing_backslashes() && line.size() >= 2) {
                line.pop_back();
                line += '\n';
                pos = line.size();
                reprint();
                continue;
            }
            return submit();
        }
        if (b == 0x7f || b == '\b') { // backspace / delete
            if (pos > 0) { del_char_at(pos); reprint(); }
            continue;
        }
        if (b == 4) { // Ctrl-D
            if (pos == line.size()) return submit(); // as in cooked mode
            if (line.empty()) break;                // EOF at an empty line
            size_t adv = next_char(line, pos) - pos; // otherwise delete the character
            line.erase(pos, adv);
            reprint();
            continue;
        }
        if (b == 1) { pos = 0; reprint(); continue; } // Ctrl-A: home
        if (b == 5) { pos = line.size(); reprint(); continue; } // Ctrl-E: end
        if (b == 2) { // Ctrl-B: left
            if (pos > 0) { pos = prev_char_start(pos); reprint(); }
            continue;
        }
        if (b == 6) { // Ctrl-F: right
            if (pos < line.size()) { pos = next_char(line, pos); reprint(); }
            continue;
        }
        if (b < 0x20) continue; // other controls: dropped
        // A printable character (ASCII, or the first byte of a multi-byte
        // character, whose remainder is peeked and read as one unit).
        std::string ch(1, static_cast<char>(b));
        int want = 0;
        unsigned char ub = static_cast<unsigned char>(b);
        if ((ub & 0xE0) == 0xC0) want = 2;
        else if ((ub & 0xF0) == 0xE0) want = 3;
        else if ((ub & 0xF8) == 0xF0) want = 4;
        for (int i = 1; i < want; ++i) {
            int c = read_byte(false);
            if (c < 0) break;
            ch += static_cast<char>(c);
        }
        drop_c1_controls(ch);
        if (ch.empty()) continue;
        line.insert(pos, ch);
        pos += ch.size();
        reprint();
    }
    return false; // EOF
}
#endif // _WIN32

// Read one logical prompt from stdin. Usually that's a single line, but a
// multi-line paste must arrive as a single prompt rather than one message per
// line. Two mechanisms handle this:
//
//  * Windows interactive console: the bracketed-paste markers below are only
//    emitted when stdin is in raw (ENABLE_VIRTUAL_TERMINAL_INPUT) mode, which
//    would disable cooked line editing. So instead we read cooked lines with
//    ReadConsoleW and, after each line, peek the console input queue: if more
//    character input is already waiting it belongs to the same paste, so we
//    keep reading and join the lines.
//
//  * Other terminals (POSIX, or redirected/piped stdin): most wrap pasted text
//    in ESC[200~ ... ESC[201~ markers. std::getline stops at the first embedded
//    newline, so we detect the start marker and keep reading until the end
//    marker (which may land on a later line), rejoining the pasted lines.
//    Terminals that don't support bracketed paste never send the markers, so
//    behaviour is unchanged.
//
// Returns false on EOF. `prompt` (POSIX only) is the visible prompt text,
// reprinted by the editor as the line changes; it must not contain escapes
// because its byte length is used as its width.
bool read_user_input(std::string& out, const std::string& prompt = "> ") {
    static const std::string kPasteStart = "\x1b[200~";
    static const std::string kPasteEnd   = "\x1b[201~";

    // Interactive terminals on POSIX get real line editing (arrows, history,
    // pastes), so the arrow keys' ESC sequences never reach the prompt text.
    // Piped or redirected stdin keeps the generic path below.
#ifndef _WIN32
    if (isatty(STDIN_FILENO)) {
        if (posix_read_line(prompt, out)) return true;
        return false; // EOF
    }
#else
    (void)prompt; // the console does its own echo and editing
#endif

#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD cmode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &cmode)) {
        // Interactive console: read cooked lines and coalesce a multi-line paste.
        bool got = false;
        if (!win_read_console_line(in, out, got) || !got) return false;
        while (win_char_input_pending(in)) {
            std::string more;
            if (!win_read_console_line(in, more, got) || !got) break;
            out += '\n';
            out += more;
        }
        return true;
    }
    // Redirected/piped stdin falls through to the generic getline path below.
#endif

    std::string line;
    if (!std::getline(std::cin, line)) return false;

    size_t start = line.find(kPasteStart);
    if (start != std::string::npos) {
        // Drop the start marker (keeping any text typed before it), then keep
        // reading until the end marker appears.
        line.erase(start, kPasteStart.size());
        while (line.find(kPasteEnd) == std::string::npos) {
            std::string more;
            if (!std::getline(std::cin, more)) break; // EOF mid-paste: use what we have
            line += '\n';
            line += more;
        }
        size_t end = line.find(kPasteEnd);
        if (end != std::string::npos) line.erase(end, kPasteEnd.size());
    }

    // Normalize CR / CRLF that some terminals use as paste line separators.
    out.clear();
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\r') {
            if (i + 1 < line.size() && line[i + 1] == '\n') continue; // CRLF -> LF
            out += '\n';                                              // lone CR -> LF
        } else {
            out += line[i];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// ESC interrupt
//
// While the model is running the main thread is free only at checkpoints
// (before each API call / tool execution), so the interrupt poll happens
// there, inline — no background thread needed. poll_esc() is installed on
// the CancellationToken via setCheckFn and called by the tool loop's check()
// at every checkpoint.
//
// Windows: _kbhit() / _getch() — non-blocking in cooked mode.
// POSIX:   select() with a zero timeout + read() — requires raw mode, which
//          is active for exactly the duration of a client->chat() call (the
//          RawTerminal above is active again only while a chat line is being
//          typed, when posix_read_line() does the line editing itself).
// ---------------------------------------------------------------------------
// Non-blocking poll for the interrupt key (ESC). Consumes and discards any
// other pending input (arrows, repeats, ...) so the queue never sticks.
bool poll_esc() {
#ifdef _WIN32
    bool esc = false;
    while (_kbhit()) {
        int c = _getch();
        if (c == 0x1B) {
            esc = true;
        } else if (c == 0x00 || c == 0xE0) {
            // Prefix for an extended key (arrows, F-keys): drop the
            // follow-up byte so the queue doesn't hold a partial sequence.
            if (_kbhit()) _getch();
        }
    }
    return esc;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv { 0, 0 }; // zero timeout: strictly non-blocking
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return false;
    unsigned char buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return false; // EOF or error — nothing to drain now
    for (ssize_t i = 0; i < n; ++i)
        if (buf[i] == 0x1B) return true;
    return false;
#endif
}

// `requested_provider` is the --provider argument, empty for the configured
// default. `resume` is --resume: continue the conversation saved for this
// folder instead of starting a new one.
int cmd_chat(const std::string& requested_provider, bool resume) {
    if (auto tf = get_effective("trace-file")) {
        mclog_set_file(*tf);
        // First line of every trace: which build wrote what follows. The file
        // is appended to across runs, so this also separates one from the next.
        mclog(std::string("tapto-code ") + TAPTO_CODE_VERSION + " (" +
              TAPTO_CODE_COMMIT + ")\n");
    }

    // Essentials: a provider and an API key for it. Prompt for whatever is
    // missing — but only for the default provider: a name given on the command
    // line is taken at face value, so a typo is reported rather than answered
    // with a setup wizard that would configure something else.
    const bool may_prompt = requested_provider.empty();
    bool ran_setup = false;
    if (may_prompt && !default_provider_name()) {
        if (!first_run_setup()) return 2; // setup prints its own errors
        ran_setup = true;
    }

    auto provider = resolve_provider(requested_provider); // prints its own errors
    if (!provider) return 2;

    if (!provider->api_key.ok() && provider->api_key.error.empty() && may_prompt && !ran_setup) {
        if (!first_run_setup()) return 2;
        provider = resolve_provider(requested_provider);
        if (!provider) return 2;
    }
    // A key that is configured but couldn't be read is reported as itself. It
    // must not fall through to the "none configured" path below, which would
    // send the user off to set a key they have already set.
    if (!provider->api_key.error.empty()) {
        ui::print_error("provider '" + provider->name + "': " + provider->api_key.error);
        return 2;
    }
    if (!provider->api_key.ok()) {
        std::string msg = "no API key for provider '" + provider->name + "'; set " +
                          api_key_env_var(provider->dialect) +
                          ", or run: tapto-code --global config set " + provider->name +
                          "-api-key <key>";
        if (auto def = default_provider_name(); def && *def != provider->name) {
            msg += "\n  (the unscoped api-key belongs to '" + *def +
                   "', so it is not used here)";
        }
        ui::print_error(msg);
        return 2;
    }

    const std::string& url = provider->url;
    const std::string& model = provider->model;

    // ai_config is declared before client so it outlives the client, which
    // holds a pointer to it.
    AiConfig ai_config;
    // Every request names the program, version and build, so a proxy in front
    // of the provider (LiteLLM, a gateway) can see who is calling and turn
    // away a version that is too old; its refusal is shown as the error.
    ai_config.setUserAgent(std::string("tapto-code/") + TAPTO_CODE_VERSION + " (" +
                           TAPTO_CODE_COMMIT + "; " +
#ifdef _WIN32
                           "windows"
#elif defined(__APPLE__)
                           "macos"
#else
                           "linux"
#endif
                           ")");
    if (auto v = get_effective("max-output-tokens")) {
        try {
            ai_config.setMaxOutputTokens(std::stoi(*v));
        } catch (const std::exception&) {
            ui::print_warning("invalid max-output-tokens '" + *v + "', using default");
        }
    }
    if (auto v = get_effective("max-tool-iterations")) {
        try {
            ai_config.setMaxToolIterations(std::stoi(*v));
        } catch (const std::exception&) {
            ui::print_warning("invalid max-tool-iterations '" + *v + "', using default");
        }
    }
    // HTTP timeouts, shared with tapto-word and tapto-vnc through the same
    // config store. read-timeout bounds the whole (unstreamed) generation, so
    // a local model that decodes slowly may need it raised well above the
    // hosted-provider default; a timeout shorter than the generation retries
    // the same request until the retry budget is gone.
    if (auto v = get_effective("connection-timeout")) {
        try {
            ai_config.setConnectionTimeoutSeconds(std::stoi(*v));
        } catch (const std::exception&) {
            ui::print_warning("invalid connection-timeout '" + *v + "', using default");
        }
    }
    if (auto v = get_effective("read-timeout")) {
        try {
            ai_config.setReadTimeoutSeconds(std::stoi(*v));
        } catch (const std::exception&) {
            ui::print_warning("invalid read-timeout '" + *v + "', using default");
        }
    }
    // The openai dialect reads this one; Claude's thinking depth is its own
    // knob (AiConfig::effort), left at the server default here.
    ai_config.setOpenaiReasoningEffort(provider->reasoning_effort);
    if (auto v = get_effective("print-cot")) {
        // Default is on; only "false"/"0"/"off"/"no" (case-insensitive) disable it.
        std::string s = *v;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        ai_config.setPrintCot(!(s == "false" || s == "0" || s == "off" || s == "no"));
    }
    // Chosen by dialect, never by name: 'gemma4' is a name this program has
    // never heard of, and what it means is whatever its -provider-type says.
    std::unique_ptr<AiBackend> client;
    if (provider->dialect == "claude") {
        client = std::make_unique<ClaudeClient>(&ai_config, url, model, provider->api_key.value);
    } else if (provider->dialect == "openai") {
        client = std::make_unique<OpenAIClient>(&ai_config, url, model, provider->api_key.value);
    } else {
        client = std::make_unique<GeminiClient>(&ai_config, url, model, provider->api_key.value);
    }

    client->start();

    // The file tools (editor + search) work inside the working directory. On
    // top of those the user can grant other folders with /add-folder: the
    // library's list_files / read_file / search_files then join the table, the
    // editor and find_files also reach into the ones granted read-write, and
    // the system prompt names what is granted. Tools and prompt both change
    // the request prefix, so they are rebuilt together and only on a change --
    // the cache misses once per grant, not per turn.
    //
    // The working directory is the set's home root, so once read_file and
    // friends are on the table they read the project as well as the grants,
    // and /list-folders shows where tapto-code was started. It is not a grant:
    // no command removes it or makes it read-only.
    // Declared before the context: the plugin tools in context.tools call into
    // it, so it must outlive them.
    Plugins plugins;
    Context context;
    FolderSet folders;
    {
        std::error_code ec;
        const std::string err = folders.set_home(std::filesystem::current_path(ec).string(),
                                                 /*writable=*/true);
        if (!err.empty()) ui::print_error("working directory: " + err);
    }
    // This session's folder for tool output too long to show (tapto/overflow.h),
    // reached by the model as @tmp, read-only, and removed when the chat ends.
    struct SessionTmp {
        ~SessionTmp() { close_session_tmp(); }
    } session_tmp;
    {
        std::string err;
        const std::filesystem::path tmp = open_session_tmp(err);
        if (!tmp.empty()) {
            err = folders.add_system("@tmp", tmp.string(), /*writable=*/false,
                                     "whole output of tool results too long to show");
        }
        if (err.empty()) {
            set_overflow_folder(tmp, "@tmp");
        } else {
            ui::print_warning("long tool output will be cut, not kept: " + err);
        }
    }
    set_granted_folders(&folders); // outlives the chat loop below
    // A plugin's own folder joins the set as @<plugin> (the memory's @memory).
    plugins.set_folders(&folders);
    // The plugins the configuration asks for, one `plugin.<name> = ro | rw |
    // off` each, the most specific level's value winning. One the policy sets
    // loads even with allow-user-plugins = 0.
    for (const auto& e : effective_config()) {
        const std::string name = plugin_of_key(e.key);
        if (name.empty() || e.value.empty()) continue; // empty counts as unset
        const auto mode = parse_plugin_mode(e.value);
        if (!mode) {
            ui::print_warning(e.key + " = " + e.value + ": not ro, rw or off, so not loaded");
            continue;
        }
        if (*mode == PluginMode::Off) continue;
        if (std::string why = plugins.add(name, *mode == PluginMode::Rw,
                                          e.origin == Level::Policy);
            !why.empty())
            ui::print_warning("plugin not loaded: " + why);
    }
    auto rebuild_tools_and_prompt = [&]() {
        context.tools = builtin_tools();
        if (folders.has_grants()) {
            for (auto& t : folder_tools(folders)) context.tools.push_back(std::move(t));
        }
        for (auto& t : plugins.tools()) context.tools.push_back(std::move(t));
        std::string prompt = resolve_system_prompt();
        if (const std::string extra = folder_prompt(folders); !extra.empty()) {
            prompt += "\n\n" + extra;
        }
        // What the plugins say of their tools, the changing part asked anew:
        // the memory plugin's index is read again, so a memory the model
        // writes mid-session is picked up at the next rebuild.
        if (const std::string extra = plugins.prompt(); !extra.empty()) {
            prompt += "\n\n" + extra;
        }
        client->setSystemPrompt(prompt);
    };
    rebuild_tools_and_prompt();

    // ESC-abort cancellation token (lives for the entire chat session). The
    // poll is installed once and runs inline in this thread at each tool-loop
    // checkpoint (no watcher thread).
    CancellationToken cancel_token;
    cancel_token.setCheckFn(poll_esc);

    // The resolved provider is printed, so a block wired to the wrong dialect or
    // URL shows up here rather than as malformed requests.
    ui::print_banner(TAPTO_CODE_VERSION, provider_label(*provider), model, url);

    // The conversation is saved after every turn, beside the folder's local
    // config, so a chat ended by accident (Ctrl-C, a closed window) can be
    // continued with --resume or /resume.
    //
    // `saved` is the conversation /resume brings back: the one on disk when
    // this session started, or the one /clear set aside. /resume swaps it with
    // the current one, so neither is lost and a second /resume goes back. The
    // file only follows the current conversation, from its next turn on.
    const std::filesystem::path session_file = session_path();
    std::optional<SavedSession> saved;
    {
        std::string err;
        saved = load_session(session_file, err);
        if (!err.empty()) ui::print_warning("saved conversation ignored: " + err);
        if (saved && saved->history.empty() && saved->pending_prompt.empty()) saved.reset();
    }
    bool save_warned = false;
    // `pending` is a prompt whose turn is under way: the history does not hold
    // it yet, so it is saved beside it and shown again on resume.
    // The current conversation, as it would be saved. `saved_at` stays empty:
    // save_session stamps what it writes, and one kept in memory is simply
    // "the previous conversation".
    auto snapshot = [&](const std::string& pending = std::string()) {
        SavedSession s;
        s.provider = provider->name;
        s.dialect = provider->dialect;
        s.model = model;
        for (const auto& f : folders.folders()) {
            if (!f.home && !f.system) s.folders.push_back({f.root.string(), f.writable});
        }
        for (const auto& p : plugins.loaded()) s.plugins.push_back({p.name, p.writable});
        s.pending_prompt = pending;
        s.history = client->getHistory();
        return s;
    };
    auto persist = [&](const std::string& pending = std::string()) {
        const std::string err = save_session(session_file, snapshot(pending));
        // Once is enough: the same failure would repeat on every turn.
        if (!err.empty() && !save_warned) {
            ui::print_warning("conversation not saved: " + err);
            save_warned = true;
        }
    };
    // The commands every tapto program has (tapto/chat_commands.h): /cot,
    // /effort, /clear, /compact and the folders. tapto-code keeps a cleared
    // conversation for /resume, saves the conversation when it changes, and
    // rebuilds the tools when the folders do.
    ChatCommands chat_commands;
    chat_commands.ai = &ai_config;
    chat_commands.client = client.get();
    chat_commands.folders = &folders;
    chat_commands.provider_name = provider->name;
    chat_commands.provider_dialect = provider->dialect;
    chat_commands.before_clear = [&]() -> std::string {
        saved = snapshot();
        return "; /resume brings it back";
    };
    chat_commands.history_changed = [&]() { persist(); };
    chat_commands.folders_changed = [&]() { rebuild_tools_and_prompt(); };
    chat_commands.progress = [](const std::string& text) { ui::print_line(text); };

    auto message_count = [](const nlohmann::json& history) {
        return std::to_string(history.size()) + (history.size() == 1 ? " message" : " messages");
    };
    auto resume_saved = [&]() {
        if (!saved) {
            ui::print_line(client->hasHistory() ? "(no other conversation to switch to)"
                                                : "(no saved conversation for this folder)");
            return;
        }
        // The history is in the wire format of the dialect that wrote it.
        if (saved->dialect != provider->dialect) {
            ui::print_error("the saved conversation was held with '" + saved->provider +
                            "', which speaks " + saved->dialect + "; '" + provider->name +
                            "' speaks " + provider->dialect +
                            ". Start with --provider " + saved->provider + " to resume it.");
            return;
        }
        for (const auto& g : saved->folders) {
            std::string label;
            const std::string err = folders.add(g.path, &label, g.writable);
            if (!err.empty()) {
                ui::print_warning("folder not granted again: " + g.path + ": " + err);
                continue;
            }
            // A folder granted already keeps its mode on add; widen it to the
            // saved one, never narrow what the user has granted since.
            if (g.writable) folders.set_writable(label, true);
        }
        for (const auto& p : saved->plugins) {
            // The same for a plugin: one loaded rw since stays rw.
            bool rw = p.writable;
            for (const auto& have : plugins.loaded()) {
                if (have.name == p.name && have.writable) rw = true;
            }
            if (std::string why = plugins.add(p.name, rw); !why.empty())
                ui::print_warning("plugin not loaded again: " + why);
        }
        rebuild_tools_and_prompt();
        // The conversation being replaced is set aside, not dropped: a second
        // /resume brings it back.
        std::optional<SavedSession> previous;
        if (client->hasHistory()) previous = snapshot();
        SavedSession next = std::move(*saved);
        saved = std::move(previous);

        client->start(); // resets the token accounting along with the history
        client->loadHistory(next.history);
        std::string msg = next.saved_at.empty()
            ? "(back to the previous conversation: " + message_count(next.history)
            : "(resumed the conversation saved " + next.saved_at + ": " +
                  message_count(next.history);
        if (next.model != model) msg += ", held with " + next.model;
        if (saved) msg += "; /resume again to switch back";
        msg += ")";
        if (folders.has_grants()) msg += "\n" + list_folders_text(folders);
        if (!next.pending_prompt.empty()) {
            // Shown the way the input would have been: prompt color, so it is
            // recognized as user text.
            msg += std::string("\n\x1b[33mThe last prompt had not been answered when the session "
                               "ended:\x1b[0m\n") +
                   kPromptColor + "> " + next.pending_prompt + kPromptReset;
        }
        // Where the conversation stood, as it looked: about a screenful of its
        // end, sized so that it and the notice below both stay in view.
        ui::print_transcript(transcript(next.history), msg);
        ui::print_line(msg);
        persist();
    };
    if (resume) {
        resume_saved();
    } else if (saved) {
        ui::print_line("(a conversation from " + saved->saved_at + " is saved for this folder, " +
                       message_count(saved->history) + "; /resume continues it)");
    }
#ifndef _WIN32
    // POSIX terminals deliver multi-line pastes via bracketed-paste markers.
    // On Windows read_user_input() coalesces pastes at the console API level, so
    // enabling this here would only risk conhost injecting the markers as text.
    std::cout << "\x1b[?2004h" << std::flush; // enable bracketed paste (multi-line input)
#endif
    std::string line;
    while (true) {
        // The marker carries the prompt color: on Windows the console echoes
        // the typed characters with the current SGR state, so the input comes
        // out in it (the POSIX editor echoes it itself, in kPromptColor). The
        // cursor is made visible first: it is hidden while the model works.
        ui::print_prompt(std::string("\x1b[?25h") + kPromptColor + "> ");
        if (!read_user_input(line, "> ")) {
            std::cout << "\x1b[0m" << std::flush; // stop the color after the input
            ui::print_line(""); // move past the prompt on EOF (Ctrl-D / Ctrl-Z)
            break;
        }
        // Back to the default style before anything else prints: the model's
        // reply and the status line must not inherit the prompt color.
        std::cout << "\x1b[0m" << std::flush;
        if (line == "/exit" || line == "/quit") break;
        if (line.empty()) continue;

        // Reset the conversation (e.g. to recover after filling the context window).
        // The cleared conversation is kept for /resume, and stays in the file
        // until the next turn replaces it, so --resume undoes a /clear too.
        if (auto r = run_chat_command(line, chat_commands)) {
            if (r->error) ui::print_error(r->text);
            else ui::print_line(r->text);
            continue;
        }

        // In-session command management. Newly added commands are immediately
        // runnable by the agent (run_command reads the store on each call).
        if (line == "/list-commands") {
            auto cmds = effective_commands();
            if (cmds.empty()) {
                ui::print_no_commands();
            } else {
                for (const auto& e : cmds) {
                    ui::print_command_entry(level_name(e.origin), e.name, e.command);
                }
            }
            continue;
        }
        if (line == "/resume") {
            resume_saved();
            continue;
        }
        if (line == "/help") {
            std::vector<std::string> tool_names;
            for (const auto& t : context.tools) tool_names.push_back(t.name);
            ui::print_help(tool_names);
            const auto commands = plugins.commands();
            if (!commands.empty()) {
                std::string text = "  Plugin commands:\n";
                for (const auto& c : commands)
                    text += "    " + c.usage + (c.description.empty() ? "" : "\n      " + c.description) + "\n";
                ui::print_line(text);
            }
            continue;
        }

        // Show the current session environment: where, with what, and how full.
        if (line == "/env") {
            auto hist = client->getHistory();
            size_t msg_count = hist.is_array() ? hist.size() : 0;
            // Rough turn estimate: each user+assistant exchange is 2 messages
            // (more when tool calls are interleaved); the first "user" message
            // in a /compact-seeded conversation is the summary, so we count it.
            size_t turns = (msg_count / 2) + (msg_count % 2 ? 1 : 0);
            // How full the model's context window is: the input-token count the
            // provider reported for the most recent request (system prompt +
            // history + current message). 0 until the first turn — or after a
            // reset (/clear, /compact) has cleared it — has run.
            size_t in_tokens = client->lastInputTokens();

            std::cout
                << "\x1b[1m"  << "  Directory"   << "\x1b[0m"
                << "  " << std::filesystem::current_path().string() << "\n"
                << "\x1b[1m"  << "  Provider"    << "\x1b[0m"
                << "  " << provider_label(*provider)
                << "  │  " << model << "\n"
                << "\x1b[1m"  << "  History"     << "\x1b[0m"
                << "  " << msg_count << " messages (~" << turns << " turns)\n"
                << "\x1b[1m"  << "  Context"     << "\x1b[0m"
                << "  " << (in_tokens == 0
                          ? std::string("— (no request yet)")
                          : std::to_string(in_tokens) + " input tokens") << "\n"
                << "\x1b[1m"  << "  Saved to"    << "\x1b[0m"
                << "  " << session_file.string() << "\n"
                << "\x1b[1m"  << "  Max tool it." << "\x1b[0m"
                << "  " << ai_config.maxToolIterations() << "\n"
                << "\x1b[1m"  << "  Version"     << "\x1b[0m"
                << "  " << TAPTO_CODE_VERSION << " (" << TAPTO_CODE_COMMIT << ")\n"
                << "\n";
            continue;
        }
        // Read-only folder access. The file tools stay pinned to the working
        // directory; these grant the model other folders to list, read and
        // search -- a library the project depends on, a sibling repository --
        // and nothing more. The labels the model addresses them by are shown
        // on grant and by /list-folders. Same commands and wording as
        // tapto-word.
        // Plugins: tools from the modules installed beside tapto-code
        // (tapto/plugins.h), read-only unless rw, worded like the folders.
        if (line == "/list-plugins") {
            ui::print_line(list_plugins_text(plugins));
            continue;
        }
        if (line.rfind("/add-plugin", 0) == 0 &&
            (line.size() == 11 || line[11] == ' ' || line[11] == '\t')) {
            // For this session by default; --project or --global also loads
            // it at every start there (plugin.<name> = ro | rw).
            const ScopeFlags flags = parse_scope_flags(line.substr(11));
            std::istringstream iss(flags.rest);
            std::string name, mode, extra;
            iss >> name >> mode >> extra;
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!flags.error.empty() || name.empty() ||
                !(mode.empty() || mode == "ro" || mode == "rw") || !extra.empty()) {
                ui::print_line((flags.error.empty() ? std::string() : flags.error + "\n") +
                               "usage: /add-plugin <name> [ro|rw] [--project|--global]   "
                               "(read-only unless rw; this session unless a scope)\n\n" +
                               list_plugins_text(plugins));
                continue;
            }
            const SettingScope scope = flags.scope.value_or(SettingScope::Session);
            const bool writable = mode == "rw";
            bool was_loaded = false, was_writable = false;
            for (const auto& e : plugins.loaded()) {
                if (e.name == name) {
                    was_loaded = true;
                    was_writable = e.writable;
                }
            }
            if (std::string why = plugins.add(name, writable); !why.empty()) {
                ui::print_error(why);
                continue;
            }
            // Where it loads from now on, when a scope says so.
            std::string saved_where;
            if (scope != SettingScope::Session) {
                const SettingWrite w =
                    write_setting("plugin." + name, writable ? "rw" : "ro", scope);
                saved_where = w.ok ? " It loads at every start (" + w.text + ")."
                                   : " It is not saved: " + w.text + ".";
            }
            if (was_loaded && was_writable == writable) {
                ui::print_line("'" + name + "' is loaded already (" +
                               (writable ? "read-write" : "read-only") + ")." + saved_where);
                continue;
            }
            rebuild_tools_and_prompt();
            persist();
            for (const auto& e : plugins.loaded()) {
                if (e.name != name) continue;
                const std::size_t offered = writable ? e.tools : e.tools - e.write_tools;
                std::string tools;
                for (const auto& t : context.tools) {
                    if (t.name.rfind(name + "_", 0) == 0)
                        tools += (tools.empty() ? "" : ", ") + t.name;
                }
                std::string msg = was_loaded
                    ? "'" + name + "' is now " + (writable ? "read-write" : "read-only") + ": "
                    : "Loaded plugin '" + name + "'" + (e.version.empty() ? "" : " " + e.version) +
                          ", " + (writable ? "read-write" : "read-only") + ": ";
                msg += std::to_string(offered) + (offered == 1 ? " tool" : " tools") +
                       (tools.empty() ? std::string() : " (" + tools + ")") + ".";
                if (!writable && e.write_tools) {
                    msg += " Its " + std::to_string(e.write_tools) + " that change things " +
                           (e.write_tools == 1 ? "is" : "are") + " not offered; /add-plugin " +
                           name + " rw offers them.";
                }
                if (!was_loaded) msg += " Remove it with /remove-plugin " + name + ".";
                ui::print_line(msg + saved_where);
            }
            continue;
        }
        if (line.rfind("/remove-plugin", 0) == 0) {
            // For this session by default; --project or --global also stops it
            // loading at every start there.
            const ScopeFlags flags =
                parse_scope_flags(line.substr(std::string("/remove-plugin").size()));
            const std::string name = flags.rest;
            if (!flags.error.empty() || name.empty() || name.find_first_of(" \t") != std::string::npos) {
                ui::print_line((flags.error.empty() ? std::string() : flags.error + "\n") +
                               "usage: /remove-plugin <name> [--project|--global]\n\n" +
                               list_plugins_text(plugins));
                continue;
            }
            const SettingScope scope = flags.scope.value_or(SettingScope::Session);
            std::string saved_where;
            if (scope != SettingScope::Session) {
                const std::string key = "plugin." + name;
                SettingWrite w = write_setting(key, std::nullopt, scope);
                // Still loaded from a less specific scope: in a project, `off`
                // there is what undoes it; globally, the reply says where.
                const auto still = get_effective(key);
                const bool loads = still && parse_plugin_mode(*still) &&
                                   *parse_plugin_mode(*still) != PluginMode::Off;
                if (loads && scope == SettingScope::Project) w = write_setting(key, "off", scope);
                if (w.ok || !loads) {
                    saved_where = loads && scope == SettingScope::Global
                                      ? " It still loads at start: the " + setting_origin(key) +
                                            " has " + key + " = " + *still + "."
                                      : " It no longer loads at start from the " +
                                            std::string(scope == SettingScope::Global
                                                            ? "global config."
                                                            : "project config.");
                } else {
                    saved_where = " Not changed in the config: " + w.text + ".";
                }
            }
            // Its tools leave the table before the plugin is shut down.
            std::vector<ToolSpec> kept;
            for (auto& t : context.tools) {
                if (t.name.rfind(name + "_", 0) != 0) kept.push_back(std::move(t));
            }
            context.tools = std::move(kept);
            bool unloaded = false;
            if (!plugins.remove(name, &unloaded)) {
                rebuild_tools_and_prompt();
                ui::print_line("'" + name + "' is not loaded." + saved_where + "\n\n" +
                               list_plugins_text(plugins));
                continue;
            }
            rebuild_tools_and_prompt();
            persist();
            ui::print_line("Removed plugin '" + name + "'; its tools are gone." +
                           (unloaded ? std::string()
                                     : std::string(" (Its module stays loaded until tapto-code "
                                                   "exits.)")) +
                           saved_where);
            continue;
        }

        if (line.rfind("/add-command", 0) == 0) {
            const ScopeFlags flags = parse_scope_flags(
                line.substr(std::string("/add-command").size()), /*leading_only=*/true);
            if (!flags.error.empty() || flags.scope == SettingScope::Session) {
                ui::print_line(flags.error.empty()
                                   ? "A command is saved, in the project's list or with --global "
                                     "in every project's; there is no command for this session "
                                     "only."
                                   : flags.error);
                continue;
            }
            const bool global_command = flags.scope == SettingScope::Global;
            std::istringstream iss("/add-command " + flags.rest);
            std::string slash, name;
            iss >> slash >> name;
            // --ro marks the command [ro]: it only reads (tapto/commands.h).
            const bool reads_only = name == "--ro";
            if (reads_only) iss >> name;
            std::string remainder;
            std::getline(iss, remainder);
            // A trailing --note "..." is a hint for the model; it comes after
            // the command, so the note's text can itself start with anything.
            std::string note;
            const std::size_t note_pos = remainder.rfind(" --note ");
            if (note_pos != std::string::npos) {
                std::string rest = remainder.substr(note_pos + std::string(" --note ").size());
                remainder = remainder.substr(0, note_pos);
                const std::size_t open = rest.find('"');
                if (open != std::string::npos) {
                    const std::size_t close = rest.find('"', open + 1);
                    if (close != std::string::npos) {
                        note = rest.substr(open + 1, close - open - 1);
                    }
                }
            }
            size_t begin = remainder.find_first_not_of(" \t");
            if (name.empty() || name == "--ro" || begin == std::string::npos) {
                ui::print_line("usage: /add-command [--global] [--ro] <name> <command...> "
                               "[--note \"...\"]");
                continue;
            }
            if (name.find('=') != std::string::npos) {
                ui::print_line("error: command name must not contain '='");
                continue;
            }
            if (tapto::is_builtin_command(name)) {
                ui::print_line("error: '" + name + "' is a reserved built-in command");
                continue;
            }
            if (std::string why = command_policy_refusal(name); !why.empty()) {
                ui::print_line("error: " + why);
                continue;
            }
            std::string cmdline = remainder.substr(begin);
            if (reads_only && !command_reads_only(cmdline)) cmdline = std::string(kReadOnlyMark) + " " + cmdline;
            if (!note.empty()) cmdline += " # " + note;
            if (std::string why = command_line_refusal(cmdline); !why.empty()) {
                ui::print_line("error: " + why);
                continue;
            }
            try {
                add_command(global_command ? Level::Global : Level::Local, name, cmdline);
                ui::print_command_added(name, global_command ? "global" : "local", cmdline);
            } catch (const std::exception& e) {
                ui::print_line("error: " + std::string(e.what()));
            }
            continue;
        }

        // A plugin's own command, /<plugin> ...: after every command of the
        // core's, so it can shadow none. Its text is the user's, never the
        // model's.
        if (line.size() > 1 && line[0] == '/') {
            const std::size_t sp = line.find_first_of(" \t");
            const std::string cmd = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
            const auto commands = plugins.commands();
            if (std::any_of(commands.begin(), commands.end(),
                            [&](const auto& c) { return c.name == cmd; })) {
                cancel_token.reset();
                context.cancel = &cancel_token;
                std::optional<std::string> text;
                {
                    // ESC cancels a command as it does a turn (a login waits
                    // for the browser).
                    RawTerminal raw;
                    text = plugins.run_command(
                        cmd, sp == std::string::npos ? std::string() : trim(line.substr(sp)), context);
                }
                ui::print_line(text ? *text : "'/" + cmd + "' is no command now.");
                rebuild_tools_and_prompt();
                continue;
            }
        }

        // A plugin's own settings edited since the last turn (the mcp
        // plugin's servers file): taken in before the turn starts, since a
        // turn keeps the tools it starts with.
        {
            bool changed = false;
            if (const std::string note = plugins.before_turn(context, changed); !note.empty())
                ui::print_line(note);
            if (changed) rebuild_tools_and_prompt();
        }

        ui::print_prompt_accepted();

        // Reset the cancellation state for this turn. The ESC poll runs in
        // this thread at each tool-loop checkpoint (see CancellationToken).
        cancel_token.reset(); // reset for this turn
        context.cancel = &cancel_token;

        // Saved before the call so a session killed mid-turn still has the
        // prompt, and after it with the turn in the history.
        persist(line);

        {
            // Raw/unbuffered stdin for the duration of the call so poll_esc()
            // can read a single ESC byte without waiting for Enter (POSIX).
            // Restored before the next prompt, which needs cooked mode.
            RawTerminal raw;
            try {
                std::string reply = client->chat(context, line);
                if (cancel_token.cancelled()) {
                    ui::print_line("\x1b[33m<interrupted by user — conversation preserved, type a new instruction>\x1b[0m");
                } else {
                    ui::print_reply(reply);
                }
            } catch (const std::exception& e) {
                if (cancel_token.cancelled()) {
                    ui::print_line("\x1b[33m<interrupted by user — conversation preserved, type a new instruction>\x1b[0m");
                } else {
                    ui::print_error(e.what());
                }
            }
        }
        // A turn keeps the tools it started with; one that changed a plugin's
        // (an MCP login) shows in the next.
        if (plugins.take_tools_changed()) rebuild_tools_and_prompt();
        persist();
    }
#ifndef _WIN32
    std::cout << "\x1b[?2004l" << std::flush; // disable bracketed paste on exit
#endif
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    tapto::set_program_name("tapto-code"); // in what the library says
    enable_console_features();

    // The `command` subcommand is parsed specially: `command add` takes its
    // command line verbatim (it may contain tokens like --build or --config),
    // so we must not run it through the general flag extraction below. Only a
    // leading scope flag (before "command") selects the scope.
    {
        std::vector<std::string> raw(argv + 1, argv + argc);
        std::optional<Level> level;
        size_t i = 0;
        for (; i < raw.size(); ++i) {
            if (raw[i] == "--system") level = Level::System;
            else if (raw[i] == "--global") level = Level::Global;
            else if (raw[i] == "--local") level = Level::Local;
            else if (raw[i] == "--policy") level = Level::Policy; // list only; add/remove refuse it
            else break;
        }
        if (i < raw.size() && raw[i] == "command") {
            std::vector<std::string> sub(raw.begin() + i + 1, raw.end());
            return cmd_command(level, sub);
        }
    }

    Args a;
    std::vector<std::string> rest;
    std::string provider; // --provider <name>, empty for the configured default
    bool resume = false;  // --resume

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--system") a.level = Level::System;
        else if (arg == "--global") a.level = Level::Global;
        else if (arg == "--local") a.level = Level::Local;
        else if (arg == "--policy") a.level = Level::Policy;
        else if (arg == "--show-origin") a.show_origin = true;
        else if (arg == "-h" || arg == "--help") { ui::print_usage(kUsage); return 0; }
        else if (arg == "--provider") {
            if (i + 1 >= argc) {
                ui::print_error("--provider requires a name");
                return 2;
            }
            provider = argv[++i];
        }
        else if (arg.rfind("--provider=", 0) == 0) provider = arg.substr(11);
        else if (arg == "--resume") resume = true;
        else rest.push_back(std::move(arg));
    }

    // No subcommand: start a chat (it's the default action).
    if (rest.empty()) {
        return cmd_chat(provider, resume);
    }
    if (!provider.empty()) {
        ui::print_error("--provider only applies to chat");
        return 2;
    }
    if (resume) {
        ui::print_error("--resume only applies to chat");
        return 2;
    }
    const std::string& top = rest[0];
    if (top == "version") return cmd_version();
    if (top != "config") {
        ui::print_error("unknown command '" + top + "'");
        return 2;
    }

    a.positional.assign(rest.begin() + 1, rest.end());
    if (a.positional.empty()) {
        ui::print_usage(kUsage);
        return 2;
    }

    const std::string& sub = a.positional[0];
    if (sub == "set")   return cmd_set(a);
    if (sub == "get")   return cmd_get(a);
    if (sub == "unset") return cmd_unset(a);
    if (sub == "list")  return cmd_list(a);

    ui::print_error("unknown config command '" + sub + "'");
    return 2;
}
