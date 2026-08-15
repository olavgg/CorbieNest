# Corbie Nest

A Claude-Code-style terminal coding agent written in plain C that talks to
**local Ollama models**. It streams responses, lets the model read/write/edit
files, grep the codebase, and run shell and git commands — each mutating action
is shown to you and confirmed before it runs.

```
╭──────────────────────────────────────────────────────────────╮
│ 🐦‍⬛ Corbie Nest v0.1.0 — local coding agent for Ollama
│ model: qwen3-coder:30b
│ host:  http://127.0.0.1:11434 ● connected
│ cwd:   ~/projects/myapp
╰──────────────────────────────────────────────────────────────╯
› fix the failing test in tests/test_parser.py

● bash(python3 -m pytest tests/test_parser.py -q)
  $ python3 -m pytest tests/test_parser.py -q
  Run this command?  yes
  ⎿  F.
     … +12 lines
● read_file(src/parser.py)
● edit_file(src/parser.py)
  - if tok == "":
  + if tok == "" or tok is None:
  Apply this edit?  yes
● bash(python3 -m pytest tests/test_parser.py -q)
  ⎿  ..
     exit code: 0
● Fixed the None handling in `src/parser.py:42`; both tests pass.
  ↳ 4120 in · 210 out · 62.3 tok/s · 4.1s · ctx 12%

›


 ⏵  manual mode (shift+tab to cycle) │ qwen2.5-coder:7b │ 4.3k tokens (↑4.1k ↓210) │ ctx 12%
```

corbienest takes over the terminal (alternate screen) while it runs, like Claude Code: the
conversation scrolls in the upper part and the bottom row is a status bar showing the
permission mode, the model, the tokens used this session (↑ prompt · ↓ generated, ticking
while the model streams) and context usage. Whenever the app is waiting on something — the
model loading or generating, a tool's shell command, an Ollama request — an animated
spinner with what it is waiting for (`⠋ generating`, `⠹ running command`, …) appears at the
right end of the bar, and long waits also get an inline spinner with the elapsed time. On
exit your shell is back exactly as it was (use `/save` to keep a transcript).

## Build

