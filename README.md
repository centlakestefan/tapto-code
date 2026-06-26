# mini-code

A small cross-platform (Windows + Linux) CLI. The first component is a
git-inspired configuration system with three scopes.

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
| local    | `--local`  | `./.minicode/config` in the current directory        |

When reading, **local overrides global overrides system**. Writes default to
the **local** scope; pass `--global` or `--system` to target another scope.

### Examples

```sh
mini-code --global config set api-key sk_ant_xx23982932
mini-code config set editor vim          # writes to ./.minicode/config
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

Configure the provider first (use `--global` to apply everywhere):

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
local, same precedence as config) in a `.minicode/commands` file, managed with:

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
# mini-code config
api-key = sk_ant_xx23982932
editor = vim
```
