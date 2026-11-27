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
// The one exception is refresh_status(), called from a thread of its own
// while the model works: what writes to the terminal during a turn takes the
// same lock as it does.
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

// The stream callback when print-cot is off: nothing of the reply is written,
// but the status line counts the characters received, "Thinking... (412
// chars)" while the model reasons and "Writing... (37 chars)" once its answer
// comes, so a long think does not look like a hang.
void count_stream(const tapto::StreamEvent& ev);

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
// does not fit is not shown, and a line says so. The chain of thought is
// dimmed and a tool call is a line of its own, as they were live.
void print_transcript(const std::vector<TranscriptEntry>& entries, const std::string& after);

// The whole conversation again, in place of what the screen holds: the
// screen and its scrollback are cleared, and `entries` printed from the
// first, as print_transcript prints them. For /cot, which shows or hides the
// chain of thought in what is already there.
void redraw_transcript(const std::vector<TranscriptEntry>& entries);

// Print a usage/diagnostic message to stderr.
void print_usage(const std::string& text);

// Headless (--headless): one turn and no terminal UI. The reply goes to
// stdout as it came, without the Markdown styling; everything else -- each
// tool call, the chain of thought, notices -- goes to stderr a line at a time,
// with no status line redrawn in place and no ANSI styling when stderr is no
// terminal. Set once, before the turn.
void set_headless(bool on);


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

struct SlashCommand; // below

// Print the /help screen: the slash commands -- `commands`, the table the
// prompt's menu shows too -- the keys, and the active tools.
void print_help(const std::vector<SlashCommand>& commands,
                const std::vector<std::string>& tool_names);


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

// Write the time of day, dim, at the right of the row above the cursor,
// which is taken `used` columns of a `cols`-wide terminal; the cursor is
// left at the start of its row. Nothing when the time does not fit beside
// the text, or headless. The chat loop stamps the prompt it sends, and
// print_reply the reply, so the two times show how long the turn took.
void stamp_time_above(size_t used, size_t cols);

// --- Slash-command menu -----------------------------------------------------

// One entry of the menu the prompt shows while a slash command is typed.
struct SlashCommand {
    std::string name;        // without the '/'
    std::string args;        // usage after the name ("<path> [ro|rw]"), may be empty
    std::string description; // one line
};

// The commands whose name starts with `typed` (the text after the '/'), in
// name order. An empty `typed` matches every command.
std::vector<SlashCommand> slash_menu_matches(const std::vector<SlashCommand>& commands,
                                             const std::string& typed);

// The menu's rows for `matches`, `selected` marked, at most `max_rows` of
// them (a window that keeps `selected` in view), each clipped to fewer than
// `cols` visible columns so that no row wraps. Pure: the caller prints them.
std::vector<std::string> slash_menu_lines(const std::vector<SlashCommand>& matches,
                                          size_t selected, size_t max_rows, size_t cols);

// --- While the model works ---------------------------------------------------

// Show on the status line, after its text, how long the request (or tool)
// has run when `elapsed` is on (status-elapsed), and `details` -- what the
// plugins say (Plugins::status) -- and redraw it: "Thinking... (412 chars) ·
// 45s · 27 tok/s". Called each second from a thread of its own while the
// model works; end_status() stops it until the next call.
void refresh_status(bool elapsed, const std::string& details);


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
