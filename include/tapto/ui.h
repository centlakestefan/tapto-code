// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Terminal UI — all user-visible output goes through these functions so that
// formatting, ANSI control sequences, and progress-line management are
// centralised in one place (ui.cpp).
//
// Thread-safety: all functions are single-threaded; the chat loop is
// synchronous so no locking is needed.
// ---------------------------------------------------------------------------

namespace tapto::ui {

// --- Progress / status line ------------------------------------------------
//
// The status line is a single terminal row kept on screen while the model is
// working. It is overwritten in-place using "\r" so it never scrolls into the
// permanent transcript. Call ui_end_status() (or any permanent-output
// function) to erase it before writing a real line.

// Show or update the spinning status line. `iteration` and `max_iterations`
// are the current tool-loop counters; pass 0/0 for the initial "Thinking..."
// phase before any tools have run.
//
// Rendered format examples:
//   "Thinking..."
//   "[1/50] Thinking..."
//   "[3/50] str_replace_based_edit_tool str_replace src/foo.cpp"
void set_status(const std::string& text, int iteration, int max_iterations);

// Commit the current status line to the scroll buffer as a permanent line,
// then clear the status state. Use this after a tool finishes so its name
// stays visible in the transcript while "Thinking..." takes the next line.
void commit_status();

// Erase the status line and restore the cursor. Call once at the end of a
// chat turn before printing the final reply.
void end_status();


// --- Permanent output (scrolls into transcript) ----------------------------

// Print the model's intermediate chain-of-thought or prose that accompanies
// a tool call. Erases the status line first so it isn't clobbered.
// `is_reasoning` selects a dimmed style for thinking blocks vs. normal prose.
// Respects `print_cot`: when false the call is a no-op.
void emit_intermediate(const std::string& text, bool is_reasoning, bool print_cot);

// Print the model's final reply (plain, no prefix).
void print_reply(const std::string& text);

// Print a line to stdout (used for config/command output in main).
void print_line(const std::string& text);

// Print an error to stderr.
void print_error(const std::string& text);

// Print a warning to stderr.
void print_warning(const std::string& text);

// Print a usage/diagnostic message to stderr.
void print_usage(const std::string& text);


// --- Chat session header ---------------------------------------------------

// Print the full startup banner: ASCII art logo, version, provider/model,
// and slash-command hints — all in one styled block.
void print_banner(const std::string& version,
                  const std::string& provider,
                  const std::string& model);

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
