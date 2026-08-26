// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/commands.h"
#include "tapto/config.h"
#include "tapto/paths.h"
#include "tapto/secret.h"
#include "tapto/tools.h"

#include "tapto/claude.h"
#include "tapto/openai.h"
#include "tapto/gemini.h"
#include "tapto/aiconfig.h"
#include "tapto/cancel.h"
#include "tapto/log.h"
#include "tapto/ui.h"
#include "tapto/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

const char* kUsage =
    "tapto-code - a small cross-platform CLI\n"
    "\n"
    "Usage:\n"
    "  tapto-code [--provider <name>]                Start an interactive chat (default)\n"
    "  tapto-code [--system|--global|--local] config <command> [args]\n"
    "  tapto-code [--system|--global|--local] command <add|remove|list> ...\n"
    "\n"
    "Config commands:\n"
    "  set <key> <value>   Set a config value (default scope: local)\n"
    "  get <key>           Print the effective value of a key\n"
    "  unset <key>         Remove a key (default scope: local)\n"
    "  list                List config values\n"
    "\n"
    "Command commands (allow-listed commands the chat agent may run):\n"
    "  command add <name> <command...>   Allow-list a command (default scope: local)\n"
    "  command remove <name>             Remove a command\n"
    "  command list                      List configured commands\n"
    "\n"
    "Other commands:\n"
    "  version             Print version info as JSON\n"
    "\n"
    "Chat config keys: provider (which provider block to use), max-output-tokens\n"
    "  (optional), max-tool-iterations (optional, default 200), print-cot\n"
    "  (optional, default true), system-prompt, trace-file, and per provider\n"
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
    "  --local    per-folder config (./.tapto); the default for writes\n"
    "\n"
    "Options:\n"
    "  --provider <name>  chat with this provider instead of the configured default\n"
    "  --show-origin      with 'list', prefix each entry with its scope\n"
    "  -h, --help         show this help\n"
    "\n"
    "Precedence (highest wins): local > global > system\n";

struct Args {
    std::optional<Level> level;
    bool show_origin = false;
    std::vector<std::string> positional; // [0]=subcommand, [1]=key, [2]=value
};

struct EffectiveEntry {
    std::string key;
    std::string value;
    Level origin;
};

// Merge all scopes lowest-to-highest so later scopes override earlier ones,
// while preserving first-seen ordering of keys.
std::vector<EffectiveEntry> effective_config() {
    std::vector<EffectiveEntry> merged;

    auto apply = [&](Level level) {
        Config cfg = Config::load(config_path(level));
        for (const auto& entry : cfg.entries()) {
            bool found = false;
            for (auto& existing : merged) {
                if (existing.key == entry.first) {
                    existing.value = entry.second;
                    existing.origin = level;
                    found = true;
                    break;
                }
            }
            if (!found) merged.push_back({entry.first, entry.second, level});
        }
    };

    apply(Level::System);
    apply(Level::Global);
    apply(Level::Local);
    return merged;
}

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
        "max-output-tokens", "max-tool-iterations", "system-prompt", "trace-file", "print-cot",
        "reasoning-effort",
    };
    for (const char* k : kKeys) {
        if (key == k) return true;
    }
    for (const char* k : kProviderBlockKeys) {
        if (is_valid_provider_name(provider_name_of_key(key, k))) return true;
    }
    return false;
}

const char* kSupportedKeysHelp =
    "provider, provider-url, model, api-key, "
    "max-output-tokens, max-tool-iterations, system-prompt, trace-file, print-cot, "
    "and per provider <name>-provider-type, <name>-provider-url, <name>-model, "
    "<name>-api-key, <name>-reasoning-effort";

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
    if (!is_supported_config_key(a.positional[1])) {
        ui::print_error("unknown config key '" + a.positional[1] +
                        "'.\n  supported keys: " + kSupportedKeysHelp);
        return 2;
    }
    Level level = a.level.value_or(Level::Local);
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
    }
    return 0;
}

