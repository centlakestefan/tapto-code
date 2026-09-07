# tapto-code: adopt libtapto

The shared tapto code now lives in one place: `../tapto-word/libtapto`, cut on
5 Sep 2026 from tapto-code (baseline) plus tapto-vnc's fixes, and built as one
static library. tapto-word's todo.md, section 1, lists "Port: tapto-code" as
the open item. This is that port.

Tick boxes are the unit of work. Steps 0-5 are done and committed (7 Sep);
the follow-ups at the end are open.

---

## Where things stand (measured 6 Sep)

`diff -w --strip-trailing-cr` of tapto-code's current tree against the library
(the raw diffs are mostly CRLF noise: tapto-code is checked out CRLF, the
library is LF via `.gitattributes`):

| file | state |
|---|---|
| `config.h/.cpp`, `secret.h/.cpp`, `log.h`, `cancel.h`, `context.h`, `encoding.h/.cpp`, `openai.h`, `gemini.h` | identical |
| `claude.h` | library drops the two timeout constants; they come from `AiConfig` now |
| `paths.h/.cpp` | library adds `global_dir()` |
| `aiconfig.h` | library adds `connectionTimeoutSeconds`/`readTimeoutSeconds` (dialect-neutral names, old `openai*` names kept as aliases), `effort` (Claude `output_config.effort`), `keepRecentImages` |
| `aibackend.h` | **library is missing `buildTrimmedHistoryForSummary()`** and `kCompactTrimPayloadChars`, the /compact trimmer from 0335060 (26 Aug). The merge said "union of both" but this one fell through. `tests/test_compact.cpp` covers it |
| `tool_registry.h` | library replaces the fixed display-name table with a per-tool hook `ToolSpec::display`; `getToolDisplayName()` now takes the tool table. Adds `tapto::ConnectionLost` |
| `ui.h` | library keeps the 7 functions the provider clients call; tapto-code's other 18 (banner, help, setup prompts, config/command listing, prompt, reply) are the program's own |
| `claude.cpp` | adaptive thinking (`{"type":"adaptive"}` + `output_config.effort`; the old `budget_tokens` form 400s on Opus 4.7+), image blocks in tool results, image pruning with cache-breakpoint pull-back, `ConnectionLost` passthrough, usage/cache logging, final text logged |
| `openai.cpp` | reasoning fields as a table (`reasoning_content`, then `reasoning`; same set as 0b2a0c6, other order), images as a user turn, permanent-error detection on 5xx ("not supported", "unsupported", "invalid_request", "does not support" are not retried), usage logging, final text logged |
| `gemini.cpp` | `thinkingLevel` instead of `thinkingBudget`, inline images, `ConnectionLost`, timeouts from config |
| new in the library | `provider.h/.cpp` (lifted from main.cpp 410-605), `base64.h`, `tool_image.h/.cpp`, `fstools.h/.cpp`, `certs.h/.cpp`, `test/test_libtapto.cpp`, `test/ui_null.cpp` |

Two facts that shape the steps:

- **Toolchains differ.** tapto-code builds with MSVC (Ninja + cl.exe, `/W4`);
  tapto-word builds with Strawberry MinGW. libtapto has never been compiled by
  MSVC. Expect a warning or two to fix upstream.
- **`first_run_setup()` needs four functions provider.cpp keeps private**:
  `default_provider_name`, `provider_dialect`, `api_key_env_var`,
  `resolve_api_key`. They sit in provider.cpp's anonymous namespace because
  tapto-word never runs setup.

## Decision

Vendor by copy, byte-identical to `../tapto-word/libtapto/`. The invariant after
every step is

```sh
diff -r ../tapto-word/libtapto libtapto     # empty
```

Library changes go into tapto-word's copy first (it has the library tests and
is where the merge was verified), get committed there, then are copied here.
The final home (own repo + submodule, or FetchContent from a tag) is tapto-word's
open question and is decided after tapto-vnc's port, not now.

