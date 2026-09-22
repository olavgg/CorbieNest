# Corbie Nest

A Claude-Code-style terminal coding agent written in plain C that talks to
**local Ollama models**. It streams responses, lets the model read/write/edit
files, grep the codebase, and run shell and git commands — each mutating action
is shown to you and confirmed before it runs.

![A corbienest session: the agent runs pytest, reads and edits src/parser.py, re-runs the tests and reports back, with the input field and status bar pinned to the bottom of the screen.](docs/demo.svg)

corbienest takes over the terminal (alternate screen) while it runs, like Claude Code: the
conversation scrolls in the upper part, and the bottom of the screen is the app's: an input
field — a rule, the `❯` prompt, a rule — so it is always visible that input is accepted,
even while the model works, and under it a status bar showing the permission mode, the
model, the tokens used this session (↑ prompt · ↓ generated, ticking while the model
streams) and context usage. The field grows with what you type (up to ten rows, then it
scrolls) and the conversation region shrinks to match; what you send moves up into the
transcript as a `›` line. Whenever the app is waiting on something — the
model loading or generating, a tool's shell command, an Ollama request — an animated
spinner with what it is waiting for (`⠋ generating`, `⠹ running command`, …) appears at the
right end of the bar, and long waits also get an inline spinner with the elapsed time. On
exit your shell is back exactly as it was (use `/save` to keep a transcript).

## Build

