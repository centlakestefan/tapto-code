# Changelog

Notable changes to tapto-code. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each entry is one line
about what changed for you, and the commit it links to carries the reasoning.

## How an entry is written

Each version's section is published as its release notes on taptomatic.com, so
the reader is someone who uses tapto-code, not someone who reads its source.

- The entry stands alone. The reader has not seen earlier entries, the code or
  the discussion, so it does not lean on terms that were introduced there.
- It says what you saw and what happens now, in the words you would use:
  commands you type, files you edit, messages on the screen.
- No internal names: not the tools the model calls, not libraries, not
  exception codes. The exception is a breaking change to one of them, where
  the name is the point.
- One or two sentences. The cause and the reasoning go in the commit message.

## What counts as a breaking change

tapto-code is a program, not a library, so the surface that has to stay stable
is not a C++ API. It is:

- **command-line flags and subcommands** — one removed or given a different
  meaning
- **config keys** — a key renamed, dropped, or resolved in a different order
- **the tools the model sees** — their names, their parameters, and what a call
  does

That last one is easy to underrate. A model's behaviour is a function of the
tool schema it is given, so renaming a tool or changing what its arguments mean
invalidates every prompt and transcript from before it, in a way no compiler
will report.

The config store at `~/.tapto/config` is **shared with
[tapto-vnc](https://github.com/centlakestefan/tapto-vnc)**, so a change to key
names or resolution order is a breaking change for both programs and should
land in both.

Versions are [Semantic Versioning](https://semver.org/) against that surface.
From 1.0.0 the major number carries breaking changes, the minor number new
features, and the patch number fixes. (Before 1.0.0 the minor number carried
breaking changes and the patch number everything else.)

A build between releases names the release it leads to: `1.3.0-dev.48+gd5e45db`
is 48 commits past the last tag, at commit d5e45db. A release candidate is
`1.3.0-rc.1`, the release itself `1.3.0`. Each sorts before the next, so a
newer build never looks older than the one it replaces.

## [Unreleased]

### Added

- Plugins: tools beyond the core, installed in the `plugins` folder beside
  tapto-code. `/add-plugin <name>` gives the model a plugin's tools that only
  read, `/add-plugin <name> rw` also those that change things, and
  `/list-plugins` and `/remove-plugin <name>` show and take them away.
  `plugin.jira = rw` (or `ro`) in the config loads one at every start, with a
  line of its own for a comment above it, and `plugin.jira = off` in a
  project's config turns off one your own config loads. `/resume` brings back
  a session's plugins. Administrators can limit them with
  the `allowed-plugins` and `allow-user-plugins` policies.
- `command add --note "..."` and `/add-command <name> <cmd> --note "..."`
  attach a short instruction to a command — which arguments it expects, or
  what to pass them. The note is shown to the model alongside the command and
  is never run.
- With a model that can see, `/add-plugin image` (or `plugin.image = ro` in
  the config) lets it look at image files: PNG, JPEG, BMP and GIF.
  Each image is normalised to PNG and its longest side capped at 1600 pixels;
  larger images are down-scaled before being shown.
- `/add-plugin jira` lets the model read your Jira server alongside the code:
  search issues with JQL, read an issue, its project, issue types, sprints and
  transitions, and open an issue in your browser. `/add-plugin jira rw` also
  lets it create and update issues, comment, and move them through the
  workflow. Point it at your server with `config set jira-url <url>` and
  `config set jira-api-token wincred:<name>`; `jira-email`, `jira-project` and
  `jira-api-version` are optional.
- `config set` accepts the settings of an installed plugin, such as
  `jira-url`, and `config list` masks the ones the plugin marks secret.
- `/add-plugin skills` (or `plugin.skills = ro` in the config) gives the model
  the skills you keep in `~/.tapto/plugins/skills/`: a `SKILL.md` in a folder
  of its own, or a `.md` file, with a name and a description. The model sees
  the list and reads a skill when a task matches it, and never changes them.
  `config set skills-folder <path>` uses another folder, and an administrator
  can set it to one the organization keeps.
- `/add-plugin mcp` gives the model the tools of MCP servers: local ones
  tapto-code starts itself and remote ones over HTTPS, listed in
  `~/.tapto/plugins/mcp/servers.json` in the `mcpServers` layout other tools
  use (`config set mcp-servers <path>` names another file).
  Tokens in that file may be `env:`, `cmd:` or `wincred:` references. Loaded
  read-only, only the tools a server marks as read-only are offered; a
  server's entry can say otherwise, or offer only some of its tools.
  Edits to the file are picked up before your next message, starting and
  stopping only the servers you changed. `/mcp list` shows the servers,
  `/mcp reload` restarts them all, and
  `/mcp login <server>` logs in to one that asks for it in your browser; the
  login is kept in Windows Credential Manager and renewed by itself until
  `/mcp logout <server>`.
- A plugin can bring a command of its own, named after it, such as `/mcp`;
  `/help` lists those of the plugins loaded.
- `--session`, `--project` and `--global` on `/cot`, `/effort`,
  `/add-command`, `/add-plugin` and `/remove-plugin` say where the change is
  saved, and `unset` removes a setting there; every reply says where it
  went. `/add-plugin jira --project` loads Jira at every start in this
  project.
- A setting can be for one program alone: `tapto-code.print-cot = false`
  applies to tapto-code and not to tapto-word or tapto-vnc, which share the
  config. `config set` takes such keys.
- A prompt can run over several lines as you type it: end a line in a
  backslash and press Enter to go on to the next line, and press Enter on a
  line without one to send it. A line ending in a Windows path such as
  `C:\temp\` or `\\server\share\` is sent as it is.
- Tab completes as you type at the prompt: the `/` commands (`/add`,
  `/res`, …) and a folder path (`/add-folder ./`, `C:\proj\t`, …). Once
  only one folder matches, it gets a trailing `\` (Windows) or `/` (Linux
  and macOS), so Tab takes you one step at a time. When Tab cannot complete
  any further, it shows the matches below the line.
- The prompt line now has the standard delete keys alongside the cursor
  moves it already had: Ctrl-K clears to the end of the line, Ctrl-U clears
  to the start, Ctrl-W deletes the word before the cursor, and Ctrl-Y puts
  back what you deleted last (several deletions in a row come back as one).
  Ctrl-Left and Ctrl-Right move by the word instead of by the character. A
  long line wraps at a word boundary rather than in the middle of a word.

### Changed

- `tapto-code version`, the banner and the trace name the release a build
  leads to: a build of work since 1.2.0 says `1.3.0-dev.<n>+g<commit>`, where
  it used to say `1.2.0`, and a release candidate says `1.3.0-rc.1`.
- The model's notes per project are now the memory plugin, and are off until
  you turn them on: `plugin.memory = rw` in the config, or `/add-plugin memory
  rw` in a session (`ro` lets the model read its notes but not change them).
  The notes move to `~/.tapto/projects/<project>/plugins/memory/` the first
  time, and administrators can leave the memory out with `allowed-plugins`.
- On Windows the prompt line has the same editing as on Linux and macOS:
  Left and Right, Home and End, Delete, and Up and Down through the prompts
  of this session. A multi-line paste is still sent as one prompt.
- After `--resume` or `/resume`, Up recalls the prompts of the conversation
  you picked up, starting with the one that never got an answer.

### Fixed

- As you typed a prompt, the line could flicker: the redraw was written to
  the terminal in small pieces, and the terminal repainted between them. The
  line is now redrawn in one go, so typing is steady.
- Pressing Up while writing a prompt showed an earlier prompt in its place,
  and the text you had written was gone. Down now brings it back, and a
  recalled prompt you change keeps your changes while you move through the
  others.
- Ctrl-D at an empty prompt ends the session, as it does in a shell; it used
  to do nothing.

- A table the model returned that was wider than your terminal had its rows
  cut off with a `…` where they did not fit, so part of each cell was missing.
  The table now wraps onto extra lines so every cell shows in full, and the
  bold and code formatting inside a cell is kept.

## [1.2.0] — 2026-10-20

### Added

- The model now keeps notes per project in
  `~/.tapto/projects/<project>/memory/`, and the first 50 lines of their
  index, `memory.md`, are shown to it at the start of every session in that
  folder, as its own notes rather than as instructions to follow. It can
  change that folder, and nothing else in `~/.tapto`, only with its file
  editor; open it to see or correct what it has remembered.
- `command add --ro` (and `/add-command --ro`) adds a command that only
  reads, which the model can then also run in, and point at files in, folders
  you granted read-only with `/add-folder`.
- `--resume` and `/resume` now show the end of the conversation you are
  picking up, about one screenful of your prompts and the model's answers,
  so you can see where you left off.

### Changed

- What you type at the prompt is now shown in yellow, so your own lines stand
  out from the model's replies.
- A command whose output was longer than about 16 KB lost everything after
  that. The model now sees the start and the end, and reads the rest from a
  file tapto-code keeps for the session and removes when it ends.

### Fixed

- On Linux and macOS, the arrow keys at the prompt typed `[A`, `[D` and the
  like into your prompt, and sent them to the model. They now move the cursor
  and recall earlier prompts, as in a shell.
- On Linux and macOS, pressing Ctrl-C while the model was working left your
  terminal not showing what you typed until you ran `reset`. The terminal is
  now put back as it was.

## [1.1.0] — 2026-10-04

### Security

- An allow-listed command such as `nmake` could run a `nmake.bat` the model
  had written into your project, instead of the real program. tapto-code now
  finds the program itself, never in a folder the model can write to, and
  says so when it skips one.
- Started in your home folder, tapto-code let the model change its own
  settings and command lists in `~/.tapto`, and so add a command and run it.
  `~/.tapto`, any folder named `.tapto` and the script folders are now
  read-only to the model, as `.git` already was.

### Changed

- **Breaking:** commands are no longer run through a shell, so a command
  line with a pipe, a redirection, `&&` or a built-in such as `dir` or
  `echo` is refused. Put it in a script in
  `%ProgramFiles%\Centlake\tapto\scripts` (your own commands:
  `~/.tapto/scripts`) and allow-list the script's name instead.
- Your administrator can move that folder with the new *Script folder*
  policy.

### Added

- Requests now tell the server which tapto-code version sent them, so an
  administrator whose requests go through a gateway can see who still runs an
  old version, and can turn it away with a message asking them to update.

## [1.0.4] — 2026-09-25

### Security

- The model could change a repository's git files when they were not in a
  folder named `.git` (a linked worktree, a bare repository, or a repository
  made with `--separate-git-dir`). Now it can only read them there too, and
  your allow-listed commands can neither run in them nor be handed a `%p1` or
  `%p*` path inside them.

### Added

- `system-prompt-file` sets the system prompt from a text file, so it can run
  to several lines; `system-prompt-append` and `system-prompt-append-file` add
  your own instructions after the built-in prompt instead of replacing it.
- If a chat ends by accident, with Ctrl-C or a closed window, start
  `tapto-code --resume` or type `/resume` in the same folder to carry on where
  you left off. `/resume` also undoes `/clear`.

### Fixed

- A path argument (`%p1`) given to a command that runs in a subfolder was
  looked up from the folder you started tapto-code in instead, so the command
  could be handed the wrong file, or one that isn't there.
- The model could miss the last line of a file that doesn't end with a
  newline, such as a closing `}`: counting the lines gave one fewer than
  viewing the file showed. Both now agree.
- Showing the start, end or whole of a binary file no longer puts its raw bytes
  into the chat; you get a short message that it is a binary file instead.

## [1.0.3] — 2026-09-21

### Fixed

- After you gave tapto-code access to an extra folder with `/add-folder`, it
  could no longer read, list or search files in the folder you started it in.
  Now it can, and a path without a folder name means the folder you started in.

### Changed

- `/list-folders` shows the folder you started tapto-code in first. That folder
  cannot be removed or made read-only, and `/remove-folder` and `/add-folder`
  now say so.

## [1.0.2] — 2026-09-16

### Fixed

- A user prompt containing an invalid UTF-8 byte — e.g. a pasted or typed
  accented character that the console code page delivered mangled — aborted
  the turn with `json.exception.type_error.316` before the model saw it,
  because the conversation history is serialized with `nlohmann::json`, which
  refuses to emit illegal UTF-8. Tool results were already sanitized on entry
  to the history; user prompts now go through the same normalization at the
  same point.

## [1.0.1] — 2026-09-14

### Fixed

- The `find_files` big-file test wrote its file into a directory an earlier
  block had already removed, so it failed on every host, and the file it
  meant to write was under the 5 MiB cap it exists to disprove. It makes its
  directory and writes past the cap. Test only; the program is 1.0.0's.

## [1.0.0] — 2026-09-14

The first stable release. The flags, config keys and tool schema described
above are now the contract: a change to them is a new major version.

### Added

- Enterprise policy: a read-only `policy` scope above local/global/system, read
  from `HKLM`/`HKCU\SOFTWARE\Policies\Centlake\tapto` on Windows (what Group
  Policy writes from the new `admx/tapto.admx` template) and from
  `/etc/tapto/policy` elsewhere. A key set there wins over every user scope and
  `config set` refuses it; `--policy config list` shows it. Two policy-only
  keys confine `--provider`: `allowed-providers` and `allow-user-providers`.
- Policy can allow-list commands for every user: the `commands` subkey on
  Windows (the template's *Allow-listed commands* setting), the file
  `/etc/tapto/policy-commands` elsewhere. They merge above the user's own
  commands, `command list` shows them as `policy`, and a third policy-only key,
  `allow-user-commands = 0`, makes them the only ones the agent can run.
- With either provider restriction set, a `-provider-url` from a user scope is
  refused, so a permitted block cannot be pointed at a personal endpoint. The
  template's work block requires its URL.

## [0.3.0] — 2026-09-07

### Added

- `/env` — shows the session environment: working directory, provider/model,
  conversation length, context usage (input tokens reported by the provider
  for the last request), tool-iteration cap, and build version. ([e4ed224])
- `/compact` — ask the model to summarize the conversation and restart with a
  compact note, freeing context-window space on long sessions. ([85d1c0c])
- Press `ESC` to interrupt the model mid-response; the partial output is kept
  and control returns to you. ([8649d5c])
- `/cot on|off` — toggle (and persist to the global config) the model's
  chain-of-thought / intermediate output; default is on. `/cot` with no
  argument reports the current state. ([af76f98])
- `<name>-reasoning-effort` config key: the openai dialect sends it as
  `reasoning_effort` on every request, for gpt-5/o-series and for
  OpenAI-compatible servers that accept the same field.
- `/effort <value>` — set the openai-dialect `reasoning_effort` for the
  session and persist it to the local (per-project) scope; `/effort off`
  clears it so the server uses its own default. Restricted to the openai
  dialect — the other two backends never send the field. ([af76f98])
- `run_command` gains an optional `cwd`: run the chosen command in a subdirectory
  of the working folder (e.g. to build in a subfolder). It is resolved against the
  working directory and confined to that subtree, so it can never escape it, and a
  missing directory is refused with a clear error. It also works for the built-in
  commands (wc/head/tail/cat/ls/tree): a relative path in `args` is resolved
  against `cwd`, so `ls` with `cwd sub` and `ls sub` both list that folder.
- `connection-timeout` and `read-timeout` config keys (seconds; defaults 30
  and 300). The reply is not streamed, so `read-timeout` bounds the whole
  generation: raise it for a slow local model. Both keys are shared with
  tapto-word, which already documents them and points users here to set them.
- `/add-folder <path>`, `/remove-folder <path|label>` and `/list-folders`:
  grant the model read-only access to folders outside the working directory.
  While something is granted the model gets `list_folders`, `list_files`,
  `read_file` and `search_files`, confined to the granted roots and unable to
  change a file; the tools and the prompt paragraph naming the roots come
  from libtapto and are the same ones tapto-word offers.
- `/add-folder <path> rw` grants a folder read-write. The working-directory
  tools reach into granted folders by `<label>/<path>` or by absolute path
  under one rule: a read is allowed anywhere granted, a write only where the
  grant is `rw`. The editor's `view`, `find_files` and the built-in `cat`,
  `ls`, `head`, `tail`, `wc` and `tree` work under any grant, so a model that
  reaches for `cat` instead of `read_file` is not sent on a detour; `create`,
  `str_replace` and `insert` need `rw`, with nothing under `.git` either way;
  an allow-listed shell command may take its `cwd` in an `rw` folder and never
  a read-only one. A same-named subfolder of the working directory wins over a
  label. Granting a folder again with the other mode switches it;
  `/list-folders` shows the mode. `find_files` also skips `__pycache__`,
  `.idea` and `build-*` now, the library's noise list.

### Changed

- The shared tapto code — the config store and secret resolver, provider
  resolution, and the three provider clients with the agent loop — now comes
  from **libtapto**, vendored in-tree under `libtapto/` as a byte-identical
  copy of the one in tapto-word. The private copies are gone, and the
  library's unit tests run under `ctest` with this program's. With it come
  the fixes tapto-vnc had made to its copy:
  - Claude requests use adaptive thinking (`thinking: {type: adaptive}`, with
    a summarized display when `print-cot` is on). Before, tapto-code sent no
    thinking field at all, so Claude did not think; turns now take longer and
    cost more, and show their reasoning. The old `budget_tokens` form is
    rejected by Opus 4.7 and later.
  - An openai-dialect server answering 5xx with a body that says the request
    itself is unsupported (`not supported`, `unsupported`, `invalid_request`,
    `does not support`) fails at once with that text instead of being retried
    five times with backoff.
  - Gemini's read timeout was a hardcoded 120 s; it is now the shared
    `read-timeout` (default 300 s). Claude's 300 s is unchanged but configurable.
  - The trace file gains a `usage:` line per request (input, output and, for
    Claude, cache read/write tokens) and the model's closing text.
- `/compact` summarizes a *trimmed* view of the conversation rather than the
  raw one: tool outputs over ~2 KB are swapped for a short placeholder before
  the model is asked to summarize, while the tool calls (which files were read
  or edited) stay. The live conversation is only replaced after a successful
  summary, and short conversations are unchanged. This keeps the largest
  request of the session small and leaves real output-token headroom for the
  summary, so compaction stops ending in a truncated `length` reply.
- The editor tool refuses to create or modify anything under `.git/`. A
  writable `.git/config` or `.git/hooks` would let any allow-listed git command
  run arbitrary code.
- On Linux/macOS the config and commands stores are written owner-only
  (`0600`, directory `0700`), since they may hold a literal API key.

### Fixed

- Tool results containing invalid UTF-8 bytes (e.g. binary file reads, some
  Windows console output) are now normalized before being sent to any backend,
  preventing protocol-level errors. ([9a829f0])
- `run_command` now tolerates the stray top-level `path` field that models
  sometimes send (conflating it with the editor tool): it is treated as the sole
  argument, so `{"name":"ls","path":"driver"}` works the same as
  `{"name":"ls","args":["driver"]}`. When both `args` and `path` are present,
  `args` wins. ([b3417aa])
- On Windows, a parameterized command that falls back to `cmd.exe` (batch
  wrappers such as `npm`, `npx`, `yarn`) now refuses argument values containing
  `"`, `&`, `|`, `<`, `>`, `^`, `%` or a newline. The line was quoted for
  `CreateProcess`, which `cmd.exe` does not honour, so a quote in a value could
  break out and run extra commands.
- `/compact` no longer discards a session when the summarization comes back
  empty. Thinking models (Claude, Gemini) can return the summary inside their
  reasoning/thought blocks, which the reply extraction then dropped — reseeding
  the conversation with an empty note so the model knew nothing of the session.
  The text reply now falls back to the model's reasoning (as the openai dialect
  already did), and if no summary is produced at all the previous conversation
  is kept rather than replaced.
- `/cot on|off` (and any config write) no longer stripped hand-written
  comments and blank lines from the config file on save. The store now keeps
  the file's raw lines and edits only the specific key's line, so everything
  else is left exactly as written. ([af76f98])

- Building on Windows/MSVC: Shining Light's Win64 OpenSSL is found with a
  CMake older than 3.27, which does not search its `lib/VC/x64/MD` layout; a
  host with no OpenSSL at all fetches a pinned, checksummed prebuilt
  (FireDaemon 3.5.4) instead of failing; and the code compiles against
  OpenSSL 4.0, where `X509_get_subject_name()` returns a const pointer.
  ([19c04bc])

## [0.2.0] — 2026-08-14

### Added

- Named provider blocks: a provider has a free-form *name* selecting a block of
  config keys, and a *dialect* named by `<name>-provider-type`. Matches
  tapto-vnc, which reads the same store. ([4f53472])
- Secrets may say where they live instead of holding the value —
  `env:ANTHROPIC_API_KEY`, `cmd:pass show anthropic`,
  `wincred:tapto/work-claude`. A value with no scheme is still the key itself,
  `config list` shows references in full and masks only literal keys, and
  `config set` skips the plaintext warning for one. ([8d5b064])
- Built-in cross-platform commands — `wc`, `head`, `tail`, `cat`, `ls`, `tree`
  — so a task does not depend on what the host shell happens to provide.
  ([6821451])
- Multi-line paste via bracketed paste mode. ([2113e7b], [e37b154])
- Configurable tool-iteration cap (`max-tool-iterations`). ([1c2f762])
- `version` reports the commit the binary was built from alongside the release,
  and every trace opens with the same line.

### Changed

- **Breaking:** an API key configured for the provider block now takes
  precedence over the vendor's environment variable, which previously won. The
  old order sent a real vendor key to whatever `<name>-provider-url` pointed at,
  and a local server logs it. ([4f53472])
- **Breaking:** the unscoped `model`, `provider-url` and `api-key` apply only to
  the default provider. ([4f53472])

An existing store keeps working: `provider-type = claude` is read as the default
provider's name when `provider` is absent.

### Fixed

- Edits to files with CRLF or mixed line endings. ([b58e6b9])
- `find_files` returns paths relative to the sandbox root. ([a7cf47b])
- The status line is clipped by visible width, so ANSI escapes no longer count
  toward it. ([9d7633c])
- `<cstdint>` in `ui.cpp`, for `uint32_t` on Linux. ([046cdcd])

## [0.1.0] — 2026-06-30

Initial release: an agent that reads, writes and runs code in a sandboxed
project directory, against Claude, OpenAI or Gemini. ([d9ac02c])

[Unreleased]: https://github.com/centlakestefan/tapto-code/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/centlakestefan/tapto-code/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/centlakestefan/tapto-code/compare/v1.0.4...v1.1.0
[1.0.4]: https://github.com/centlakestefan/tapto-code/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/centlakestefan/tapto-code/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/centlakestefan/tapto-code/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/centlakestefan/tapto-code/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/centlakestefan/tapto-code/compare/v0.3.0...v1.0.0
[0.3.0]: https://github.com/centlakestefan/tapto-code/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/centlakestefan/tapto-code/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/centlakestefan/tapto-code/releases/tag/v0.1.0
[d9ac02c]: https://github.com/centlakestefan/tapto-code/commit/d9ac02c
[046cdcd]: https://github.com/centlakestefan/tapto-code/commit/046cdcd
[1c2f762]: https://github.com/centlakestefan/tapto-code/commit/1c2f762
[9d7633c]: https://github.com/centlakestefan/tapto-code/commit/9d7633c
[a7cf47b]: https://github.com/centlakestefan/tapto-code/commit/a7cf47b
[2113e7b]: https://github.com/centlakestefan/tapto-code/commit/2113e7b
[6821451]: https://github.com/centlakestefan/tapto-code/commit/6821451
[e37b154]: https://github.com/centlakestefan/tapto-code/commit/e37b154
[4f53472]: https://github.com/centlakestefan/tapto-code/commit/4f53472
[8d5b064]: https://github.com/centlakestefan/tapto-code/commit/8d5b064
[b58e6b9]: https://github.com/centlakestefan/tapto-code/commit/b58e6b9
[85d1c0c]: https://github.com/centlakestefan/tapto-code/commit/85d1c0c
[8649d5c]: https://github.com/centlakestefan/tapto-code/commit/8649d5c
[9a829f0]: https://github.com/centlakestefan/tapto-code/commit/9a829f0
[b3417aa]: https://github.com/centlakestefan/tapto-code/commit/b3417aa
[e4ed224]: https://github.com/centlakestefan/tapto-code/commit/e4ed224
[af76f98]: https://github.com/centlakestefan/tapto-code/commit/af76f98