Boundary rule, unchanged from the merge: the library holds what is
program-independent. `main.cpp`, `tools.cpp`, `commands.cpp`, `ui.cpp`, the
config CLI and its key validation stay here.

---

## Steps

### 0. Upstream first, in `../tapto-word/libtapto`

- [x] `aibackend.h`: restore `buildTrimmedHistoryForSummary()` and
      `kCompactTrimPayloadChars` from tapto-code's copy, verbatim. It is
      header-only and already handles all three history shapes.
- [x] `test/test_libtapto.cpp`: add a trimmer test (port the stub-backend cases
      from `tests/test_compact.cpp`; three shapes, under/over the limit, live
      history untouched). tapto-code's `test-compact` can then go, or stay as a
      second run of the same thing.
- [x] `provider.h`: export `default_provider_name()`, `provider_dialect()`,
      `api_key_env_var()`, `resolve_api_key()`. Move them out of the anonymous
      namespace; no body changes.
- [x] Build tapto-word, run `test-libtapto`, commit. Done 6 Sep as tapto-word
      commit c50a820.

### 1. Vendor

- [x] `cp -r ../tapto-word/libtapto libtapto` (all of it, `test/` included).
- [x] Add `.gitattributes` with `libtapto/** text=auto eol=lf` so the copy stays
      LF under `core.autocrlf=true` and `diff -r` stays clean. Repo-wide LF
      normalisation is a separate, noisy commit; not now.
- [x] Delete the private copies:
      `include/tapto/{aibackend,aiconfig,cancel,claude,config,context,encoding,gemini,log,openai,paths,secret,tool_registry,ui}.h`
      and `src/{claude,openai,gemini,config,paths,secret,encoding}.cpp`.
      `ui.h` in particular must go: `include/` is searched before the library's
      include dir, so a stale copy would shadow the library's header silently.
- [x] What remains under `include/tapto/`: `commands.h`, `tools.h`, and the new
      `termui.h` (step 2). Under `src/`: `main.cpp`, `commands.cpp`,
      `tools.cpp`, `ui.cpp`.
- [x] `diff -r ../tapto-word/libtapto libtapto` is empty.

### 2. Program side

- [x] **`include/tapto/termui.h`** (new): the 18 declarations the library's
      `ui.h` dropped -- `print_reply`, `print_usage`, `print_banner`,
      `print_chat_header`, `print_chat_hints`, `print_help`, the four
      `print_setup_*`, `print_prompt`, `print_prompt_accepted`,
      `print_config_entry`, `print_command_entry`, `print_command_added`,
      `print_command_removed`, `print_no_commands`. Same `tapto::ui` namespace,
      same bodies in `ui.cpp`; `ui.cpp` includes both headers, `main.cpp`
      includes `termui.h`. This is what tapto-word did with `paneui.h`.
- [x] **`main.cpp`**: delete the provider block (the anonymous-namespace
      functions from `get_effective` at ~410 through `provider_label` at ~605,
      and `effective_config` at ~130) and `#include "tapto/provider.h"`. Keep
      `has_suffix`, `provider_name_of_key`, `kProviderBlockKeys`,
      `is_valid_provider_name`, `is_supported_config_key`, `is_api_key_key`:
      they serve the config CLI, which is this program's. (`has_suffix` and
      `provider_name_of_key` are duplicated privately in provider.cpp; ten
      lines, leave it.)
- [x] **`tools.cpp`**: set `ToolSpec::display` on each of the four tools with
      the label logic from the old `getToolDisplayName` table, verbatim --
      `View src/foo.cpp:10-50`, `Edit src/foo.cpp`, `Search *.cpp ~"q"`,
      `Run build arg1`. Without this the status line regresses to raw tool
      names and nothing fails. Add a case to `tests/test_edits.cpp` that calls
      `getToolDisplayName(builtin_tools(), ...)` for each of the four.
