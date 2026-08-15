# AGENTS.md — working on Corbie Nest (corbienest)

corbienest is a Claude-Code-style terminal coding agent for local Ollama models, written in
plain C11 with no dependencies beyond libc, POSIX and cJSON. Keep it that way: no libcurl,
no ncurses, no readline. HTTP is raw sockets (`src/http.c`), the UI is ANSI escapes
(`src/term.c`).

## Build, run, test

```sh
make                 # builds ./corbienest  (needs cc, make, libcjson-dev)
make release         # clean, optimized, stripped build (RELEASE_CFLAGS)
make test            # C unit tests (tests/test_unit.c) + pty integration tests (tests/test_integration.py)
./corbienest           # needs an Ollama at $OLLAMA_HOST or 127.0.0.1:11434
```

`make test` needs no real model: `tests/fake_ollama.py` is a scripted stand-in
(keywords in the user message such as `TOOL_BASH`, `TOOL_WRITE`, `SLOW`, `ERROR` select a
canned response). Run the tests after every change to `src/`; both suites must pass and the
build must be warning-free with `-Wall -Wextra`.

## Layout

| file | owns |
|---|---|
| `src/common.h` | every shared declaration; one header, sectioned per module |
| `src/util.c` | `sbuf` string buffer, file helpers, config load/save, permission-mode names |
| `src/http.c` | minimal HTTP/1.1 client, chunked streaming, interrupt-while-waiting |
| `src/term.c` | raw mode, key decoding (incl. Shift+Tab = `CSI Z`), full-screen mode (alternate screen, scroll region, bottom status bar: mode · model · queued messages · session tokens · ctx · busy spinner via `term_busy()`/`term_busy_tick()`), line editor, prompt history (latest 100, `hist_*`), the message queue (`term_queue_*`: Enter while busy turns type-ahead into a queued message; pending type-ahead is echoed live in the bar and Shift+Tab while busy cycles the mode at once), `term_confirm` menu, `term_select` picker, streaming markdown printer |
| `src/tools.c` | tool definitions + implementations (incl. `task`, which calls back into `main.c`), `confirm()` (mode-aware; 4th menu option saves a rule to `.corbienest/permissions`, `tools_permissions_*`), checkpoints for `/rewind` (`tools_checkpoint_*`: pre-write file states per request), shell runner |
| `src/ollama.c` | `/api/chat` streaming, tool-call accumulation, text tool-call recovery |
| `src/skills.c` | SKILL.md discovery, frontmatter parsing, `/NAME` expansion, scaffolding |
| `src/main.c` | REPL (`process_input`), slash commands, system prompt, sub-agents (`run_subagent`, hooked into `tools.c`'s `task` tool via `tools_subagent`), agent loop (`run_turn`, which injects queued messages between tool rounds via `inject_queued()` and auto-compacts at ≥`AUTO_COMPACT_PCT` context via `maybe_auto_compact()`), `/ctx` picker (model max from `/api/show`), sessions (`session_save()` after each request to `config_dir()/sessions/<id>.json`, `--continue`/`--resume`/`/resume`), project memory (`.corbienest/memory.md`: `load_memory()` into the system prompt, `memory_update()` = quiet model call after each request via `ollama_quiet`, `/memory`), banner |

## Conventions

- C11 (`-std=gnu11`), 4-space indent, dense but readable; small static helpers per file.
  Prefer `sbuf` over manual `malloc`/`snprintf` juggling; use `xmalloc`/`xstrdup` (they abort on OOM).
- Every terminal-drawing routine must be **width-aware**: call `term_width()` at draw time (the
  user resizes), never hard-code 72/80, and truncate on UTF-8 boundaries with an `…`.
  The banner, previews, tool headers and menus all follow this — keep new output consistent.
- Interactive prompts go through `term.c` (`term_readline`, `term_ask_line`, `term_confirm`,
  `term_select`). Never `getchar()`/`fgets()` on stdin. Anything that asks the user a question
  must set the pending type-ahead aside first (see `ta_take`/`ta_restore`) so keys typed while
  the model was generating are never taken as an answer.
- Anything that waits (HTTP streaming, shell commands) must poll `term_poll_interrupt()`: it is
  what keeps type-ahead alive and turns Enter into a queued message. Queued messages are only
  consumed by `main.c` — between tool rounds (`inject_queued()`, plain messages only) and after
  the turn (`process_input` loop); after an interrupt they go back to the editor
  (`term_queue_to_editor()`), never silently dropped.
- Tool confirmations obey `g_cfg.mode` (`MODE_MANUAL`, `MODE_ACCEPT_EDITS`, `MODE_PLAN`,
  `MODE_AUTO`); route new mutating tools through `confirm(what, CONF_EDIT|CONF_BASH, ...)` in
  `tools.c`. Plan mode must stay read-only: `tools_for_mode()` in `main.c` drops
  `write_file`/`edit_file` from what the model sees.
- Slash commands: add to `SLASH_CMDS`, `handle_slash()`, `cmd_help()` and the README table.
  Unknown `/name` falls through to skills, so keep built-in names distinct from likely skill names.
- The fake Ollama answers memory-extraction calls with `NO_CHANGE` unless a user message contains
  `REMEMBER_ME`; the test config sets `memory=0` so only the dedicated memory test pays for the extra call.
- Anything persisted goes into `~/.config/corbienest/config` via `config_save()`; keep old keys
  readable (e.g. `yolo=1` still maps to `mode=auto`).
- Tests: extend `tests/test_unit.c` for pure logic and `tests/test_integration.py` (pty
  `Session`) for anything the user sees. Integration checks read `clean()`ed output — the
  editor redraws in place, so assert on stable phrases, not layout.

## Skills policy

Skills live in `.corbienest/skills/NAME/SKILL.md` (frontmatter `name`/`description`, body with
`$ARGUMENTS`). When you create a skill or a helper program for one:

1. Write helpers in **C** (compile with `cc` / a Makefile in the skill directory).
2. Use **Rust** only if the helper genuinely needs third-party libraries.
3. **Python** is a last resort only.

The same rule is given to the model in corbienest's system prompt (`skills_prompt_section()`).

## Don'ts

- Don't add dependencies or a build system beyond the Makefile.
- Don't print raw model/tool output without going through the existing preview helpers.
- Don't block on stdin outside `term.c`.
- Don't reflow or reformat unrelated code in a change; diffs should stay reviewable.
