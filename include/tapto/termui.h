// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>
#include <vector>

#include "tapto/session.h"
#include "tapto/stream.h"
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

// The color the user's own input is shown in, and the reset after it: the
// chat loop's prompt, the POSIX line editor's echo and /resume's replay of
// earlier prompts all use it, so the user's lines look the same everywhere.
inline constexpr const char* kPromptColor = "\x1b[33m";
inline constexpr const char* kPromptReset = "\x1b[0m";

// Print the model's final reply (plain, no prefix). When the reply was
// streamed (print_stream), it is on the screen already: its plain text is
// redrawn as Markdown in place while all of it is in view, and otherwise left
// as it is.
void print_reply(const std::string& text);

// Write a piece of a streamed reply as it arrives (AiConfig::setStreamCallback):
// the chain of thought dimmed, the text plain, a line end at the end of each.
void print_stream(const tapto::StreamEvent& ev);

// Forget the streamed blocks of the turn before. Called as a turn starts.
void begin_turn_output();

// Render Markdown text to ANSI-styled terminal output. Tables are wrapped so
// the rendered width never exceeds `max_width` visible columns (the terminal
// width in normal use; a smaller value in tests). Pure function: returns the
// rendered string for the caller to emit.
std::string render_markdown(const std::string& text, size_t max_width);

// Print the end of a resumed conversation as it looked on the screen, about
// one screenful: the user's prompts in the prompt color, the model's text as
// a reply. `after` is what the caller prints next (the resume notice, the
// folders, an unanswered prompt), counted with the prompt line below it, so
// the start of what is shown stays on screen above them. An older part that
// does not fit is not shown, and a line says so.
void print_transcript(const std::vector<TranscriptEntry>& entries, const std::string& after);

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
