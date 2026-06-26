#include "minicode/commands.hpp"
#include "minicode/config.hpp"
#include "minicode/paths.hpp"
#include "minicode/tools.hpp"

#include "claude.h"
#include "openai.h"
#include "gemini.h"
#include "ai/aiconfig.h"
#include "ai/log.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace minicode;

#ifndef MINICODE_VERSION
#define MINICODE_VERSION "0.0.0-dev"
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
    "mini-code - a small cross-platform CLI\n"
    "\n"
    "Usage:\n"
    "  mini-code                                    Start an interactive chat (default)\n"
    "  mini-code [--system|--global|--local] config <command> [args]\n"
    "  mini-code [--system|--global|--local] command <add|remove|list> ...\n"
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
    "  provider-url (optional), model (optional), max-output-tokens (optional)\n"
    "\n"
    "Scope flags:\n"
    "  --system   machine-wide config\n"
    "  --global   current user's config (~/.minicode)\n"
    "  --local    per-folder config (./.minicode); the default for writes\n"
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

int cmd_set(const Args& a) {
    if (a.positional.size() < 3) {
        std::cerr << "error: 'set' requires <key> <value>\n";
        return 2;
    }
    Level level = a.level.value_or(Level::Local);
    auto path = config_path(level);
    Config cfg = Config::load(path);
    cfg.set(a.positional[1], a.positional[2]);
    try {
        cfg.save(path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    if (a.positional[1] == "api-key") {
        std::cerr << "warning: api-key stored in plaintext at " << path.string()
                  << "; set the provider's API key env var (e.g. ANTHROPIC_API_KEY) "
                     "to avoid storing it on disk\n";
    }
    return 0;
}

int cmd_get(const Args& a) {
    if (a.positional.size() < 2) {
        std::cerr << "error: 'get' requires <key>\n";
        return 2;
    }
    const std::string& key = a.positional[1];

    if (a.level) {
        Config cfg = Config::load(config_path(*a.level));
        if (auto value = cfg.get(key)) {
            std::cout << *value << "\n";
            return 0;
        }
        return 1; // not found
    }

    for (const auto& entry : effective_config()) {
        if (entry.key == key) {
            std::cout << entry.value << "\n";
            return 0;
        }
    }
    return 1; // not found
}

int cmd_unset(const Args& a) {
    if (a.positional.size() < 2) {
        std::cerr << "error: 'unset' requires <key>\n";
        return 2;
    }
    Level level = a.level.value_or(Level::Local);
    auto path = config_path(level);
    Config cfg = Config::load(path);
    if (!cfg.unset(a.positional[1])) {
        std::cerr << "error: key not found: " << a.positional[1] << "\n";
        return 1;
    }
    try {
        cfg.save(path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int cmd_list(const Args& a) {
    if (a.level) {
        Config cfg = Config::load(config_path(*a.level));
        for (const auto& entry : cfg.entries()) {
            if (a.show_origin) std::cout << level_name(*a.level) << "\t";
            std::cout << entry.first << "=" << entry.second << "\n";
        }
        return 0;
    }

    for (const auto& entry : effective_config()) {
        if (a.show_origin) std::cout << level_name(entry.origin) << "\t";
        std::cout << entry.key << "=" << entry.value << "\n";
    }
    return 0;
}

int cmd_version() {
    nlohmann::json info;
    info["name"] = "mini-code";
    info["version"] = MINICODE_VERSION;
    std::cout << info.dump(2) << "\n";
    return 0;
}

// Handles the `command` subcommand (add / remove / list). `rest` is the raw
// argument list after `command`, taken verbatim so `add` can capture a command
// line containing tokens like --build or --config.
int cmd_command(std::optional<Level> level, const std::vector<std::string>& rest) {
    if (rest.empty()) {
        std::cerr << "usage: mini-code [--scope] command <add|remove|list> ...\n";
        return 2;
    }
    const std::string& sub = rest[0];

    if (sub == "add") {
        if (rest.size() < 3) {
            std::cerr << "error: 'command add' requires <name> <command...>\n";
            return 2;
        }
        const std::string& name = rest[1];
        if (name.find_first_of(" \t=") != std::string::npos) {
            std::cerr << "error: command name must not contain spaces or '='\n";
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
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
        std::cout << "Added '" << name << "' (" << level_name(lvl) << "): " << cmdline << "\n";
        return 0;
    }

    if (sub == "remove" || sub == "rm") {
        if (rest.size() < 2) {
            std::cerr << "error: 'command remove' requires <name>\n";
            return 2;
        }
        Level lvl = level.value_or(Level::Local);
        if (!remove_command(lvl, rest[1])) {
            std::cerr << "error: command not found in " << level_name(lvl)
                      << " scope: " << rest[1] << "\n";
            return 1;
        }
        std::cout << "Removed '" << rest[1] << "' from " << level_name(lvl) << "\n";
        return 0;
    }

    if (sub == "list") {
        if (level) {
            for (const auto& e : commands_in_scope(*level)) {
                std::cout << e.name << " = " << e.command << "\n";
            }
        } else {
            for (const auto& e : effective_commands()) {
                std::cout << level_name(e.origin) << "\t" << e.name << " = " << e.command << "\n";
            }
        }
        return 0;
    }

    std::cerr << "error: unknown command subcommand '" << sub << "'\n";
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
// is the job of `mini-code install`.
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
        std::cerr << "error: " << e.what() << "\n";
        return false;
    }
}

// First-run setup: when the essentials aren't configured, interactively prompt
// for provider-type and api-key and store them in the global config. Returns
// false if the user aborts (EOF) or gives invalid input.
bool first_run_setup() {
    std::cout << "Welcome to mini-code. Let's set up your AI provider.\n";

    if (!get_effective("provider-type")) {
        std::cout << "Provider (claude / openai / gemini): " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        std::string provider = trim(line);
        if (provider != "claude" && provider != "openai" && provider != "gemini") {
            std::cerr << "error: provider must be claude, openai, or gemini\n";
            return false;
        }
        if (!set_global("provider-type", provider)) return false;
    }

    auto provider = get_effective("provider-type");
    const char* keyvar = provider ? api_key_env_var(*provider) : "";
    // Only prompt for an api-key if it isn't already available via the env var.
    if (!env_value(keyvar) && !get_effective("api-key")) {
        std::cout << "API key";
        if (keyvar && *keyvar) std::cout << " (or leave blank to use $" << keyvar << ")";
        std::cout << ": " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        std::string key = trim(line);
        if (!key.empty()) {
            if (!set_global("api-key", key)) return false;
            std::cerr << "warning: api-key stored in plaintext at "
                      << config_path(Level::Global).string();
            if (keyvar && *keyvar) std::cerr << "; set " << keyvar << " to avoid storing it on disk";
            std::cerr << "\n";
        }
        // A blank entry means the user intends to use the environment variable.
    }

    std::cout << "Saved to " << config_path(Level::Global).string() << "\n\n";
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
            std::cerr << "error: provider-type and api-key must be configured to chat.\n";
            return 2;
        }
    }

    std::string url = get_effective("provider-url").value_or(default_url(*provider));
    std::string model = get_effective("model").value_or(default_model(*provider));

    auto key = resolve_api_key(*provider); // env var first, else config (with warning)
    if (!key) {
        std::cerr << "error: no api-key available for " << *provider << "\n";
        return 2;
    }

    // ai_config is declared before client so it outlives the client, which
    // holds a pointer to it.
    AiConfig ai_config;
    if (auto v = get_effective("max-output-tokens")) {
        try {
            ai_config.setMaxOutputTokens(std::stoi(*v));
        } catch (const std::exception&) {
            std::cerr << "warning: invalid max-output-tokens '" << *v << "', using default\n";
        }
    }
    std::unique_ptr<AiBackend> client;
    if (*provider == "claude") {
        client = std::make_unique<ClaudeClient>(&ai_config, url, model, *key);
    } else if (*provider == "openai") {
        client = std::make_unique<OpenAIClient>(&ai_config, url, model, *key);
    } else if (*provider == "gemini") {
        client = std::make_unique<GeminiClient>(&ai_config, url, model, *key);
    } else {
        std::cerr << "error: unknown provider-type '" << *provider
                  << "' (expected claude|openai|gemini)\n";
        return 2;
    }

    client->start();
    client->setSystemPrompt(resolve_system_prompt());

    // Register the file tools (editor + search) for this chat session.
    Context context;
    context.tools = builtin_tools();

    std::cout << "mini-code chat - provider: " << *provider << ", model: " << model << "\n"
              << "Tools: ";
    for (size_t i = 0; i < context.tools.size(); ++i) {
        std::cout << (i ? ", " : "") << context.tools[i].name;
    }
    std::cout << "\nSlash commands: /clear, /list-commands, /add-command <name> <command...>, /exit\n";

    std::string line;
    while (true) {
        std::cout << "\x1b[?25h> " << std::flush; // ensure cursor is visible at the prompt
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break; // EOF (Ctrl-D / Ctrl-Z)
        }
        if (line == "/exit" || line == "/quit") break;
        if (line.empty()) continue;

        // Reset the conversation (e.g. to recover after filling the context window).
        if (line == "/clear") {
            client->start();
            std::cout << "(conversation cleared)\n";
            continue;
        }

        // In-session command management. Newly added commands are immediately
        // runnable by the agent (run_command reads the store on each call).
        if (line == "/list-commands") {
            auto cmds = effective_commands();
            if (cmds.empty()) {
                std::cout << "(no commands configured)\n";
            } else {
                for (const auto& e : cmds) {
                    std::cout << level_name(e.origin) << "\t" << e.name << " = " << e.command << "\n";
                }
            }
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
                std::cout << "usage: /add-command <name> <command...>\n";
                continue;
            }
            if (name.find('=') != std::string::npos) {
                std::cout << "error: command name must not contain '='\n";
                continue;
            }
            std::string cmdline = remainder.substr(begin);
            try {
                add_command(Level::Local, name, cmdline);
                std::cout << "Added '" << name << "' (local): " << cmdline << "\n";
            } catch (const std::exception& e) {
                std::cout << "error: " << e.what() << "\n";
            }
            continue;
        }

        try {
            std::string reply = client->chat(context, line);
            std::cout << reply << "\n";
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
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
        else if (arg == "-h" || arg == "--help") { std::cout << kUsage; return 0; }
        else rest.push_back(std::move(arg));
    }

    // No subcommand: start a chat (it's the default action).
    if (rest.empty()) {
        return cmd_chat();
    }
    const std::string& top = rest[0];
    if (top == "version") return cmd_version();
    if (top != "config") {
        std::cerr << "error: unknown command '" << top << "'\n";
        return 2;
    }

    a.positional.assign(rest.begin() + 1, rest.end());
    if (a.positional.empty()) {
        std::cerr << kUsage;
        return 2;
    }

    const std::string& sub = a.positional[0];
    if (sub == "set")   return cmd_set(a);
    if (sub == "get")   return cmd_get(a);
    if (sub == "unset") return cmd_unset(a);
    if (sub == "list")  return cmd_list(a);

    std::cerr << "error: unknown config command '" << sub << "'\n";
    return 2;
}