- [x] **Timeouts as config**: read `connection-timeout` (default 30) and
      `read-timeout` (default 300) in `cmd_chat` and set them on `AiConfig`
      with the dialect-neutral setters; add both to `is_supported_config_key`,
      `kSupportedKeysHelp`, the usage text and the README. tapto-word already
      documents these keys as shared and points users at tapto-code to set
      them, so today `tapto-code config set read-timeout 1800` is refused.
      Keep calling `setOpenaiReasoningEffort`: the openai dialect reads
      `openaiReasoningEffort()`, Claude reads `effort()`. Leave `effort` unset
      (server default). Unifying the two knobs is tapto-word's open question,
      not this port.
- [x] **Claude thinking**: tapto-code never set a thinking budget, so the old
      client sent no `thinking` field and Claude did not think. The library
      sends `{"type":"adaptive"}` (plus `display: summarized` when `print-cot`
      is on) unless the budget is exactly 0. That is a behaviour change for
      every Claude user: slower and costlier turns, reasoning shown in the
      terminal. Accept it -- it is the current API and tapto-vnc/tapto-word
      already run this way -- and say so in the CHANGELOG. A `thinking = off`
      key can come later if someone asks.
- [x] `tests/test_compact.cpp`: deleted; `test-libtapto` runs the same cases.
      Step 3 drops the CMake target. Also found: `tools.h` included
      `tool_registry.h` by sibling path, now `tapto/tool_registry.h`.

### 3. CMake

- [x] `add_subdirectory(libtapto)` replaces the two `FetchContent_Declare`
      blocks, `find_package(OpenSSL)`, the `CPPHTTPLIB_OPENSSL_SUPPORT`
      definition and the `advapi32` link: the `tapto` target exports all of
      them PUBLIC. Keep the version stamp block as is.
- [x] `add_executable(tapto-code src/main.cpp src/commands.cpp src/tools.cpp
      src/ui.cpp)`, `target_link_libraries(tapto-code PRIVATE tapto::tapto)`,
      include dirs `include` and the generated dir.
- [x] `test-edits`: sources `tests/test_edits.cpp src/tools.cpp
      src/commands.cpp` plus `libtapto/test/ui_null.cpp`, link `tapto::tapto`.
      (`config.cpp`/`paths.cpp` come from the library now. `ui_null.cpp`
      because a static library only pulls the objects a test references, and
      the day a test touches provider.cpp it would otherwise fail to link on
      `tapto::ui::print_error`.)
- [x] `test-compact` (if kept): link `tapto::tapto` for the include path.
- [x] `set(LIBTAPTO_BUILD_TESTS ON)` before the `add_subdirectory`, so
      `ctest` here runs `test-libtapto` against the vendored copy under MSVC.
      That is the only MSVC coverage the library gets.
- [x] `tapto-code.vcxproj` still lists `include\minicode\*.h` from before the
      June rename, so it is already dead. Out of scope; delete it in its own
      commit or leave it.

### 4. Verify

- [x] `cmake -S . -B build && cmake --build build` under MSVC: clean at `/W4`.
      Anything the library trips on is fixed in `../tapto-word/libtapto`,
      committed there, re-copied (the invariant).
- [x] `ctest --test-dir build --output-on-failure`: `edits`, `compact` (if
      kept), `libtapto`.
- [x] `tapto-code config list`, `config set read-timeout 900`, `config get`,
      `config unset`, `command list`: the CLI path through the exported
      `effective_config`.
- [x] Setup path: move `~/.tapto/config` aside, run `tapto-code chat`, walk
      the first-run prompts, confirm the key lands in the global store.
- [x] One real turn per dialect (claude, a local openai-compatible server,
      gemini) with `trace-file` set: status line shows `Edit src/...` not
      `str_replace_based_edit_tool`; `/compact` runs and the trace shows the
      trimmed placeholders; ESC interrupts; `/env` reports input tokens; the
      trace has the new `usage:` lines.
