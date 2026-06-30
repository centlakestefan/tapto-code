// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/commands.h"
#include "tapto/config.h"
#include "tapto/paths.h"
#include "tapto/tools.h"

#include "tapto/claude.h"
#include "tapto/openai.h"
#include "tapto/gemini.h"
#include "tapto/aiconfig.h"
#include "tapto/log.h"
#include "tapto/ui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace tapto;

#ifndef TAPTO_VERSION
#define TAPTO_VERSION "0.0.0-dev"
#endif

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
    "  tapto-code                                    Start an interactive chat (default)\n"
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
    "Chat config keys: provider-type (claude|openai|gemini), api-key,\n"
    "  provider-url (optional), model (optional), max-output-tokens (optional),\n"
    "  print-cot (optional, default true)\n"
    "\n"
    "Scope flags:\n"
    "  --system   machine-wide config\n"
    "  --global   current user's config (~/.tapto)\n"
    "  --local    per-folder config (./.tapto); the default for writes\n"
    "\n"
    "Options:\n"
    "  --show-origin   with 'list', prefix each entry with its scope\n"
    "  -h, --help      show this help\n"
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

// Config keys tapto-code understands; `config set` rejects anything else.
bool is_supported_config_key(const std::string& key) {
    static const char* kKeys[] = {
        "provider-type", "api-key", "provider-url", "model",
        "max-output-tokens", "system-prompt", "trace-file", "print-cot",
    };
    for (const char* k : kKeys) {
        if (key == k) return true;
    }
    return false;
}

