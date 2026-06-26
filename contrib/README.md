# Starter commands

[`commands`](commands) is a curated, security-conscious set of common
development commands for mini-code's `run_command` tool, so you don't have to
build your allow-list from scratch.

## Design

- **Scoped, not broad.** Each entry runs one specific thing (`git-commit`,
  `npm-build`, `cmake-build`), rather than a wildcard passthrough like
  `git %*`. The agent picks a name; it cannot choose arbitrary subcommands or
  flags.
- **Placeholders only at data positions.** The only placeholder used is the
  commit message (`git-commit = git commit -m %1`). Because parameterized
  commands are passed as a literal argv vector (no shell), the value can't turn
  into a flag — `git-commit` can never become `git push --force`.
- **Terminating commands only.** `run_command` waits for the process to exit, so
  long-running commands (dev servers, `--watch`) would hang. They're left out on
  purpose — add your own only if you can bound them.

## Install

These go in the **global** scope (your home folder), so they're available in
every project.

**New setup** — copy the file:

```sh
# Linux / macOS
mkdir -p ~/.minicode && cp commands ~/.minicode/commands

# Windows (PowerShell)
New-Item -ItemType Directory -Force "$env:USERPROFILE\.minicode" | Out-Null
Copy-Item commands "$env:USERPROFILE\.minicode\commands"
```

**Already have commands** — don't overwrite; append the lines you want, or add
them individually (this never clobbers existing entries):

```sh
mini-code --global command add npm-build npm run build
mini-code --global command add git-status git status
```

Then check what's active:

```sh
mini-code command list
```

## Customize

Treat this as a starting point — review it, delete what you don't use, and add
commands for your own stack. They run real programs on your machine, so only
keep what you're comfortable letting the agent run unattended.