Dependencies: a C11 compiler, `make`, and [cJSON](https://github.com/DaveGamble/cJSON)
(`sudo apt install libcjson-dev` on Debian/Ubuntu, `brew install cjson` on macOS).
No libcurl, no ncurses — HTTP is done over plain POSIX sockets and the UI uses ANSI escapes.

```sh
make
./corbienest                 # run in the current directory
make release               # optimized, stripped binary (no debug info) — clean rebuild
sudo make install          # optional: installs to /usr/local/bin
```

You need [Ollama](https://ollama.com) running (`ollama serve`) with at least one model
pulled. For the agentic features (files/shell/git) pick a model with **tool** support,
e.g. `ollama pull qwen3-coder:30b`, `qwen2.5-coder`, `devstral`, `llama3.1`, `gpt-oss`.

## Usage

```
corbienest [options] [-p PROMPT]
  -m, --model NAME     model to use (default: saved / first tool-capable model)
  -H, --host URL       ollama host (default $OLLAMA_HOST or http://127.0.0.1:11434)
  -c, --ctx N          context window (num_ctx, default 32768; accepts 64k, 128k, default)
  -s, --system TEXT    extra system instructions
  -p, --prompt TEXT    non-interactive: run one prompt and exit (add --yolo to allow tools)
  -y, --yolo           auto-approve tool calls (same as --mode auto)
      --mode NAME      permission mode: manual, accept-edits, plan, auto
  -T, --no-tools       disable tool calling
      --continue       resume the latest session started in this directory
  -r, --resume [ID]    resume a session: by ID, or pick one from a menu
      --think / --no-think / --show-thinking
```

### Slash commands

| command | what it does |
|---|---|
| `/model` | interactive model picker (↑/↓, type to filter, Enter) — `/model NAME` switches directly |
| `/models` | list installed models with tool/thinking capabilities |
| `/clear` | new conversation |
| `/compact` | have the model summarise the conversation to free context |
| `/memory [on\|off\|clear]` | show the project memory (`.corbienest/memory.md`), toggle its automatic update, or delete it |
| `/status` | model, context usage, settings |
| `/cost` | tokens, model calls, tool calls, model time and wall time of this session |
| `/system [text\|clear]` | extra system instructions |
| `/think on\|off\|auto`, `/think show\|hide` | control thinking on thinking-capable models |
| `/permissions [add …\|remove N\|clear]` | the project's saved "always allow" rules (`.corbienest/permissions`) |
| `/mode [name]` | permission mode: `manual`, `accept-edits`, `plan`, `auto` (Shift+Tab cycles) |
| `/yolo [on\|off]` | shortcut for `/mode auto` / `/mode manual` (careful) |
| `/init` | have the model explore the project and write a `CORBIENEST.md` (build/test commands, architecture, conventions); improves an existing one |
| `/skills [reload\|new NAME]` | list skills; run one with `/NAME [args]` |
| `/tools on\|off` | enable/disable tools |
| `/ctx [N\|Nk\|max\|default]` | context window: no argument opens a size picker (up to the model's trained maximum), `/ctx 64k`, `/ctx max` … set it directly |
| `/temp X`, `/host URL` | tuning |
| `/save [file]` | save transcript as markdown |
| `/resume [ID\|all]` | pick an earlier session to continue (this directory; `all` for every directory), or load one by ID |
| `/history [N]` | show the last N queries (default 20; the latest 100 are kept across sessions, ↑/↓ recalls them) |
| `/cd DIR`, `/pwd` | change working directory |
| `/quit` | exit (also Ctrl-D) |

### Input tricks

- `!cmd` — run a shell command yourself; its output is added to the conversation.
- `@path` — attach a file (or a directory listing) to your message.
- `# fact` — remember something: a menu asks which section of `.corbienest/memory.md` it belongs
  to (Project / User / Feedback / Reference) and the line is appended, no model call involved.
- Enter sends; Alt+Enter, Ctrl+J or a trailing `\` inserts a newline. Bracketed paste works.
- Ctrl-C (or Esc) cancels a running generation / clears the line (twice on an empty line quits).
- **You can keep typing while the model works.** What you type shows up in the status bar as
  you go (`› …▏`); press Enter to queue it as a message — add details,
  narrow the request, change the spec — the status bar shows `N queued`, and the message is
  delivered at the next opportunity: between tool rounds (so the model sees it before its next
  step, alongside the tool result) or as the next turn once the current one finishes. Queue as
  many as you like; they are sent in order. Ctrl-C hands queued text back to the editor instead
  of sending it. Text without Enter simply reappears in the prompt afterwards.
- Tab completes slash commands and skill names; ↑/↓ browse history — the latest 100 queries are
  kept in `~/.config/corbienest/history` (`/history` lists them). Ctrl-R searches it
  incrementally (`(reverse-i-search)`, like bash): type to refine, Ctrl-R again for an older
  match, Enter keeps the match in the editor, Esc restores what you had.
- Shift+Tab cycles the permission mode, also while the model is working (it applies to the
  remaining tool confirmations of that turn); the current mode is shown in the bottom status bar.

### Permission modes

| mode | file edits (`write_file`, `edit_file`) | shell (`bash`) | notes |
|---|---|---|---|
| `manual` (default) | ask | ask | |
| `accept-edits` | auto-approved | ask | |
| `plan` | not offered / denied | ask | the model is told to explore and present a plan instead of changing things |
| `auto` | auto-approved | auto-approved | the old `/yolo`; use with care |

Cycle with **Shift+Tab** at the prompt, or set one with `/mode NAME`, `--mode NAME`
(`--yolo` = `--mode auto`). The mode is saved to the config file.

### Tools the model can call

| tool | confirmation |
|---|---|
| `read_file(path, offset?, limit?)` | none |
| `list_dir(path?)` | none |
| `grep(pattern, path?, include?)` | none |
| `write_file(path, content)` | yes — shows a preview |
| `edit_file(path, old_string, new_string, replace_all?)` | yes — shows a diff |
| `bash(command, timeout?)` | yes — shows the command; output/exit code returned to the model |

Each confirmation is a small menu:

```
  Run this command?
  ❯ 1. Yes  (y)
    2. Yes, and don't ask again for shell commands this session  (a)
    3. Yes, and always allow `git status …` in this project  (p)
    4. No, and tell the model what to do instead  (n)
  ↑/↓ or j/k move · enter select · 1-4 / y / a / p / n · esc = no
```

Move with ↑/↓ (or j/k) and press Enter, or hit `1`–`4` or the `y`/`a`/`p`/`n` shortcuts;
Esc or Ctrl-C means no. `p` saves a rule to `.corbienest/permissions` (like Claude Code's
project allow-list): `edit` allows file writes/edits, `bash git status` allows shell commands
that start with those words (`git status --short` yes, `git status; rm -rf /` no — commands
with `; | & $ \` < >` never match a rule). `/permissions` lists the rules; `/permissions add
bash make`, `/permissions remove N`, `/permissions clear` manage them by hand. On "no" you can type what the model should do instead (Enter to skip)
and it is sent back as the tool result. Anything you typed while the model was still generating
is never taken as an answer — it is kept for your next prompt. Ctrl-C while a command runs kills it.

Some local models occasionally emit their native tool syntax as plain text
(e.g. `<function=grep>…`) when Ollama's parser fails; corbienest recognises the
Qwen XML and Hermes `<tool_call>{json}</tool_call>` shapes and still executes them.

### Skills

A skill is a reusable instruction file, `SKILL.md`, with a short frontmatter:

```
---
name: review
description: Review the given files for bugs and style problems
---
Review these files carefully: $ARGUMENTS
Report findings as a list.
```

Put project skills in `.corbienest/skills/NAME/SKILL.md` (or `NAME.md`; `.claude/skills/` is
read too), personal ones in `~/.config/corbienest/skills/`. Run one with `/NAME args` —
`$ARGUMENTS` is replaced by the arguments — or from the shell with `-p "/NAME args"`.
`/skills` lists them, `/skills reload` rescans, `/skills new NAME` scaffolds one. The
available skills are also listed in the system prompt so the model can pick one up itself.

Skills may ship helper programs next to `SKILL.md`. Write those in **C** (build with `cc`
or a small Makefile in the skill directory); use Rust only when a third-party library is
genuinely needed, and Python only as a last resort. The model is told the same rule when
asked to create a skill.

### Context window

The status bar shows how much of the context window (`num_ctx`, default 32k) the last request
used; the footer under a reply warns when it is nearly full. To grow it, run `/ctx` for a
picker of sizes up to the model's trained maximum (read from `ollama show`), or set one directly:
`/ctx 64k`, `/ctx 131072`, `/ctx max`, `/ctx default`. `/status` shows the model's maximum.
Larger windows need more RAM/VRAM and take effect on the next request; `/compact` is the other
way out when a long session fills up. Once a request has used 95% or more of the window,
corbienest compacts automatically before the next model call (also mid-task, between tool rounds).

### Sessions

Every conversation is saved after each request to `~/.config/corbienest/sessions/<id>.json`
(the latest 100 are kept). `corbienest --continue` picks up the most recent session started in
the current directory, `--resume` opens a picker (or `--resume ID`), and `/resume` does the same
from inside a session — the recap shows the first request and the last reply. `/clear` starts a
new session; `/status` shows the current id, which is also printed when you quit.

### Project memory

Like Claude Code's auto-memory: corbienest keeps `.corbienest/memory.md` in the working
directory and loads it into the system prompt of every request. The model curates it — at the
end of each request a quiet extraction call asks whether the exchange revealed anything durable
(who you are and how you like to work, feedback you gave, project goals/decisions/constraints
that aren't in the code, references such as URLs or tickets) and, if so, rewrites the file;
otherwise nothing is written. Facts the repository already records, and anything only relevant
to the current conversation, are deliberately not saved. Type `# fact` to add something yourself
in one keystroke, ask the model to remember
or forget something (it edits the file with `edit_file`), or edit the file by hand. `/memory`
prints it, `/memory off` (saved to config) or `--no-memory` disables the update, `/memory clear`
deletes it. One-shot runs (`-p`) only touch the file if `.corbienest/` already exists.

### Project instructions

If a `CORBIENEST.md`, `CLAUDE.md` or `AGENTS.md` exists in the working directory it is
appended to the system prompt, so you can give the model project-specific guidance.

### Config

Settings changed with `/model`, `/ctx`, `/think`, `/mode`, `/yolo`, `/host`, `/memory on|off` are saved to
`~/.config/corbienest/config`. Environment: `OLLAMA_HOST`, `CORBIENEST_MODEL`.

## Tests

```sh
make test
```

- `tests/test_unit.c` — C unit tests: string buffer, file helpers, the HTTP client
  (chunked/content-length/abort against a forked local server), the streaming
  markdown printer, text tool-call recovery, every tool (read/write/edit/list/grep/bash
  including timeouts and non-interactive denial), permission modes, and skills
  (frontmatter parsing, `$ARGUMENTS`, scaffolding).
- `tests/test_integration.py` — runs the real binary against `tests/fake_ollama.py`,
  a tiny scripted Ollama stand-in (no model needed): one-shot mode, the full tool loop
  with results fed back, `--yolo`, XML tool-call recovery, `@file`, errors, and — through a
  pseudo-terminal — the editor (cursor keys, history, multi-line, paste, type-ahead),
  the confirmation menu (arrow keys, deny with reason, always, stray keys ignored),
  messages queued with Enter while the model streams or a tool runs (delivered between tool
  rounds / after the turn, handed back on Ctrl-C), the `/ctx` picker and `/history`,
  Shift+Tab mode cycling (plan / accept-edits behaviour), `/skills`, Ctrl-C interruption,
  slash commands, the `/model` picker, `!cmd`, `/save`, and config/history persistence.

## Layout

```
src/common.h   shared declarations
src/util.c     string buffer, file helpers, config
src/http.c     minimal HTTP/1.1 client (chunked streaming, Ctrl-C interrupt)
src/term.c     raw mode, key decoding, full-screen mode + status bar, line editor, confirmation menu,
               list picker, markdown printer
src/tools.c    the tools + permission-mode checks + shell runner
src/ollama.c   /api/chat streaming, tool-call accumulation, /api/tags
src/skills.c   SKILL.md discovery/parsing, /NAME expansion, scaffolding
src/main.c     REPL, slash commands, system prompt, agent loop
tests/         unit tests, fake Ollama server, pty integration tests
```

## Notes

- Ollama's default context is small; corbienest sends `num_ctx=32768` by default.
  Lower it with `/ctx` or `-c` if your machine runs out of memory, raise it for big tasks.
- Models without tool support still work as a plain chat (`/models` shows which is which).
- Only `http://` hosts are supported (Ollama's local API is plain HTTP).
