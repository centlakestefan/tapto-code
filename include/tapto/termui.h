// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>
#include <vector>

#include "tapto/ui.h"

// ---------------------------------------------------------------------------
// tapto-code's own terminal output, over and above tapto/ui.h.
//
// libtapto declares the seven functions its provider clients call (status
// line, intermediate text, plain/error/warning lines) and each program supplies
// the bodies. Everything else this program prints -- the banner, the help
// screen, the first-run prompts, config and command listings -- is declared
// here and implemented in the same ui.cpp, so formatting and ANSI handling
// stay in one file. tapto-word does the same with its paneui.h.
//
// Thread-safety: all functions are single-threaded; the chat loop is
// synchronous so no locking is needed.
// ---------------------------------------------------------------------------

namespace tapto::ui {

// --- Permanent output (scrolls into transcript) ----------------------------

// Print the model's final reply (plain, no prefix).
void print_reply(const std::string& text);

// Print a usage/diagnostic message to stderr.
void print_usage(const std::string& text);


// --- Chat session header ---------------------------------------------------

// Print the full startup banner: ASCII art logo, version, provider/model/URL,
// and slash-command hints -- all in one styled block. `provider` is the
// resolved provider as shown to the user, e.g. "claude" or "gemma4 (openai)".
void print_banner(const std::string& version,
                  const std::string& provider,
                  const std::string& model,
                  const std::string& url);

// Legacy thin wrappers kept for any callers that haven't migrated yet.
void print_chat_header(const std::string& provider,
                       const std::string& model,
                       const std::vector<std::string>& tool_names);
void print_chat_hints();

// Print the /help screen: slash-command reference and active tool list.
void print_help(const std::vector<std::string>& tool_names);


// --- First-run / interactive prompts --------------------------------------

void print_setup_welcome();
void print_setup_provider_prompt();
void print_setup_apikey_prompt(const char* env_var_name); // env_var_name may be ""
void print_setup_saved(const std::string& path);

// Print a plaintext prompt for interactive use (no newline, flushes).
void print_prompt(const std::string& text);

// Print a blank line after the user has submitted their prompt, separating
// the input line from the model's response.
void print_prompt_accepted();


// --- Config / command listing ---------------------------------------------

void print_config_entry(const std::string& scope,   // empty when --show-origin is off
                        const std::string& key,
                        const std::string& value);

void print_command_entry(const std::string& scope,  // empty when listing a single scope
                         const std::string& name,
                         const std::string& command);

void print_command_added(const std::string& name,
                         const std::string& scope,
                         const std::string& command);

void print_command_removed(const std::string& name, const std::string& scope);

void print_no_commands();

} // namespace tapto::ui
