# Starter commands

[`commands`](commands) is a curated, security-conscious set of common
development commands for tapto-code's `run_command` tool, so you don't have to
build your allow-list from scratch.

## Design

- **Scoped, not broad.** Each entry runs one specific thing (`git-commit`,
  `npm-build`, `cmake-build`), rather than a wildcard passthrough like
  `git %*`. The agent picks a name; it cannot choose arbitrary subcommands or
  flags.
- **Placeholders only at data positions.** Where a value is needed it's always a
  data position, never a flag/subcommand: a commit message
  (`git-commit … -m %1`) or a sandbox-checked path (`git-add … %p1`). Because
  parameterized commands are passed as a literal argv vector (no shell), a value
  can't turn into a flag — `git-commit` can never become `git push --force`. And
  a `%p` path is validated to be inside the launch folder before the command
  runs, so `git-add` stays within the sandbox (unlike `git add -A`, which would
  stage the whole repository).
- **Terminating commands only.** `run_command` waits for the process to exit, so
  long-running commands (dev servers, `--watch`) would hang. They're left out on
  purpose — add your own only if you can bound them.

## Install

These go in the **global** scope (your home folder), so they're available in
every project.

**New setup** — copy the file:

```sh
# Linux / macOS
mkdir -p ~/.tapto && cp commands ~/.tapto/commands

# Windows (PowerShell)
New-Item -ItemType Directory -Force "$env:USERPROFILE\.tapto" | Out-Null
Copy-Item commands "$env:USERPROFILE\.tapto\commands"
```

**Already have commands** — don't overwrite; append the lines you want, or add
them individually (this never clobbers existing entries):

```sh
tapto-code --global command add npm-build npm run build
tapto-code --global command add git-status git status
```

Then check what's active:

```sh
tapto-code command list
```

## Customize

Treat this as a starting point — review it, delete what you don't use, and add
commands for your own stack. They run real programs on your machine, so only
keep what you're comfortable letting the agent run unattended.