Dependencies: a C11 compiler, `make`, [cJSON](https://github.com/DaveGamble/cJSON) and
[libcurl](https://curl.se/libcurl/) (7.66 or newer) — that's all. No ncurses: the UI uses ANSI
escapes. libcurl is what speaks HTTPS, for an Ollama behind TLS and for the hosted APIs the
[advisor](#the-advisor) can consult. (`python3` is only needed to run the test suite, and
`openssl` for its HTTPS tests.)

```sh
# Debian 13 (trixie) / Ubuntu 26.04 (resolute)
sudo apt install build-essential libcjson-dev libcurl4-openssl-dev

# Fedora 44
sudo dnf install gcc make cjson-devel libcurl-devel

# RHEL 10 / AlmaLinux 10 / Rocky Linux 10 — cjson comes from EPEL
sudo dnf install epel-release          # on RHEL: dnf install https://dl.fedoraproject.org/pub/epel/epel-release-latest-10.noarch.rpm
sudo dnf install gcc make cjson-devel libcurl-devel

# macOS
brew install cjson                     # libcurl comes with macOS
```

Then:

```sh
make
./corbienest               # run in the current directory
make release               # optimized, stripped binary (no debug info) — clean rebuild
sudo make install          # optional: installs to /usr/local/bin
```

On macOS Homebrew installs outside the default search paths, so point the build at it:
`make CFLAGS="-O2 -std=gnu11 -I$(brew --prefix)/include" LDLIBS="-L$(brew --prefix)/lib -lcjson -lcurl"`.

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
      --output-format text|json   with -p: plain reply (default) or one JSON object
                       {result, session_id, model, prompt_tokens, eval_tokens, model_calls, tool_calls, advisor_calls, duration_s}
  -y, --yolo           auto-approve tool calls (same as --mode auto)
      --mode NAME      permission mode: manual, accept-edits, plan, auto
  -T, --no-tools       disable tool calling
      --continue       resume the latest session started in this directory
  -r, --resume [ID]    resume a session: by ID, or pick one from a menu
      --keep-alive DUR how long Ollama keeps the model loaded between requests (default 30m; -1 forever, 0 unload, default = server's)
      --no-memory      don't update .corbienest/memory.md after requests
      --no-web         don't offer web_search/web_fetch (the model cannot look documentation up)
      --think / --no-think / --show-thinking
      --effort LEVEL   how hard the model thinks: one of the levels it has (off, on, low, medium, high, … — see /effort); default = the model's own
      --advisor MODEL  a stronger model the agent may consult through the advisor tool (see /advisor); off = none;
                       xai:MODEL, openai:MODEL, anthropic:MODEL for a hosted one (key from XAI_API_KEY, OPENAI_API_KEY, ANTHROPIC_API_KEY)
      --advisor-guidance LEVEL   how much the agent leans on it: light, normal (default), strong, max (see /advisor guidance)
      --draft N        draft_num_predict: speculative-decoding / MTP draft tokens per step (0 = off; default: the model's own,
                       e.g. models that ship an MTP head set 4); changing it makes Ollama reload the model
      --benchmark [N]  measure tokens per second at every context size the model supports (or just the -c size):
                       per size a warm-up (model load time, GPU placement) then N timed runs (default 3) of a fixed
                       prompt (or -p PROMPT) capped at 256 tokens; prompt-eval and generation tok/s per size;
                       --output-format json for one JSON object
  -h, --help / -v, --version
```

```
$ corbienest --model qwen3.8 --benchmark 1
benchmark  qwen3.8 · 1 run × 256 tokens per context size · draft 4 (model default, MTP/speculative) · http://127.0.0.1:11434
  ctx    load     placement                 prompt eval    generation              first token
  4k     6.1s     100% GPU (17.3 GB)        441 tok/s     60.2 tok/s               0.36s
  8k     5.9s     100% GPU (17.3 GB)        441 tok/s     56.7 tok/s               0.37s
  16k    5.9s     100% GPU (17.3 GB)        439 tok/s     45.7 tok/s               0.36s
  32k    5.9s     100% GPU (17.4 GB)        437 tok/s     58.2 tok/s               0.37s
  64k    7.7s     89% GPU (16.5/18.6 GB)    342 tok/s     33.7 tok/s               0.40s
  128k   8.8s     66% GPU (13.9/20.9 GB)    205 tok/s     15.4 tok/s               0.50s
  256k   19.1s    41% GPU (11.0/27.1 GB)    137 tok/s      9.5 tok/s               0.63s
```
(The row where the placement drops below 100% is where the KV cache stopped fitting in VRAM — the
largest context you can run at full speed is the row before it. With several runs per size the
generation column also shows the min–max spread; a size that fails to load is reported and the
larger ones skipped.)

### Slash commands

| command | what it does |
|---|---|
| `/model` | interactive model picker (↑/↓, type to filter, Enter) — `/model NAME` switches directly |
| `/models` | list installed models with tool/thinking capabilities |
| `/clear` | new conversation |
| `/compact` | have the model summarise the conversation to free context |
| `/memory [on\|off\|clear\|update\|every N\|idle N]` | show the project memory (`.corbienest/memory.md`), toggle its automatic update, run the pending update now, set how often it runs (default every 5 requests, plus after 15s idle at the prompt and at exit/`/clear`/`/compact`), how long the prompt must sit idle before a pending update runs (`idle off` waits for exit), or delete it |
| `/status` | model, context usage, settings |
| `/diff [git args]` | show `git diff` of the working tree (stat, patch, untracked files) for you only — nothing is added to the conversation; `/diff --staged`, `/diff HEAD~1` … pass through |
| `/rewind` | (or **Esc Esc** at an empty prompt) pick an earlier request and go back: undo the file changes the model made since (files are checkpointed before every `write_file`/`edit_file`), truncate the conversation to just before it (the request text returns to the editor), or both |
| `/cost` | tokens, model calls, tool calls, model time and wall time of this session |
| `/system [text\|clear]` | extra system instructions |
| `/think on\|off\|auto`, `/think show\|hide` | *when* a thinking-capable model thinks: `auto` (default) lets it think about each request once and turns thinking off for the tool rounds that follow, `on` thinks on every call, `off` never. *How hard* is `/effort` (`/think low\|medium\|high\|max` still works, as an alias for it) |
| `/effort [LEVEL\|default]` | how hard **this model** thinks, in the levels it has — see [Effort](#effort). No argument opens a picker of them; `default` leaves it to the model. Kept per model, shown next to the model's name in the status bar |
| `/advisor [MODEL\|off]`, `/advisor guidance [LEVEL]`, `/advisor effort [LEVEL]`, `/advisor ctx N\|auto` | a stronger model the agent may consult when the work is hard — see [The advisor](#the-advisor). No argument opens a picker of the installed models (a cloud or hosted model is set by name: `/advisor gpt-oss:120b-cloud`, `/advisor anthropic:claude-opus-5`). `guidance` says how much the agent leans on it (`light`, `normal`, `strong`, `max`), `effort` how hard it thinks |
| `/permissions [add …\|remove N\|clear]` | the project's saved "always allow" rules (`.corbienest/permissions`) |
| `/mode [name]` | permission mode: `manual`, `accept-edits`, `plan`, `auto` (Shift+Tab cycles) |
| `/yolo [on\|off]` | shortcut for `/mode auto` / `/mode manual` (careful) |
| `/init` | have the model explore the project and write a `CORBIENEST.md` (build/test commands, architecture, conventions); improves an existing one |
| `/skills [reload\|new NAME]` | list skills; run one with `/NAME [args]` |
| `/tools on\|off` | enable/disable tools |
| `/web [on\|off\|engine URL]` | whether the model may look documentation up with `web_search`/`web_fetch` (on by default; saved), and which search engine it uses (`%s` = the query; `/web engine default` restores DuckDuckGo) |
| `/max_iters [N]` | how many tool rounds one request may run before the loop guard stops it (default 100). Takes effect at once, so it can be raised from under a turn that is about to hit it |
| `/ctx [N\|Nk\|max\|default]` | context window: no argument opens a size picker (up to the model's trained maximum), `/ctx 64k`, `/ctx max` … set it directly |
| `/temp X`, `/host URL` | tuning |
| `/keepalive [DUR]` | how long Ollama keeps the model loaded after a request (default `30m`, so it is not reloaded from disk mid-session; `-1` = forever, `0` = unload right away, `default` = the server's 5 minutes) |
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
- PgUp at the prompt scrolls back through the conversation (the alternate screen has no scrollback of
  its own, so corbienest keeps one): PgUp/PgDn/↑/↓ move, Home/End jump, Esc/Enter/PgDn at the bottom
  return to the prompt exactly as it was.
  Esc twice at an empty prompt opens `/rewind`.
- **You can keep typing while the model works.** What you type shows up in the input field as
  you go (`❯ …▏`); press Enter to queue it as a message — add details,
  narrow the request, change the spec — the status bar shows `N queued`.
  **A queued message does not wait for the job in flight to end**, because it is usually about
  that job: the running shell command is stopped, the tool calls of that round that had not
  started are skipped (the model is told they did not run), a sub-agent reports what it has
  found so far instead of finishing its research, and the message goes to the model on its very
  next call. A message queued *before* the turn started — the ones sent one after another after
  a turn ends — stops nothing; it is delivered between tool rounds as before. Queue as many as
  you like; they are sent in order, and a `/command` waiting for the end of the turn does not
  hold back the messages behind it. Ctrl-C hands queued text back to the editor instead of
  sending it. Text without Enter simply reappears in the prompt afterwards.
- Tab completes slash commands and skill names; ↑/↓ browse history — the latest 100 queries are
  kept in `~/.config/corbienest/history` (`/history` lists them). Ctrl-R searches it
  incrementally (`(reverse-i-search)`, like bash): type to refine, Ctrl-R again for an older
  match, Enter keeps the match in the editor, Esc restores what you had.
- Shift+Tab cycles the permission mode, also while the model is working (it applies to the
  remaining tool confirmations of that turn); the current mode is shown in the bottom status bar.
- **Slash commands that only report or set something answer straight away while the model
  works** — they never become a queued message: `/help`, `/status`, `/cost`, `/diff`, `/history`,
  `/pwd`, `/skills`, `/memory`, `/mode`, `/yolo`, `/permissions`, `/tools`, `/max_iters`,
  `/think`, `/effort` (it prints the levels instead of opening the picker), bare `/advisor`, `/temp`, `/keepalive`. A `/permissions add` or `/mode` typed mid-turn applies to the
  tool confirmations still to come, like Shift+Tab, and `/max_iters` to the rounds still to come. Everything that touches the conversation (`/clear`, `/compact`,
  `/rewind`, `/resume`, `/save`, `/system`, `/init`, skills), needs the server (`/model`,
  `/models`, `/ctx`, `/host`, `/memory update`) or asks a question stays queued until the turn ends.

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
| `web_search(query, max_results?)` | yes — shows the query and the engine; "always allow" saves the engine's **host** |
| `web_fetch(url, offset?, timeout?)` | yes — shows the URL; "always allow" saves the **host** |
| `task(description, prompt)` | none for the call itself — runs a **sub-agent**: a fresh, read-only agent loop (read_file, list_dir, grep, bash, web_search, web_fetch — with the usual confirmations) that investigates and returns a report as the tool result, keeping the noise out of the main context. Its tool calls are echoed as `⎿ grep(…)` lines and the report is previewed. Sub-agents cannot edit files or start further sub-agents |
| `advisor(question?)` | none — only on offer after `/advisor MODEL`. Puts the agent's question, and the conversation so far, to a **stronger model** (local, Ollama cloud, or a hosted API) and returns its advice as the tool result; see [The advisor](#the-advisor) |

Each confirmation is a small menu:

![The confirmation menu: a yellow question, four numbered options with the selected one marked by an orange arrow, and a line of key hints underneath.](docs/confirm.svg)

Move with ↑/↓ (or j/k) and press Enter, or hit `1`–`4` or the `y`/`a`/`p`/`n` shortcuts;
Esc or Ctrl-C means no. `p` saves a rule to `.corbienest/permissions` (like Claude Code's
project allow-list): `edit` allows file writes/edits, `bash git status` allows shell commands
that start with those words (`git status --short` yes, `git status; rm -rf /` no — commands
with `; | & $ \` < >` never match a rule), `fetch www.postgresql.org` allows `web_fetch` to read
pages from that host (exact host, no wildcards — approve a manual once and read the rest of it
without being asked). `/permissions` lists the rules; `/permissions add
bash make`, `/permissions add fetch keycloak.org`, `/permissions remove N`, `/permissions clear`
manage them by hand. On "no" you can type what the model should do instead (Enter to skip)
and it is sent back as the tool result. Anything you typed while the model was still generating
is never taken as an answer — it is kept for your next prompt. Ctrl-C while a command runs kills it.

Some local models occasionally emit their native tool syntax as plain text
(e.g. `<function=grep>…`) when Ollama's parser fails; corbienest recognises the
Qwen XML and Hermes `<tool_call>{json}</tool_call>` shapes and still executes them.

### Reading documentation (`web_search`, `web_fetch`)

Local models are confidently wrong about other projects' APIs: an admin endpoint that does not
exist, a `postgresql.conf` option from three major versions ago. These two tools give the model
the page instead of its memory of the page.

- **`web_search(query)`** — the top results as title, URL and snippet, for when it does not know
  where the documentation is. Default engine is DuckDuckGo's HTML endpoint; `/web engine URL`
  points it anywhere (`%s` is the query), so a self-hosted SearXNG works as well.
- **`web_fetch(url)`** — the page as readable text: scripts, styling and markup stripped,
  `<pre>` blocks kept as fenced code, links kept with their target so it can follow the docs to
  the next page. At most 24 KB per call, the rest paged with `offset`, so a long manual page
  cannot swallow the context window.

**Every session is told to use them.** One line of the system prompt corbienest builds for every
request: never guess at another project's API, options or errors — search for its docs, read
them, match the version this project actually uses (lockfile, manifest, image tag), say which
page you used, and skip the web when the repository answers the question. It is deliberately
one line: the two tools' own descriptions carry the rest, and a system prompt is re-sent with
every single call. Naming the doc sites you care about in `CORBIENEST.md` makes it concrete —
see [Project instructions](#project-instructions).

Pages come in through the `curl` (or `wget`) program, which already brings redirects, size caps
and a fallback for machines without it. Every
search and fetch is confirmed like a shell command — a search shows the query, which is what
leaves your machine — `http(s)` only, and cloud-metadata addresses (`169.254.169.254`,
`metadata.google.internal`) are refused outright. Approving a host once (`p`) covers the rest of
that manual, and the engine's host covers later searches. `/web off` (or `--no-web`) takes both
tools away.

A big page is often best handed to `task` — the sub-agent has both tools too, reads the manual
in its own context and reports back the three endpoints that matter.

### The end-of-task report

When the model is done with what you asked for it closes the turn with a short report: what it
did — the changes, the files they touched, and how it verified them (build, tests, running the
program, or that it did not) — and, when there is something worth raising, up to three
suggestions of what you may want to do next: follow-up work it left out on purpose, a weakness
it noticed nearby, something worth testing. Suggestions are only suggestions; it does not start
on them.

The report is meant to say *finished*, so it is asked for only when the work really is. When
something is left — a step skipped, a tool you denied, a build or test that failed, work stopped
partway — the model writes no report and says instead, in a line or two, what is done, what is
not, and what comes next. There is no report in plan mode either (the plan is the answer), nor
after a turn that only answered a question or stopped to ask you something. Small local models
follow all of this more loosely than large ones.

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
read too), personal ones in `~/.config/corbienest/skills/`. Claude Code's flat custom
commands are picked up as well: `.corbienest/commands/NAME.md`, `.claude/commands/NAME.md`,
`~/.claude/commands/NAME.md` (frontmatter optional — the file body is the prompt). Run one with `/NAME args` —
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
way out when a long session fills up. Once a request has used 85% or more of the window,
corbienest compacts automatically before the next model call — also mid-task, between tool
rounds, and then the summary is handed straight back to the model, along with your request as
you made it, so it carries on with the job instead of stopping to ask what to do next.

### Sessions

Every conversation is saved after each request to `~/.config/corbienest/sessions/<id>.json`
(the latest 100 are kept). `corbienest --continue` picks up the most recent session started in
the current directory, `--resume` opens a picker (or `--resume ID`), and `/resume` does the same
from inside a session — the recap shows the first request and the last reply. `/clear` starts a
new session; `/status` shows the current id, which is also printed when you quit.

### Project memory

Like Claude Code's auto-memory: corbienest keeps `.corbienest/memory.md` in the working
directory and loads it into the system prompt of every request. The model curates it — every
few requests (default 5, `/memory every N`; after 15s idle at the prompt, `/memory idle N`;
also at exit, `/clear`, `/compact`, `/cd`) a quiet
extraction call asks whether the exchanges since the last update revealed anything durable
(who you are and how you like to work, feedback you gave, project goals/decisions/constraints
that aren't in the code, references such as URLs or tickets) and, if so, rewrites the file;
otherwise nothing is written. Facts the repository already records, and anything only relevant
to the current conversation, are deliberately not saved. Type `# fact` to add something yourself
in one keystroke, ask the model to remember
or forget something (it edits the file with `edit_file`), or edit the file by hand. `/memory`
prints it, `/memory off` (saved to config) or `--no-memory` disables the update, `/memory clear`
deletes it. One-shot runs (`-p`) only touch the file if `.corbienest/` already exists.

The extraction call is a real model call (the status bar shows `⠋ updating memory` while it
runs) and its cost is printed afterwards (`✎ memory: no change · 3.2s`). It never asks the
model to think and is capped in length (a cut-off reply is ignored rather than written). Its
different prompt evicts Ollama's prompt cache for the conversation, so the request after it
re-evaluates the whole context — which is why it is batched rather than run after every
request (`/memory every 1` restores that; `/memory update` runs a pending one now; `/memory`
shows how many requests are pending). On slow machines `/memory off` removes it entirely.

So that a pending update never holds up the way out, it also runs on its own once the prompt
has been idle for 15 seconds (`/memory idle N`, `/memory idle off`, saved to config): the call
happens while you read the reply, and by the time you quit there is usually nothing left to
write. Typing anything cancels it for that prompt — a request you start right away still finds
a warm prompt cache, and the batch simply waits for the next pause. The update is a model call
like any other, so Enter while it runs queues your message (`1 queued` in the status bar)
rather than sending it: it is sent the moment the write is done, without a second Enter. If you
do quit with an update still pending, the flush on the way out says so and Ctrl-C skips it.

### Project instructions

If a `CORBIENEST.md`, `CLAUDE.md` or `AGENTS.md` exists in the working directory it is
appended to the system prompt, so you can give the model project-specific guidance (up to
32 KB). `/init` writes a first one for you.

This is the place to say which upstream projects this code talks to and where their
documentation lives, so `web_fetch` lands on the right page and the right *version* of it:

```markdown
# Project instructions

We run Keycloak 26 behind PostgreSQL 17.

- Keycloak admin REST API: https://www.keycloak.org/docs-api/26.0/rest-api/ —
  the admin client lives in src/auth/. Check the docs before adding an endpoint.
- PostgreSQL settings and SQL: https://www.postgresql.org/docs/17/ — we are on 17,
  do not use syntax from 18, and do not trust your memory of default values.
- Look it up when you are unsure (web_search to find the page, web_fetch to read it)
  and say which page you used. Searching for "keycloak 26 <thing>" beats guessing.
```

Three other ways to add instructions: `-s "…"` / `/system …` for the session, a skill for
instructions the model should pull in on demand (see [Skills](#skills)), and
`.corbienest/memory.md`, which the model curates itself (see [Project memory](#project-memory)).

### Effort

Thinking models do not all think in the same way, so how hard one thinks is set **per model**,
in the levels that model has:

| model | `/effort` offers |
|---|---|
| gpt-oss | `low` · `medium` · `high` — it cannot stop thinking, so there is no `off` (`/think off` sends it `low`) |
| qwen3.8 | `off` · `low` · `medium` · `high` |
| most other thinking models (qwen3, deepseek-r1, …) | `off` · `on` |
| a model without the thinking capability | nothing — and it is never sent anything but `think: false`: the server refuses the rest |

Where the list comes from: Ollama 0.34.3 and later say which levels a model has
(`thinking.values` in `/api/show` — cloud models have their own, e.g. `low` · `high` · `max`), and
corbienest offers exactly those. Older servers only say *that* a model thinks; for them the
table above is built in, and any other thinking model is on/off. You can still give such a
model a level by name (`/effort high`) — it is passed on, and corbienest tells you the server
will take it as plain "on". `high`, `xhigh` and `max` all mean "as hard as it goes": one of
them that the model does *not* have is sent as the strongest level it does have, so a saved
`high` keeps working when a server upgrade renames the top level (qwen3.8's becomes `xhigh` in
0.34.3, where `high` would quietly mean medium).

`/effort` and `/think` work together: `/think` says **when** the model thinks, `/effort` **how
hard**. One thing to know: the models that have levels write the level into the *top* of the
prompt, so a level is sent with **every** call of a request — switching it off for the tool
rounds, as `/think auto` does for on/off models, would change the top of the prompt and make
the server read the whole conversation again, twice per request. If that thinking on every
tool round is too slow, pick a lower level rather than `/think auto`.

Capabilities are read from `/api/show`, not from the model list: `/api/tags` reports them as
they were when the model was pulled, so a model whose template has learned tools or thinking
since then (deepseek-r1, for one) shows up there as chat-only.

### The advisor

`/advisor MODEL` names a stronger model that the agent may **consult** while it works — a bigger
local model, one of Ollama's cloud models (`/advisor gpt-oss:120b-cloud`; run `ollama signin`
once, the local server relays the call), or a hosted API: xAI's Grok, OpenAI, or Anthropic's
Claude (see [Hosted advisors](#hosted-advisors)). The agent gets an `advisor` tool and is told
when to use it: before it commits to an approach for a non-trivial change, when an error has
survived two fixes or a result makes no sense, and before it calls a difficult task done. You
can also just say so: *"ask the advisor before you continue"*.

```
● advisor(parse() crashes on the input "1, 2,,3" - what is the smallest corr…)
  ⤷ advisor gpt-oss:120b-cloud · consultation 1 of 3 in this request · 2 KB of the conversation · esc skips it
    ⎿ advice · 930 tokens · 14s:
  ⎿  The crash is int("") on the empty field between the two commas. …
```

- **What the advisor sees.** Not just the question — a small model writes a poor brief — but the
  conversation itself, as one quoted text: your request, what the agent said, its tool calls and
  their results (long ones lose their middle; when it does not all fit, the newest part wins and
  the request always stays), plus the project's rules. It has **no tools**: it cannot read a
  file or run anything, it answers with advice, and the agent is told that where a file or a
  command contradicts the advice, they are right.
- **What it costs.** Time — and on one machine more than that: loading a second local model may
  push the main one out of memory, and its next reply then reloads it and reads the whole
  conversation again (corbienest says so when it happens). So a consultation runs in a window
  of its own (`/advisor ctx`, default the main window but at most 16k: a 70B model pays several
  times the memory per token that a small one does), the advisor is unloaded as soon as it has
  answered, and one request may consult it a limited number of times (**3** at the default
  guidance) — after that the tool answers with an error instead. A cloud or hosted advisor
  touches neither your memory nor the main model's prompt cache, which makes it the cheap choice
  on a small machine; it does mean the conversation is sent off the machine with each
  consultation.
- `/advisor effort LEVEL` sets how hard *it* thinks (the same levels as `/effort`, kept with its
  model; `/think` does not apply to it). Lower it if consultations run out of tokens while
  thinking. `/advisor MODEL` with the model already doing the work is allowed — a second opinion
  from a fresh context, not a stronger one.
- Enter with a message while it is consulted stops the consultation (your message goes first);
  Esc skips it. Either way the turn carries on. Plan mode keeps the advisor — it changes
  nothing; sub-agents do not get it. Its tokens are part of the session totals, and `/cost`
  shows them on a line of their own.

#### Guidance: how much the agent leans on it

A small model is a poor judge of when it needs help: it is sure of the wrong approach, and calls
a task done that is not. `/advisor guidance` (or `--advisor-guidance`) sets how much it leans on
the advisor — more tokens spent on advice, fewer spent on the agent going the wrong way:

| level | consultations per request | the agent is told to ask | advice | corbienest also asks |
|---|---|---|---|---|
| `light` | 1 | only when it is stuck | ≤ ~250 words | — |
| `normal` (default) | 3 | when the work is hard (above) | ≤ ~400 words | — |
| `strong` | 6 | after reading the code and before committing to an approach, before each non-trivial change, when a build or test fails in a way it does not understand | ≤ ~700 words, concrete: file, function, the lines to change | a **review** before a request that changed files ends |
| `max` | 10 | as `strong` | ≤ ~900 words, as concrete | the review, and a **check** of the request's first change before it is made |

The review and the check do not count against the agent's consultations, and each happens at
most once per request. They enter the conversation the way a consultation does — as an
`advisor` call and its result — so the agent reads them as advice, not as something you said:

```
● write_file(src/parse.py)
  ⤷ advisor anthropic:claude-opus-5 · checks the first change before it is made · 6 KB of the conversation · esc skips it
    ⎿ advice · 2.4k tokens · 9s:
  ⎿  Not yet: the fix belongs in split_fields(), not in parse() — …
  ⎿  held back — the advisor sees a problem with it
```

When the advisor finds nothing to change it answers `LGTM`, and the change is made or the
request ends there (`⎿ nothing to change`). Otherwise the agent gets its answer and one more
round to act on it; where the files or a command's output show the advisor is wrong, the agent
is told to say so. Higher levels also show the advisor more of the conversation (up to 96 KB
instead of 48). Changing the level changes the agent's instructions, so its next reply reads the
conversation again.

#### Hosted advisors

`/advisor PROVIDER:MODEL` consults a hosted API directly — no Ollama in between:

| provider | name it | key | base URL (override) | API |
|---|---|---|---|---|
| xAI (Grok) | `xai:grok-4.7` (or `grok:…`) | `XAI_API_KEY` | `https://api.x.ai/v1` (`XAI_BASE_URL`) | Chat Completions |
| OpenAI | `openai:gpt-5.2` | `OPENAI_API_KEY` | `https://api.openai.com/v1` (`OPENAI_BASE_URL`) | Chat Completions |
| Anthropic (Claude) | `anthropic:claude-opus-5` (or `claude:…`) | `ANTHROPIC_API_KEY` | `https://api.anthropic.com` (`ANTHROPIC_BASE_URL`) | Messages |

- **Keys come from the environment only** — export one before starting corbienest. They are
  never written to the config, a session or the history. An API key from the provider's
  console is what is needed; a chat subscription (grok.com, ChatGPT, claude.ai) is not one.
- `/advisor NAME` asks the provider's model endpoint first, so a wrong key or an unknown model
  is refused there and then. Consultations are billed to that key — `/cost` shows what they
  took.
- **Effort.** For Anthropic models the levels come from the API itself (`low` … `max`, or a
  thinking budget `on`/`off` for models that have no levels), and thinking is always adaptive.
  xAI and OpenAI do not say per model, so `/advisor effort` offers what their models take between
  them (OpenAI: `minimal` … `max`; xAI: `low` … `xhigh`; `off` goes out as `none`); a level the
  model lacks is refused by the server, which says so — `/advisor effort default` leaves it to the
  model.
- An Anthropic model with server-side refusal fallbacks (Opus 5, Fable 5.x) is asked with
  `fallbacks: "default"`: a consultation its safety classifiers decline — security tooling can
  look like that — is answered by the fallback model instead of not at all. A refusal that
  stands is reported as one, and the turn carries on.
- The base URLs can point at a proxy or a compatible server. `https_proxy`/`no_proxy` are
  honoured (never for this machine), and `CURL_CA_BUNDLE` or `SSL_CERT_FILE` names a private CA.

### Config

Settings changed with `/model`, `/ctx`, `/think`, `/effort`, `/advisor` (and its `guidance`, `effort`, `ctx`), `/mode`, `/yolo`, `/host`, `/keepalive`, `/web on|off|engine URL`, `/memory on|off|every N|idle N` are saved to
`~/.config/corbienest/config` (the efforts as `effort.<model>=<level>`, the advisor as `advisor=`, `advisor_ctx=` and `advisor_guidance=`). Environment: `OLLAMA_HOST`, `CORBIENEST_MODEL`, and for a hosted advisor `XAI_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY` (never saved) and their `…_BASE_URL`s.

### Running more than one session

Several corbienest sessions can work on separate tasks against the same Ollama at the same
time. On the Ollama side that needs `OLLAMA_NUM_PARALLEL` ≥ the number of sessions (otherwise
they simply queue behind one another), enough VRAM for one KV cache *per slot*, and the same
`-c` in every session — corbienest always sends `num_ctx`, and different values make the
scheduler load a separate runner for each rather than sharing one.

Give each session its own directory (a `git worktree` is the natural fit): two agents editing
one tree fight over the files themselves, and `/rewind` only knows about the writes its own
process made. Then nothing per-project collides — `.corbienest/memory.md` and
`.corbienest/permissions` are per directory, session files are named after the start time and
pid, and `--continue` resumes the latest session *from this directory*.

What is shared is `~/.config/corbienest`, and it is written to survive that:

- **History accumulates.** `history` is only ever appended to, so what one session typed is
  never overwritten by another's save (it is folded back to the latest 100 entries once the
  file grows past 64 KB). Each session still starts with the entries that existed when it did.
- **The config is never half-written.** It is swapped in with `rename(2)`, so a session reading
  it gets the whole old file or the whole new one — never a truncated one, and never a mixture.
  It is not merged, though: it is rewritten in full on every setting change, so whichever
  session saved last is what the *next* one starts with. Give the sessions separate settings
  with `XDG_CONFIG_HOME=~/.config/crow-a corbienest` if that matters.

The same atomic swap is used for `.corbienest/memory.md`, `.corbienest/permissions` and the
session files, so a crash or a kill mid-write cannot truncate any of them either.

## Tests

```sh
make test
```

- `tests/test_unit.c` — C unit tests: string buffer, file helpers, the HTTP client
  (chunked/content-length/abort/extra headers against a forked local server, and what a host
  setting becomes as a URL), the streaming
  markdown printer, text tool-call recovery, every tool (read/write/edit/list/grep/bash
  including timeouts and non-interactive denial), URL checks and the HTML-to-text and
  search-result extraction behind `web_fetch`/`web_search`, permission modes, skills
  (frontmatter parsing, `$ARGUMENTS`, scaffolding), what each kind of model can be set to and
  what is sent as `think` for every combination of `/think`, `/effort` and kind of call,
  where an advisor consultation runs and what it is shown at each guidance level, and what goes
  to and comes back from the hosted APIs (request bodies, answers, refusals, errors).
- `tests/test_integration.py` — runs the real binary against `tests/fake_ollama.py`,
  a tiny scripted Ollama stand-in (no model needed): one-shot mode, the full tool loop
  with results fed back, `--yolo`, XML tool-call recovery, `@file`, errors, and — through a
  pseudo-terminal — the editor (cursor keys, history, multi-line, paste, type-ahead),
  the confirmation menu (arrow keys, deny with reason, always, stray keys ignored),
  messages queued with Enter while the model streams or a tool runs (they stop the command,
  the round and the sub-agent in flight, step past a queued `/command`, and are handed back on
  Ctrl-C), the `/ctx` picker and `/history`,
  Shift+Tab mode cycling (plan / accept-edits behaviour), `/skills`, Ctrl-C interruption,
  slash commands, the `/model` picker, `!cmd`, `/save`, config/history persistence, `/effort`
  (per-model levels, the picker, the status bar), `/advisor` (the consultation, its limit,
  a missing or signed-out advisor, Esc and a queued message during one; xAI, OpenAI and
  Anthropic through the fake's stand-ins for them — keys, effort, refusals, a missing or wrong
  key, and that the key is written nowhere), `/advisor guidance` (the review before a request
  ends, the check of its first change) and HTTPS (an Ollama and a hosted API behind TLS, and a
  certificate nobody vouches for refused).

## Layout

```
src/common.h   shared declarations
src/util.c     string buffer, file helpers, config, URL checks + HTML-to-text + search results
src/http.c     the HTTP(S) client over libcurl (streaming, Esc interrupt, idle timeout)
src/term.c     raw mode, key decoding, full-screen mode + status bar, line editor, confirmation menu,
               list picker, markdown printer
src/tools.c    the tools + permission-mode checks + shell runner
src/ollama.c   /api/chat streaming, tool-call accumulation, /api/tags, /api/show (capabilities, effort levels)
src/provider.c the hosted APIs the advisor can consult: xAI, OpenAI (Chat Completions), Anthropic (Messages)
src/skills.c   SKILL.md discovery/parsing, /NAME expansion, scaffolding
src/main.c     REPL, slash commands, system prompt, agent loop, sub-agents, the advisor
tests/         unit tests, fake Ollama server, pty integration tests
```

## Notes

- Ollama's default context is small; corbienest sends `num_ctx=32768` by default.
  Lower it with `/ctx` or `-c` if your machine runs out of memory, raise it for big tasks.
- **Context hygiene.** Tool results (file contents, command output) are the bulk of a long
  conversation and go stale quickly. Once the context is half full, results from requests
  before the previous one are replaced in place by a short stub (tool name, size, first
  line — `⋯ elided N old tool results` is printed) so the model can call the tool again if it
  needs the details; at 85 % the conversation is auto-compacted (summarised by the model), and
  from 70 % the stats line suggests `/compact`.
- **Slow?** The stats line under each reply tells you where the time went: `prefill Ns` is
  prompt evaluation (large when the model was just (re)loaded or the prompt cache missed),
  `tok/s` is generation speed. Things that help, roughly in order: make sure the whole model
  fits in GPU memory (corbienest warns `⚠ model is only NN% in GPU memory` after the first
  reply when it does not — pick a smaller `/ctx` or model), turn off the per-request memory
  update (`/memory every 10` or `/memory off`), spend less on thinking (a lower `/effort`; `/think auto` — the default —
  thinks once per request instead of after every tool result; `/think off` never; the stats line shows
  `thought 41s (≈2.1k tok)` per call and `/cost` the session total), keep the model
  loaded (`/keepalive`, default 30m), and `/compact` long conversations. Tool output is capped
  (`read_file` 2000 lines / 64 KB, `bash`/`grep` 32 KB, `web_search`/`web_fetch` 24 KB) so a single tool round cannot fill the
  context; the model is told to page with `offset`/`limit`.
- Models without tool support still work as a plain chat (`/models` shows which is which).
- The Ollama host may be `http://` or `https://` (an Ollama behind a TLS proxy); a host given
  without a port is Ollama's 11434 for `http`, 443 for `https`. The certificate is checked
  against the system's CAs, or the file `CURL_CA_BUNDLE`/`SSL_CERT_FILE` names.

## Safety

Corbie Nest writes files and runs shell commands in the directory you start it in, and with
`web_search`/`web_fetch` it makes outbound HTTP requests to hosts the model picked — a search
sends your query text to the engine. In the
default `manual` mode every mutating action, command and page fetch is shown and confirmed
first, and `plan` mode is read-only (a fetch is still allowed there: reading the docs is how a
plan gets the API right) — but `--yolo` / `/mode auto` approves everything, and rules saved to
`.corbienest/permissions` stay approved for that project. Run it on code you can restore
(a git working tree), and remember that a local model is still a model: it can be talked into
things by the content of the files it reads. An advisor that is not on your machine (an Ollama
cloud model, or `xai:`/`openai:`/`anthropic:`) is sent the conversation — file contents and
command output the agent has seen included — with every consultation, and at guidance `strong`
or `max` corbienest consults it without the agent asking; pick a local advisor for code that
must not leave the machine.

## Contributing

Bug reports and patches are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for the build,
test and style rules, and [AGENTS.md](AGENTS.md) for how the code is laid out. The short
version: no new dependencies, `make test` must pass, and the build must be warning-free.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Copyright 2026 The Corbie Nest authors, Olav Gjerde.
