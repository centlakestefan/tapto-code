# Changelog

Notable changes to tapto-code. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each entry is one line
about what changed for you, and the commit it links to carries the reasoning.

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
Before 1.0.0 the minor number carries breaking changes, and the patch number
carries everything else.

## [Unreleased]

### Added

- `/env` — shows the session environment: working directory, provider/model,
  conversation length, tool-iteration cap, and build version.
- `/compact` — ask the model to summarize the conversation and restart with a
  compact note, freeing context-window space on long sessions. ([85d1c0c])
- Press `ESC` to interrupt the model mid-response; the partial output is kept
  and control returns to you. ([8649d5c])
- `<name>-reasoning-effort` config key: the openai dialect sends it as
  `reasoning_effort` on every request, for gpt-5/o-series and for
  OpenAI-compatible servers that accept the same field.
- `run_command` gains an optional `cwd`: run the chosen command in a subdirectory
  of the working folder (e.g. to build in a subfolder). It is resolved against the
  working directory and confined to that subtree, so it can never escape it, and a
  missing directory is refused with a clear error. It also works for the built-in
  commands (wc/head/tail/cat/ls/tree): a relative path in `args` is resolved
  against `cwd`, so `ls` with `cwd sub` and `ls sub` both list that folder.

### Changed

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

[Unreleased]: https://github.com/centlakestefan/tapto-code/compare/v0.2.0...HEAD
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