int cmd_set(const Args& a) {
    if (a.positional.size() < 3) {
        ui::print_error("'set' requires <key> <value>");
        return 2;
    }
    if (!is_supported_config_key(a.positional[1])) {
        ui::print_error("unknown config key '" + a.positional[1] +
                        "'.\n  supported keys: provider-type, api-key, provider-url, "
                        "model, max-output-tokens, system-prompt, trace-file, print-cot");
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
    if (a.positional[1] == "api-key") {
        ui::print_warning("api-key stored in plaintext at " + path.string() +
                          "; set the provider's API key env var (e.g. ANTHROPIC_API_KEY) "
                          "to avoid storing it on disk");
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

std::string list_value(const std::string& key, const std::string& value) {
    return key == "api-key" ? mask_secret(value) : value;
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
    info["version"] = TAPTO_VERSION;
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

// Look up a config key's effective value across all scopes.
std::optional<std::string> get_effective(const std::string& key) {
    for (const auto& entry : effective_config()) {
        if (entry.key == key) return entry.value;
    }
    return std::nullopt;
}

std::string default_url(const std::string& provider) {
    if (provider == "claude") return "https://api.anthropic.com";
    if (provider == "openai") return "https://api.openai.com";
    if (provider == "gemini") return "https://generativelanguage.googleapis.com";
    return "";
}

std::string default_model(const std::string& provider) {
    if (provider == "claude") return "claude-sonnet-4-6";
    if (provider == "openai") return "gpt-4o";
    if (provider == "gemini") return "gemini-2.0-flash";
    return "";
}

const char* api_key_env_var(const std::string& provider) {
    if (provider == "claude") return "ANTHROPIC_API_KEY";
    if (provider == "openai") return "OPENAI_API_KEY";
    if (provider == "gemini") return "GEMINI_API_KEY";
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

// Resolve the API key for a provider: the provider's environment variable first,
// otherwise the config value. (The plaintext warning is emitted when the key is
// written, not on use.)
std::optional<std::string> resolve_api_key(const std::string& provider) {
    if (auto v = env_value(api_key_env_var(provider))) return v;
    return get_effective("api-key");
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
// for provider-type and api-key and store them in the global config. Returns
// false if the user aborts (EOF) or gives invalid input.
bool first_run_setup() {
    ui::print_setup_welcome();

    if (!get_effective("provider-type")) {
        ui::print_setup_provider_prompt();
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        std::string provider = trim(line);
        if (provider != "claude" && provider != "openai" && provider != "gemini") {
            ui::print_error("provider must be claude, openai, or gemini");
            return false;
        }
        if (!set_global("provider-type", provider)) return false;
    }

    auto provider = get_effective("provider-type");
    const char* keyvar = provider ? api_key_env_var(*provider) : "";
    // Only prompt for an api-key if it isn't already available via the env var.
    if (!env_value(keyvar) && !get_effective("api-key")) {
        ui::print_setup_apikey_prompt(keyvar);
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        std::string key = trim(line);
        if (!key.empty()) {
            if (!set_global("api-key", key)) return false;
            std::string warn = "api-key stored in plaintext at " +
                               config_path(Level::Global).string();
            if (keyvar && *keyvar)
                warn += "; set " + std::string(keyvar) + " to avoid storing it on disk";
            ui::print_warning(warn);
        }
        // A blank entry means the user intends to use the environment variable.
    }

    ui::print_setup_saved(config_path(Level::Global).string());
    return true;
}

int cmd_chat() {
    if (auto tf = get_effective("trace-file")) mclog_set_file(*tf);

    // Essentials: provider-type plus an api-key available via the provider's
    // environment variable or config. Run first-run setup if anything's missing.
    auto provider = get_effective("provider-type");
    auto have_key = [&] {
        return provider && (env_value(api_key_env_var(*provider)) || get_effective("api-key"));
    };
    if (!provider || !have_key()) {
        if (!first_run_setup()) return 2; // setup prints its own errors
        provider = get_effective("provider-type");
        if (!provider || !have_key()) {
            ui::print_error("provider-type and api-key must be configured to chat.");
            return 2;
        }
    }

    std::string url = get_effective("provider-url").value_or(default_url(*provider));
    std::string model = get_effective("model").value_or(default_model(*provider));

    auto key = resolve_api_key(*provider); // env var first, else config (with warning)
    if (!key) {
        ui::print_error("no api-key available for " + *provider);
        return 2;
    }

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
    if (auto v = get_effective("print-cot")) {
        // Default is on; only "false"/"0"/"off"/"no" (case-insensitive) disable it.
        std::string s = *v;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        ai_config.setPrintCot(!(s == "false" || s == "0" || s == "off" || s == "no"));
    }
    std::unique_ptr<AiBackend> client;
    if (*provider == "claude") {
        client = std::make_unique<ClaudeClient>(&ai_config, url, model, *key);
    } else if (*provider == "openai") {
        client = std::make_unique<OpenAIClient>(&ai_config, url, model, *key);
    } else if (*provider == "gemini") {
        client = std::make_unique<GeminiClient>(&ai_config, url, model, *key);
    } else {
        ui::print_error("unknown provider-type '" + *provider + "' (expected claude|openai|gemini)");
        return 2;
    }

    client->start();
    client->setSystemPrompt(resolve_system_prompt());

    // Register the file tools (editor + search) for this chat session.
    Context context;
    context.tools = builtin_tools();

    ui::print_banner(TAPTO_VERSION, *provider, model);
    std::string line;
    while (true) {
        ui::print_prompt("\x1b[?25h> "); // ensure cursor is visible at the prompt
        if (!std::getline(std::cin, line)) {
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
        try {
            std::string reply = client->chat(context, line);
            ui::print_reply(reply);
        } catch (const std::exception& e) {
            ui::print_error(e.what());
        }
    }
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--system") a.level = Level::System;
        else if (arg == "--global") a.level = Level::Global;
        else if (arg == "--local") a.level = Level::Local;
        else if (arg == "--show-origin") a.show_origin = true;
        else if (arg == "-h" || arg == "--help") { ui::print_usage(kUsage); return 0; }
        else rest.push_back(std::move(arg));
    }

    // No subcommand: start a chat (it's the default action).
    if (rest.empty()) {
        return cmd_chat();
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
