# tapto-code

[![CI](https://github.com/centlakestefan/tapto-code/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/centlakestefan/tapto-code/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)

A small, dependency-light command-line AI coding assistant for Windows and
Linux. Chat with an AI model in your terminal and let it work in the current
folder — read and edit files, search the tree, and run commands you've
approved — all from a single self-contained C++17 binary.

**Highlights**

- **No permission prompts** — it never interrupts to ask. Its reach is bounded by
  design instead: run it in a folder and it stays there, and it only runs the
  commands you configured beforehand.
- **Multiple providers** — Claude, any OpenAI-compatible endpoint (vLLM,
  llama.cpp, LM Studio, OpenAI itself), or Gemini. Name as many as you like in
  one config, including two local servers speaking the same API, and switch with
  `--provider`. The API key is read from an environment variable or config.
- **File tools** — the model can view, create, and edit files and search the
  tree, confined to the directory you launch it in (sandboxed: no escaping via
  `..`, absolute paths, or symlinks).
- **Allow-listed commands** — `run_command` only runs commands you've explicitly
  added (with optional `%1` / `%p1` placeholders); it is never a general shell.
- **Three-scope config** — system / global / project, with git-style precedence.
- **Self-contained** — one binary; dependencies (nlohmann/json, cpp-httplib,
  OpenSSL) are fetched at build time.

Licensed under the Apache License 2.0.

## Build

Requires CMake 3.16+ and a C++17 compiler (MSVC, gcc, or clang).

```sh
cmake -S . -B build
cmake --build build --config Release
```

The binary is produced at `build/tapto-code` (Linux) or
`build/Release/tapto-code.exe` (Windows / MSVC).

Tests are plain executables with assertions of their own — nothing to fetch or
install:

```sh
ctest --test-dir build --output-on-failure
```

### Dependencies

Fetched automatically at configure time via CMake `FetchContent` (needs git +
network on the first configure):

- [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3` — JSON.
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) `v0.15.3` — HTTP client
  used by the provider clients to talk to the chat APIs.

**OpenSSL is required** (the provider APIs are HTTPS). It must be installed and
findable by CMake. On Linux install your distro's `libssl-dev` /
`openssl-devel`. On Windows/MSVC, if it isn't auto-detected, point CMake at it:

```sh
cmake -S . -B build -DOPENSSL_ROOT_DIR=C:/path/to/openssl
```

## Config

Three scopes, in increasing order of precedence:

| Scope    | Flag       | Location                                              |
| -------- | ---------- | ---------------------------------------------------- |
| system   | `--system` | `%PROGRAMDATA%\tapto\config` / `/etc/tapto/config` |
| global   | `--global` | `~/.tapto/config` (user home)                     |
| local    | `--local`  | per-folder, stored centrally at `~/.tapto/projects/<folder>/config` |

When reading, **local overrides global overrides system**. Writes default to
the **local** scope; pass `--global` or `--system` to target another scope.

Local (project) settings are stored **centrally**, keyed by the working
directory, under `~/.tapto/projects/` — not inside the project folder. So
nothing is written into your repo, and cloning a repo can't bring its own config
or runnable commands with it.

### Examples

```sh
tapto-code --global config set claude-api-key sk_ant_xx23982932
tapto-code config set max-output-tokens 32000   # writes to this folder's local config
tapto-code config get claude-api-key      # effective value across scopes
tapto-code config list                    # all effective values
tapto-code config list --show-origin      # prefix each entry with its scope
tapto-code --global config list           # only the global scope
tapto-code config unset model             # remove from local
```

## Chat

Running `tapto-code` with no subcommand starts an interactive chat with the
configured AI provider (chat is the default action); `--provider <name>` picks a
different one for this session.
It prints a `>` prompt, reads a line, sends it to the provider, prints the
reply, and repeats. Type `/exit` (or Ctrl-D) to quit.

In-session slash commands: `/clear` (reset the conversation — useful to recover
after filling the model's context window), `/list-commands`, and
`/add-command <name> <command...>`.

**First run:** if no provider/api-key is configured, tapto-code prompts for them
interactively and saves them to the global (`~/.tapto`) config, then starts
the chat. You can also set them manually instead:

```sh
tapto-code --global config set provider claude             # claude | openai | gemini
tapto-code --global config set claude-api-key sk_ant_xx23982932
tapto-code                                                 # starts the chat
```

Chat config keys:

| Key             | Required | Default (per dialect)                               |
| --------------- | -------- | --------------------------------------------------- |
| `provider`      | yes      | `claude` — which provider block to use              |
| `<name>-provider-type` | —  | `claude` / `openai` / `gemini` — the API to speak  |
| `<name>-api-key` | yes     | — or the vendor's environment variable; may be an `env:` / `cmd:` / `wincred:` reference |
| `<name>-provider-url` | no | claude: `https://api.anthropic.com`, openai: `https://api.openai.com`, gemini: `https://generativelanguage.googleapis.com` |
| `<name>-model`  | no       | claude: `claude-sonnet-4-6`, openai: `gpt-4o`, gemini: `gemini-2.0-flash` |
| `<name>-reasoning-effort` | no | unset — sent as `reasoning_effort` by the **openai** dialect only (e.g. `low`, `medium`, `high`); ignored with a warning on other dialects |
| `max-output-tokens` | no   | `16000` — raise it for long replies (large tables, reports) |
| `max-tool-iterations` | no | `200` — max tool-call rounds per reply before the agent stops |
| `print-cot`     | no       | `true` — show the model's intermediate reasoning/text during tool calls; set `false` to keep it in the trace file only |
| `system-prompt` | no       | built-in prompt                                      |
| `trace-file`    | no       | unset — set to a path to enable diagnostic logging  |

### Naming providers

A provider has a **name** and a **dialect**. The name picks a block of keys and
can be anything; the dialect is one of the three request shapes tapto-code
speaks, named by that block's `-provider-type`. So two local servers that both
expose an OpenAI-compatible API can still be told apart:

```
qwen-provider-type = openai          gemma-provider-type = openai
qwen-provider-url  = http://box:8000 gemma-provider-url  = http://box:8081
qwen-model         = Qwen3-VL-30B    gemma-model         = gemma-3-27b
qwen-api-key       = local           gemma-api-key       = local
```

```sh
tapto-code --provider gemma          # or 'provider = gemma' as the default
```

`claude`, `openai` and `gemini` need no block at all — used as a name, each
means its own dialect with that vendor's defaults. Asking for a name the store
does not define lists the ones it does. The resolved provider, dialect, model and
URL are printed in the startup banner, so a block wired to the wrong dialect
shows up immediately rather than as malformed requests.

The unscoped `model`, `provider-url` and `api-key` apply only to the **default**
provider — the one `provider` names. Otherwise a local endpoint's URL would be
sent to a hosted vendor, or a hosted key to a local server.

An older store using `provider-type = claude` with an unscoped `api-key` and
`model` keeps working: `provider-type` is read as the default provider's name
when `provider` is absent.

The config store is shared with
[tapto-vnc](https://github.com/centlakestefan/tapto-vnc), which reads the same
provider blocks.

**API key resolution:** `<name>-api-key` first, then the vendor's environment
variable — `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` / `GEMINI_API_KEY` — then the
unscoped `api-key` for the default provider. The block's own key wins on purpose:
an environment variable taking precedence would send a real vendor key to
whatever `<name>-provider-url` points at, and a local server will log it. A
server that checks no key still needs one; any non-empty value does.

### Keeping keys out of the config file

An `api-key` value may say **where** the key lives instead of holding it:

| Value | Reads from |
| --- | --- |
| `sk-ant-...` | the value itself |
| `env:ANTHROPIC_API_KEY` | an environment variable |
| `cmd:pass show anthropic` | the first line of a command's output |
| `wincred:tapto/work-claude` | a Windows Credential Manager generic credential |

```sh
tapto-code --global config set work-api-key wincred:tapto/work-claude
cmdkey /generic:tapto/work-claude /user:tapto /pass    # prompts for the key
```

`cmd:` is the general escape hatch — `pass`, `gopass`, `op read op://vault/item`,
`gcloud`, `security find-generic-password`, or a git credential helper all work,
and the store keeps saying which provider uses which secret. The command's stderr
and stdin are left attached to the terminal, so a helper that needs to unlock a
vault can prompt.

`wincred:` reads a **generic** credential, which Windows encrypts under your user
account with DPAPI. That protects the key from a config file that gets backed up,
synced, screen-shared or pasted into an issue — it does *not* protect it from
code running as you, which can read the credential without a prompt. It is the
same trade git's `wincred` credential helper makes. Both blob encodings are
accepted, so credentials written by `cmdkey` or by git's helper are read
correctly.

A reference that fails — variable unset, command non-zero, credential missing —
is a hard error naming the problem. It never falls through to the next source,
because that is how one endpoint ends up being handed another one's key.

Writing a literal key to config (via `config set` or the first-run prompt) prints
a one-time plaintext-storage warning; a reference does not, since storing one is
the remedy. `config list` masks literal keys (e.g. `sk_ant...cdef`) but shows
references in full, as they name a location rather than a secret; use
`config get <name>-api-key` for the raw value.

**Diagnostic logging:** off by default. Set `trace-file` to a path
(`tapto-code config set trace-file ./tapto.log`) to append request/response
diagnostics there; unset it to disable.

If a reply hits the output-token limit it is cut off and marked
`[truncated: hit max output tokens - raise max-output-tokens]`; raise
`max-output-tokens` to allow longer responses.

### Tools

During a chat the model can call these local-filesystem tools (relative to the
directory tapto-code is launched from):

- **`str_replace_based_edit_tool`** — view / create / str_replace / insert.
  Declared as Claude's built-in `text_editor_20250728` for Anthropic, and with
  an explicit JSON schema for OpenAI/Gemini.
- **`find_files`** — find files by wildcard pattern (`*`, `?`), optionally
  grepping their contents.
- **`list_commands`** — list the built-in and allow-listed commands (see below).
- **`run_command`** — run one built-in or allow-listed command by name and
  return its output.

These edit real files on disk. `create` refuses to overwrite an existing file;
`str_replace` requires the target string to be unique.

**Line endings:** files are read and written byte-exact, and `view` shows the
model each line with its CR stripped — so the model composes edits in LF terms
whatever the file uses. Matching therefore treats a line ending as a line
ending: an `old_str` written with `\n` matches CRLF text, and vice versa. Text
written back takes the ending of the text it replaces, so an edit to a Windows
file stays CRLF and a one-line replacement that becomes several lines uses the
file's convention for the new ones. `insert` splices at a byte offset rather
than rebuilding the file, so a file with mixed endings — which a long Windows
history produces — keeps every ending the edit didn't touch. Two occurrences
that differ only in their line endings count as ambiguous and are refused,
since they are indistinguishable in what the model was shown.

**Sandbox:** the file tools (`str_replace_based_edit_tool`, `find_files`) are
confined to the directory tapto-code was started in and its subdirectories. Paths
that resolve outside that subtree — via `..`, an absolute path, or a symlink —
are rejected. (`run_command` is governed separately: it can only run the
commands you explicitly allow-list, so its reach is whatever you configure.)

## Commands

### Built-in commands (always available)

`run_command` ships a small set of read-only, cross-platform utilities
implemented in-process (no shell), so they work everywhere — including on pure
Windows, where these don't otherwise exist. They resolve paths inside the
sandbox and never shell out:

| Command | Purpose |
| ------- | ------- |
| `wc [-l\|-w\|-c] <file>` | count lines / words / bytes (all three with no flag) |
| `head [-n N] <file>`     | first N lines (default 10) |
| `tail [-n N] <file>`     | last N lines (default 10) |
| `cat <file>`             | print a file's contents |
| `ls [path]`              | list a directory (default `.`) |
| `tree [path] [-L depth]` | show a directory tree (skips `.git`, `build`, `node_modules`, …) |

These names are reserved — they take precedence over allow-listed commands and
can't be redefined with `command add`.

### Allow-listed commands

`run_command` is **not** a general shell — beyond the built-ins above, the agent
can only run commands you have explicitly allow-listed. Commands are stored per scope (system / global /
local, same precedence as config); local commands live in the central
per-folder store (not in the repo), so a cloned project can't ship runnable
commands. Managed with:

```sh
tapto-code command add build-debug cmake --build build --config Debug
tapto-code --global command add gs git status
tapto-code command list                 # merged, with scope of each
tapto-code command remove build-debug
```

To get started quickly, [`contrib/commands`](contrib/commands) is a curated,
security-conscious starter set (git, npm, cmake, make, bazel, maven, cargo, go,
python, …) you can
copy into your global store — see [contrib/README.md](contrib/README.md).

Everything after the name is captured verbatim as the command line (so flags
like `--config Debug` are part of the command, not parsed by tapto-code). In a
chat the agent discovers them via `list_commands` and runs them via
`run_command` — it can never supply arbitrary shell text, only pick a name.

### Command arguments

A command template may contain positional placeholders `%1`, `%2`, … and `%*`
(all remaining values). The agent fills them via `run_command`'s `args`:

```sh
tapto-code command add commit git commit -m %1
# agent calls run_command{ name: "commit", args: ["fix: handle empty input"] }
```

Quoting is a non-issue by design: a command **with** placeholders bypasses the
shell entirely and is executed as a literal argv vector, so an argument value
can contain spaces, quotes, `&`, `|`, `%`, etc. and is passed through verbatim —
nothing is re-interpreted by a shell. (A command **without** placeholders still
runs through the shell, so it can use pipes and redirection.)

On Windows, batch wrappers like `npm`, `npx`, and `yarn` are `.cmd` files that
can't be launched directly; parameterized commands targeting them are run via
`cmd.exe` automatically, so `npm %*` just works.

Because values are literal arguments, the model cannot inject extra commands.
Put placeholders only at *data* positions, not where they could become a flag
or subcommand (`git commit -m %1` is fine; `git %1` lets the model choose the
subcommand). Use `--` before a placeholder if a value might start with `-`
(e.g. `rm -- %1`).

**Path arguments (`%p1`):** a `%p`-prefixed placeholder (`%p1`..`%p9`, `%p*`)
marks a path that must stay inside the working-directory sandbox. The value is
resolved the same way as the file tools and the command is refused if it points
outside (via `..`, an absolute path, or a symlink); the resolved absolute path
is substituted.

```sh
tapto-code command add fmt clang-format -i %p1   # only formats files in-tree
```

## Other commands

```sh
tapto-code version                        # print version info as JSON
```

### File format

Plain `key = value` lines; `#` and `;` begin comments.

```
# tapto-code
provider = claude
claude-api-key = sk_ant_xx23982932
claude-model = claude-sonnet-4-6
```

## Part of TaptoMatic

tapto-code is a small, standalone spin-off of
[TaptoMatic](https://taptomatic.com) — a larger AI-powered development platform
from Centlake Software AB where teams of AI agents collaborate on software
projects under your direction. Where tapto-code is a single-binary CLI you point
at a folder, TaptoMatic is a local platform (web GUI) built around *structured
autonomy*: you set the direction with goals and tasks, and agents do the work.

- **Multi-agent teams** — assemble teams of agents with distinct roles that write
  code in parallel, review each other's work, and retry until a team leader
  approves. A task database with parent/child and dependency relationships keeps
  everything coordinated.
- **Goal-driven development** — define high-level goals; a goal agent tracks
  completion percentage and spawns tasks as needed.
- **Isolated build engines** — a separate build engine compiles and tests your
  code; the Podman engine adds container isolation and disables network access
  during the build/test phases. Polyglot and auto-detected: C/C++ (CMake, Bazel),
  Java (Maven, Gradle), Go, Rust, Python, JavaScript/Node.
- **Built-in Git** — every project has an internal Git repository; agents work in
  isolated Git worktrees and integrate approved changes into the development
  branch. External GitLab connections are supported.
- **Documents with semantic search** — a structured, versioned document store
  (draft → archived) so design docs and requirements stay organized instead of
  scattered as `.md` files. Semantic search uses AI embeddings (VoyageAI, ranked
  by cosine similarity) alongside Groonga full-text keyword search.
- **Chat and MCP** — plan interactively, or drive it from Claude Code and other
  MCP-compatible tools via the built-in MCP server.

Like tapto-code, TaptoMatic runs locally and uses your own provider API keys
(Claude, OpenAI, Gemini, or a local inference server) — your code only leaves
your machine to call those APIs. It's currently in preview; see
[taptomatic.com](https://taptomatic.com).

## License

tapto-code is licensed under the Apache License, Version 2.0 (SPDX:
`Apache-2.0`). See [LICENSE](LICENSE) and [NOTICE](NOTICE).
Copyright 2026 Centlake Software AB.
