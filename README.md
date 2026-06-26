# mini-code

A small, dependency-light command-line AI coding assistant for Windows and
Linux. Chat with an AI model in your terminal and let it work in the current
folder — read and edit files, search the tree, and run commands you've
approved — all from a single self-contained C++17 binary.

**Highlights**

- **No permission prompts** — it never interrupts to ask. Its reach is bounded by
  design instead: run it in a folder and it stays there, and it only runs the
  commands you configured beforehand.
- **Multiple providers** — Claude, OpenAI, or Gemini, chosen via config; the API
  key is read from an environment variable or config.
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

The binary is produced at `build/mini-code` (Linux) or
`build/Release/mini-code.exe` (Windows / MSVC).

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
| system   | `--system` | `%PROGRAMDATA%\minicode\config` / `/etc/minicode/config` |
| global   | `--global` | `~/.minicode/config` (user home)                     |
| local    | `--local`  | per-folder, stored centrally at `~/.minicode/projects/<folder>/config` |

When reading, **local overrides global overrides system**. Writes default to
the **local** scope; pass `--global` or `--system` to target another scope.

Local (project) settings are stored **centrally**, keyed by the working
directory, under `~/.minicode/projects/` — not inside the project folder. So
nothing is written into your repo, and cloning a repo can't bring its own config
or runnable commands with it.

### Examples

```sh
mini-code --global config set api-key sk_ant_xx23982932
mini-code config set max-output-tokens 32000   # writes to this folder's local config
mini-code config get api-key             # effective value across scopes
mini-code config list                    # all effective values
mini-code config list --show-origin      # prefix each entry with its scope
mini-code --global config list           # only the global scope
mini-code config unset editor            # remove from local
```

## Chat

Running `mini-code` with no subcommand starts an interactive chat with the
configured AI provider (chat is the default action).
It prints a `>` prompt, reads a line, sends it to the provider, prints the
reply, and repeats. Type `/exit` (or Ctrl-D) to quit.

In-session slash commands: `/clear` (reset the conversation — useful to recover
after filling the model's context window), `/list-commands`, and
`/add-command <name> <command...>`.

**First run:** if no provider/api-key is configured, mini-code prompts for them
interactively and saves them to the global (`~/.minicode`) config, then starts
the chat. You can also set them manually instead:

```sh
mini-code --global config set provider-type claude        # claude | openai | gemini
mini-code --global config set api-key sk_ant_xx23982932
mini-code                                                 # starts the chat
```

Chat config keys:

| Key             | Required | Default (per provider)                              |
| --------------- | -------- | --------------------------------------------------- |
| `provider-type` | yes      | —  (`claude` / `openai` / `gemini`)                 |
| `api-key`       | yes      | —                                                   |
| `provider-url`  | no       | claude: `https://api.anthropic.com`, openai: `https://api.openai.com`, gemini: `https://generativelanguage.googleapis.com` |
| `model`         | no       | claude: `claude-sonnet-4-6`, openai: `gpt-4o`, gemini: `gemini-2.0-flash` |
| `max-output-tokens` | no   | `16000` — raise it for long replies (large tables, reports) |
| `trace-file`    | no       | unset — set to a path to enable diagnostic logging |

**API key from the environment:** the key is read from the provider's
environment variable first — `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` /
`GEMINI_API_KEY` — and only falls back to the `api-key` config value if the
variable isn't set. Writing the key to config (via `config set api-key` or the
first-run prompt) prints a one-time plaintext-storage warning; using it
afterward is silent. `config list` masks the key (e.g. `sk_ant...cdef`); use
`config get api-key` for the full value.

**Diagnostic logging:** off by default. Set `trace-file` to a path
(`mini-code config set trace-file ./minicode.log`) to append request/response
diagnostics there; unset it to disable.

If a reply hits the output-token limit it is cut off and marked
`[truncated: hit max output tokens - raise max-output-tokens]`; raise
`max-output-tokens` to allow longer responses.

### Tools

During a chat the model can call these local-filesystem tools (relative to the
directory mini-code is launched from):

- **`str_replace_based_edit_tool`** — view / create / str_replace / insert.
  Declared as Claude's built-in `text_editor_20250728` for Anthropic, and with
  an explicit JSON schema for OpenAI/Gemini.
- **`find_files`** — find files by wildcard pattern (`*`, `?`), optionally
  grepping their contents.
- **`list_commands`** — list the allow-listed commands (see below).
- **`run_command`** — run one allow-listed command by name and return its
  output and exit code.

These edit real files on disk. `create` refuses to overwrite an existing file;
`str_replace` requires the target string to be unique.

**Sandbox:** the file tools (`str_replace_based_edit_tool`, `find_files`) are
confined to the directory mini-code was started in and its subdirectories. Paths
that resolve outside that subtree — via `..`, an absolute path, or a symlink —
are rejected. (`run_command` is governed separately: it can only run the
commands you explicitly allow-list, so its reach is whatever you configure.)

## Commands (allow-list)

`run_command` is **not** a general shell — the agent can only run commands you
have explicitly allow-listed. Commands are stored per scope (system / global /
local, same precedence as config); local commands live in the central
per-folder store (not in the repo), so a cloned project can't ship runnable
commands. Managed with:

```sh
mini-code command add build-debug cmake --build build --config Debug
mini-code --global command add gs git status
mini-code command list                 # merged, with scope of each
mini-code command remove build-debug
```

Everything after the name is captured verbatim as the command line (so flags
like `--config Debug` are part of the command, not parsed by mini-code). In a
chat the agent discovers them via `list_commands` and runs them via
`run_command` — it can never supply arbitrary shell text, only pick a name.

### Command arguments

A command template may contain positional placeholders `%1`, `%2`, … and `%*`
(all remaining values). The agent fills them via `run_command`'s `args`:

```sh
mini-code command add commit git commit -m %1
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
mini-code command add fmt clang-format -i %p1   # only formats files in-tree
```

## Other commands

```sh
mini-code version                        # print version info as JSON
```

### File format

Plain `key = value` lines; `#` and `;` begin comments.

```
# mini-code
api-key = sk_ant_xx23982932
model = claude-sonnet-4-6
```

## Part of TaptoMatic

mini-code is a small, standalone spin-off of
[TaptoMatic](https://taptomatic.com) — a larger AI-powered development platform
from Centlake Software AB where teams of AI agents collaborate on software
projects under your direction. Where mini-code is a single-binary CLI you point
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

Like mini-code, TaptoMatic runs locally and uses your own provider API keys
(Claude, OpenAI, Gemini, or a local inference server) — your code only leaves
your machine to call those APIs. It's currently in preview; see
[taptomatic.com](https://taptomatic.com).

## License

mini-code is licensed under the Apache License, Version 2.0 (SPDX:
`Apache-2.0`). See [LICENSE](LICENSE) and [NOTICE](NOTICE).
Copyright 2026 Centlake Software AB.