int cmd_get(const Args& a) {
    if (a.positional.size() < 2) {
        ui::print_error("'get' requires <key>");
        return 2;
    }
    const std::string& key = a.positional[1];

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
std::string list_value(const std::string& key, const std::string& value) {
    return (is_api_key_key(key) && !is_secret_reference(value)) ? mask_secret(value) : value;
}

int cmd_list(const Args& a) {
    if (a.level) {
        Config cfg = Config::load(config_path(*a.level));
        for (const auto& entry : cfg.entries()) {
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
int cmd_command(std::optional<Level> level, const std::vector<std::string>& rest) {
    if (rest.empty()) {
        ui::print_usage("usage: tapto-code [--scope] command <add|remove|list> ...\n");
        return 2;
    }
    const std::string& sub = rest[0];

    if (sub == "add") {
        if (rest.size() < 3) {
            ui::print_error("'command add' requires <name> <command...>");
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
        for (size_t i = 2; i < rest.size(); ++i) {
            if (i > 2) cmdline += ' ';
            cmdline += rest[i];
        }
        Level lvl = level.value_or(Level::Local);
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

// Look up a config key's effective value across all scopes. A key present but
// empty counts as unset, so `model =` falls back to the default instead of
// asking the provider for a model with no name.
std::optional<std::string> get_effective(const std::string& key) {
    for (const auto& entry : effective_config()) {
        if (entry.key == key) {
            if (entry.value.empty()) return std::nullopt;
            return entry.value;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Providers
//
// A provider has a *name* and a *dialect*, and they are not the same thing.
// The name selects a block of config keys and is free-form — qwen36, gemma4,
// work-claude. The dialect is one of the three request shapes this program can
// speak, named by that block's `<name>-provider-type` key:
//
//   qwen36-provider-type = openai         gemma4-provider-type = openai
//   qwen36-provider-url  = http://a:8000  gemma4-provider-url  = http://b:8081
//   qwen36-model         = Qwen3-VL-30B   gemma4-model         = gemma-3-27b
//   qwen36-api-key       = local          gemma4-api-key       = local
//
// Two local servers speaking the same API can therefore be told apart, which
// they could not when the name *was* the dialect and one store held at most one
// configuration per vendor. The config store is shared with tapto-vnc, which
// reads the same blocks.
// ---------------------------------------------------------------------------

// The request shapes this program can speak. Used as a provider name, each one
// means its own dialect with that vendor's defaults, so `claude`, `openai` and
// `gemini` need no block at all.
bool is_dialect(const std::string& s) {
    return s == "claude" || s == "openai" || s == "gemini";
}

std::string default_url(const std::string& dialect) {
    if (dialect == "claude") return "https://api.anthropic.com";
    if (dialect == "openai") return "https://api.openai.com";
    if (dialect == "gemini") return "https://generativelanguage.googleapis.com";
    return "";
}

std::string default_model(const std::string& dialect) {
    if (dialect == "claude") return "claude-sonnet-4-6";
    if (dialect == "openai") return "gpt-4o";
    if (dialect == "gemini") return "gemini-2.0-flash";
    return "";
}

const char* api_key_env_var(const std::string& dialect) {
    if (dialect == "claude") return "ANTHROPIC_API_KEY";
    if (dialect == "openai") return "OPENAI_API_KEY";
    if (dialect == "gemini") return "GEMINI_API_KEY";
    return "";
}

std::optional<std::string> env_value(const char* name) {
    if (!name || !*name) return std::nullopt;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // std::getenv is the portable, intended call here
#endif
    const char* v = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (v && *v) return std::string(v);
    return std::nullopt;
}

// Every provider block the store defines, found by its `<name>-provider-type`
// key. Only used to name the alternatives when someone asks for a provider that
// isn't configured — a list of what exists is worth more than a list of what is
// allowed.
std::vector<std::string> configured_provider_names() {
    std::vector<std::string> names;
    for (const auto& entry : effective_config()) {
        if (entry.value.empty()) continue; // an empty value counts as unset
        std::string name = provider_name_of_key(entry.key, "provider-type");
        if (!name.empty()) names.push_back(std::move(name));
    }
    return names;
}

// The provider used when none is named on the command line. `provider-type`
// doubles as the legacy spelling: a store saying `provider-type = claude` names
// the block "claude", whose dialect is claude because that is also a dialect
// name, so nothing needs rewriting.
std::optional<std::string> default_provider_name() {
    if (auto v = get_effective("provider")) return v;
    return get_effective("provider-type");
}

// The dialect a provider name resolves to: its block's `-provider-type`, or the
// name itself when that is a dialect. Empty when the name isn't configured.
std::string provider_dialect(const std::string& name) {
    if (auto v = get_effective(name + "-provider-type")) return *v;
    return is_dialect(name) ? name : "";
}

// Resolve the API key for a provider block. The block's own key comes first: it
// is the most specific thing the user wrote, it is the only thing that can be
// right when two blocks share a dialect, and — more sharply — an environment
// variable winning here would send a real vendor key to whatever
// `<name>-provider-url` points at, which for a local server means writing it
// into somebody's log. The plaintext warning is emitted when the key is
// written, not on use.
//
// A configured value may name where the key lives — `env:`, `cmd:`, `wincred:`
// — instead of being the key; see tapto/secret.h. The vendor environment
// variable is a secret in its own right, never a reference, so it is taken
// verbatim.
Secret resolve_api_key(const std::string& name, const std::string& dialect) {
    if (auto v = get_effective(name + "-api-key")) return resolve_secret(*v);
    if (auto v = env_value(api_key_env_var(dialect))) {
        Secret s;
        s.value = *v;
        return s;
    }
    // The unscoped api-key belongs to the default provider only; otherwise one
    // vendor's key would be handed to another.
    if (auto def = default_provider_name(); def && *def == name) {
        if (auto v = get_effective("api-key")) return resolve_secret(*v);
    }
    return Secret{};
}

// A provider block resolved into everything a chat session needs.
struct ResolvedProvider {
    std::string name;    // the config block, e.g. "qwen36"
    std::string dialect; // claude | openai | gemini
    std::string url;
    std::string model;
    std::string reasoning_effort; // empty when unset; openai dialect only
    Secret api_key;      // unresolved if none is configured; the caller decides
};

// Resolve a provider name (empty for the configured default). Prints its own
// error and returns nullopt when the name names no dialect this program speaks.
std::optional<ResolvedProvider> resolve_provider(const std::string& requested) {
    const std::string def = default_provider_name().value_or("claude");

    ResolvedProvider p;
    p.name = requested.empty() ? def : requested;
    p.dialect = provider_dialect(p.name);

    if (p.dialect.empty()) {
        std::string msg = "unknown provider '" + p.name + "'";
        auto names = configured_provider_names();
        if (!names.empty()) {
            msg += "; configured:";
            for (const auto& n : names) msg += " " + n;
        }
        msg += ".\n  name one by setting '" + p.name +
               "-provider-type' to claude, openai or gemini, "
               "or use claude, openai or gemini directly";
        ui::print_error(msg);
        return std::nullopt;
    }
    if (!is_dialect(p.dialect)) {
        ui::print_error("'" + p.name + "-provider-type' is '" + p.dialect +
                        "'; expected claude, openai or gemini."
                        "\n  that key names the API shape to speak, not the model");
        return std::nullopt;
    }

    // The unscoped keys belong to the default provider only. Otherwise a local
    // endpoint's URL and model would be sent to a hosted vendor, and vice versa.
    const bool is_default = (p.name == def);
    auto scoped = [&](const std::string& key) -> std::optional<std::string> {
        if (auto v = get_effective(p.name + "-" + key)) return v;
        if (is_default) return get_effective(key);
        return std::nullopt;
    };

    p.url = scoped("provider-url").value_or(default_url(p.dialect));
    p.model = scoped("model").value_or(default_model(p.dialect));
    // The openai dialect is the only one that sends this. On a block speaking
    // another dialect it is a mistake worth reporting, not a silent no-op.
    p.reasoning_effort = scoped("reasoning-effort").value_or("");
    if (!p.reasoning_effort.empty() && p.dialect != "openai") {
        ui::print_warning("'" + p.name + "-reasoning-effort' is ignored: only the "
                          "openai dialect sends it");
        p.reasoning_effort.clear();
    }
    p.api_key = resolve_api_key(p.name, p.dialect);
    return p;
}

// How the provider is shown to the user: the block name, plus the dialect when
// it adds something the name doesn't already say.
std::string provider_label(const ResolvedProvider& p) {
    if (p.name == p.dialect) return p.name;
    return p.name + " (" + p.dialect + ")";
}

const char* kDefaultSystemPrompt =
    "You are an experienced fullstack developer in a chat with a user. "
    "The user has started a session in a working folder. You have tools to "
    "view and edit files in that folder, search files, and run a set of "
    "pre-approved commands (use list_commands to see them and run_command to "
    "run them).";

// The effective system prompt: whatever is configured, else the built-in
// default used in-memory. Chat never persists anything — persisting the default
// is the job of `tapto-code install`.
std::string resolve_system_prompt() {
    return get_effective("system-prompt").value_or(kDefaultSystemPrompt);
}

// Trim surrounding whitespace (incl. a trailing CR from piped CRLF input).
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Write a key to the global (user) config scope.
bool set_global(const std::string& key, const std::string& value) {
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
// Returns false on EOF.
bool read_user_input(std::string& out) {
    static const std::string kPasteStart = "\x1b[200~";
    static const std::string kPasteEnd   = "\x1b[201~";

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
//          is active for exactly the duration of a client->chat() call (see
//          RawTerminal below); it must be off while read_user_input() runs,
//          since that relies on cooked line mode and echo.
// ---------------------------------------------------------------------------
// RAII terminal guard: unbuffered, echo-off stdin for the ESC poll.
struct RawTerminal {
    RawTerminal() {
#ifndef _WIN32
        if (tcgetattr(STDIN_FILENO, &m_saved) != 0) return;
        m_raw = m_saved;
        m_raw.c_lflag &= ~(ICANON | ECHO);
        m_raw.c_cc[VMIN]  = 0;
        m_raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &m_raw) == 0)
            m_active = true;
#else
        (void)this; // _kbhit works in cooked mode; nothing to do
#endif
    }
    ~RawTerminal() {
#ifndef _WIN32
        if (m_active) tcsetattr(STDIN_FILENO, TCSANOW, &m_saved);
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
// default.
int cmd_chat(const std::string& requested_provider) {
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
    client->setSystemPrompt(resolve_system_prompt());

    // Register the file tools (editor + search) for this chat session.
    Context context;
    context.tools = builtin_tools();

    // ESC-abort cancellation token (lives for the entire chat session). The
    // poll is installed once and runs inline in this thread at each tool-loop
    // checkpoint (no watcher thread).
    CancellationToken cancel_token;
    cancel_token.setCheckFn(poll_esc);

    // The resolved provider is printed, so a block wired to the wrong dialect or
    // URL shows up here rather than as malformed requests.
    ui::print_banner(TAPTO_CODE_VERSION, provider_label(*provider), model, url);
#ifndef _WIN32
    // POSIX terminals deliver multi-line pastes via bracketed-paste markers.
    // On Windows read_user_input() coalesces pastes at the console API level, so
    // enabling this here would only risk conhost injecting the markers as text.
    std::cout << "\x1b[?2004h" << std::flush; // enable bracketed paste (multi-line input)
#endif
    std::string line;
    while (true) {
        ui::print_prompt("\x1b[?25h> "); // ensure cursor is visible at the prompt
        if (!read_user_input(line)) {
            ui::print_line(""); // move past the prompt on EOF (Ctrl-D / Ctrl-Z)
            break;
        }
        if (line == "/exit" || line == "/quit") break;
        if (line.empty()) continue;

        // Reset the conversation (e.g. to recover after filling the context window).
        if (line == "/clear") {
            client->start();
            ui::print_line("(conversation cleared)");
            continue;
        }

        // Compact the conversation: ask the model to summarize the discussion
        // so far, then restart from that summary. Optional focus hint:
        //   /compact keep the CMake changes in mind
        if (line.rfind("/compact", 0) == 0) {
            if (!client->hasHistory()) {
                ui::print_line("(nothing to compact — the conversation is empty)");
                continue;
            }
            std::istringstream ciss(line);
            std::string ccmd, focus;
            ciss >> ccmd;
            std::getline(ciss, focus);
            size_t cfirst = focus.find_first_not_of(" \t");
            focus = (cfirst == std::string::npos) ? std::string() : focus.substr(cfirst);

            // Tool-free context so the model summarizes instead of editing files.
            Context summarize_ctx;
            std::string prompt =
                "Summarize the entire conversation so far as a compact, structured note: "
                "the user's goals, decisions made, files read or modified, and the current "
                "state of any in-progress task. Reply with the summary only. " +
                (focus.empty() ? std::string() : "Emphasize: " + focus);
            // Snapshot the current conversation so a failed or empty
            // summarization can be rolled back instead of throwing the session
            // away: compaction must never leave you with "nothing."
            auto before = client->getHistory();
            std::string summary;
            try {
                ui::print_line("(summarizing conversation…)");
                summary = client->chat(summarize_ctx, prompt);
            } catch (const std::exception& e) {
                // Restore the untouched conversation — the summarize request was
                // made in-turn, so a failure may have appended stray messages.
                client->loadHistory(before);
                mclog(std::string("[/compact] FAILED: ") + e.what() + "\n");
                ui::print_error(std::string("compaction failed: ") + e.what());
                continue;
            }
            // Dump the raw model output to the trace file (when one is set) so
            // the exact summary — empty or not — can be inspected while
            // debugging the next couple of runs.
            mclog("[/compact] raw summary (" + std::to_string(summary.size()) + " bytes):\n"
                   + summary + "\n[/compact] --- end of raw summary ---\n");
            // Guard against a missing/empty summary. Thinking models (Claude,
            // Gemini) may return the summary inside their reasoning/thought
            // blocks, which the text-only reply extraction drops — that would
            // reseed the conversation with an empty note and the model would
            // "know" nothing of the previous session. Never discard a real
            // session for an empty summary: keep what we had and say so.
            {
                size_t first = summary.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) {
                    client->loadHistory(before);
                    mclog("[/compact] empty summary — original conversation kept\n");
                    ui::print_error("compaction produced no summary — conversation kept as is.");
                    continue;
                }
            }
            client->beginWithSummary(summary);
            mclog("[/compact] reseeded the conversation with a " + std::to_string(summary.size()) + "-byte summary\n");
            ui::print_line("(conversation compacted)");
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
        if (line == "/help") {
            std::vector<std::string> tool_names;
            for (const auto& t : context.tools) tool_names.push_back(t.name);
            ui::print_help(tool_names);
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

            std::cout
                << "\x1b[1m"  << "  Directory"   << "\x1b[0m"
                << "  " << std::filesystem::current_path().string() << "\n"
                << "\x1b[1m"  << "  Provider"    << "\x1b[0m"
                << "  " << provider_label(*provider)
                << "  │  " << model << "\n"
                << "\x1b[1m"  << "  History"     << "\x1b[0m"
                << "  " << msg_count << " messages (~" << turns << " turns)\n"
                << "\x1b[1m"  << "  Max tool it." << "\x1b[0m"
                << "  " << ai_config.maxToolIterations() << "\n"
                << "\x1b[1m"  << "  Version"     << "\x1b[0m"
                << "  " << TAPTO_CODE_VERSION << " (" << TAPTO_CODE_COMMIT << ")\n"
                << "\n";
            continue;
        }
        if (line.rfind("/add-command", 0) == 0) {
            std::istringstream iss(line);
            std::string slash, name;
            iss >> slash >> name;
            std::string remainder;
            std::getline(iss, remainder);
            size_t begin = remainder.find_first_not_of(" \t");
            if (name.empty() || begin == std::string::npos) {
                ui::print_line("usage: /add-command <name> <command...>");
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
            std::string cmdline = remainder.substr(begin);
            try {
                add_command(Level::Local, name, cmdline);
                ui::print_command_added(name, "local", cmdline);
            } catch (const std::exception& e) {
                ui::print_line("error: " + std::string(e.what()));
            }
            continue;
        }

        ui::print_prompt_accepted();

        // Reset the cancellation state for this turn. The ESC poll runs in
        // this thread at each tool-loop checkpoint (see CancellationToken).
        cancel_token.reset(); // reset for this turn
        context.cancel = &cancel_token;

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
    }
#ifndef _WIN32
    std::cout << "\x1b[?2004l" << std::flush; // disable bracketed paste on exit
#endif
    return 0;
}

} // namespace

int main(int argc, char** argv) {
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--system") a.level = Level::System;
        else if (arg == "--global") a.level = Level::Global;
        else if (arg == "--local") a.level = Level::Local;
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
        else rest.push_back(std::move(arg));
    }

    // No subcommand: start a chat (it's the default action).
    if (rest.empty()) {
        return cmd_chat(provider);
    }
    if (!provider.empty()) {
        ui::print_error("--provider only applies to chat");
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
