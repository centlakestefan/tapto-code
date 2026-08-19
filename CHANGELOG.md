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

- `<name>-reasoning-effort` config key: the openai dialect sends it as
  `reasoning_effort` on every request, for gpt-5/o-series and for
  OpenAI-compatible servers that accept the same field.

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