- [x] `git diff --stat`: only `libtapto/` added, the 21 duplicates deleted,
      `CMakeLists.txt`, `main.cpp`, `tools.cpp`, `ui.cpp`, `termui.h`,
      `test_edits.cpp`, `.gitattributes`, README, CHANGELOG.


Notes from steps 3 and 4 (6 Sep):

- MSVC found two things in the library, both fixed upstream and re-copied:
  `certs.cpp` used `fopen` (C4996 at /W4, tapto-word 1ab7ba2), and then
  `test_certificates` died with 0xC0000409 because the program's UCRT opened
  a `FILE*` that Strawberry's MinGW-built libcrypto DLL then wrote through.
  PEM now goes through memory BIOs and the library's own file I/O
  (tapto-word d9e5cd4). Any program linking an OpenSSL from another
  toolchain was exposed to the same crash.
- Verified: `edits` (49 checks) and `libtapto` pass under MSVC; the config
  CLI incl. the new timeout keys and the refusal of `word-port`; first-run
  setup against a scratch `USERPROFILE`; one tool-using turn each on
  qwen38_gb10 (openai), claude and gemini with `View src/hello.cpp` on the
  status line; `/env` reporting input tokens; `/compact` trimming a 7 KB
  input to ~500 bytes; Claude's `cache_read` non-zero on the second request.
- Not verified: ESC interrupt (needs a console, not a pipe). The Claude
  request body is not in the trace, so adaptive thinking is inferred from
  the request succeeding on claude-sonnet-5, not read back.

### 5. Docs and version

- [x] README "Dependencies": nlohmann/httplib/OpenSSL now arrive through
      `libtapto/CMakeLists.txt`; a "libtapto" section names the library, what
      it holds, and that tapto-word shares it. `connection-timeout` and
      `read-timeout` are in the config-key table.
- [x] CHANGELOG `[Unreleased]`: Added -- the two timeout keys. Changed -- the
      move to libtapto, with the four user-visible consequences (adaptive
      thinking on Claude, permanent 5xx not retried, Gemini read timeout,
      trace usage lines).
- [x] One commit for the port, docs included (7 Sep). The version stays
      0.2.0: a release here is its own "Version x.y.z" commit plus a tag, and
      `[Unreleased]` already holds a month of other changes, so the number is
      a call to make over all of them. The port itself touches nothing on the
      breaking surface (flags, keys, tool schemas); on its own it would be
      0.2.1.

---

## Behaviour changes a tapto-code user will notice

| area | before | after |
|---|---|---|
| Claude | no `thinking` field | adaptive thinking, summarized reasoning shown when `print-cot` is on |
| Gemini read timeout | 120 s hardcoded | 300 s default, `read-timeout` key |
| Claude read timeout | 300 s hardcoded | same value, now configurable |
| openai 5xx with "not supported" etc. | retried 5 times with backoff | fails at once with the server's text |
| openai reasoning field | `reasoning` checked before `reasoning_content` | the other order; same fields |
| trace file | tool calls and responses | plus `usage:` per request and the model's closing text |
| status line | `Edit src/foo.cpp` | identical, provided step 2's hooks are ported |

## Follow-ups, not this port

- `config set` refuses sibling programs' keys (`word-port`, `word-cert`, and
  whatever tapto-vnc adds), while tapto-word tells its users to set config
  with tapto-code. Either tapto-code learns a pass-through for known program
  prefixes, or each program grows a `config set` of its own.
- One thinking knob across dialects (`<name>-effort` mapped per dialect)
  instead of `effort` + `reasoning-effort`. Open in tapto-word's todo.
- Final home of libtapto once tapto-vnc is ported: own repository consumed as
  a submodule or via FetchContent; then the copies here and in tapto-word go.
- Repo-wide `* text=auto eol=lf`, as tapto-word has, in a commit of its own.
- tapto-jira: not on this machine; its drift is unmeasured.
- `config list` masks only `api-key` values. Other secrets the shared store
  holds for sibling programs (tapto-vnc's `vcenter-password`) print in clear.
