# AGENTS.md — working on Corbie Nest (corbienest)

corbienest is a Claude-Code-style terminal coding agent for local and hosted models (Ollama,
Anthropic, xAI, OpenAI, DeepSeek, Kimi), written in plain C11 with no dependencies beyond libc,
POSIX, cJSON and libcurl. No readline, nothing more — with one exception, optional: a notcurses
frontend, which needs libunistring, zlib and terminfo (ncurses). It is built when all three are
installed (`NOTCURSES=auto`, the default) and left out otherwise (see "The notcurses frontend" below).
The notcurses core it runs on is vendored in `vendor/notcurses/` (a copy of 3.0.17 with the tmux 3.4+
DA1 fix; see its README) and built by make, so no notcurses package is used. Without the three libraries
the build is the dependency-free one; with them the binary has the notcurses screen.
HTTP (and HTTPS, for an Anthropic model that does the work and for the hosted APIs an advisor can
consult) is libcurl behind `src/http.c`; the UI is ANSI escapes (`src/term.c`), or notcurses
(`src/term_nc.c`) when that build is asked for with `--ui notcurses` or `/ui notcurses`.

### The notcurses frontend

- Build: the Makefile probes for libunistring, zlib and terminfo (`NC_PROBE`); when all three are
  there `NOTCURSES=auto` (the default) builds the vendored core (`vendor/notcurses/src/lib`,
  `src/compat`, `src/media/shim.c` and `none.c`, objects next to their sources) and the frontend
  (`NC_OBJ`, `NC_VFLAGS`, generated `build/notcurses/version.h` and `builddef.h` from CMake's templates).
  `NOTCURSES=0` leaves them out, `NOTCURSES=1` fails without the libraries. Debian/Ubuntu:
  `libunistring-dev zlib1g-dev libncurses-dev`. The system notcurses is not used any more. `-DWITH_NOTCURSES`
  is defined when it is built; without it `term_nc.c` compiles to stubs and `--ui notcurses` says so.
  `make test` must pass in both builds (run it with and without the libraries).
- Choice: `--ui ansi|notcurses` for one run, `ui=` in the config for the default, `/ui X` to switch:
  it saves the choice and the session, then re-executes the binary with `--resume ID` (never
  for `-p`, `--benchmark` or a pipe).
- `term.c` is the seam: each exported `term_*` whose behaviour differs starts with
  `if (g_ui_nc) return nc_...;`. What the two UIs share stays shared: history, the message queue,
  paste tokens, the scrollback model (the conversation is still captured from stdout, `sb_feed`),
  and the chrome *text* (`term_status_text()` produces the same escapes the
  ANSI bar draws, which `term_nc.c` parses into cell styles).
- Threads: notcurses starts its own input thread; that is the one exception to "threads exist only
  in `http_job_*`". It is in the library, never calls into corbienest, and this build only.
- Processes: a memory update runs in a worker *process* (`corbie --memory-worker SNAPSHOT FD`, `memory_spawn()`:
  fork, then setsid, /dev/null on 0-2, the fds closed, exec of the binary itself), so it outlives the program: exit does not
  wait for it, and nothing reaps it but `memory_reap()` (waitpid WNOHANG on its own pid). It has no terminal and must stay
  free of term_* and stdout use; the worker's usage comes back through its pipe and is booked at the prompt.
- Keys during a request: notcurses reads stdin on that thread, so a busy wait must watch
  `term_interrupt_fd()` (notcurses' readiness pipe here, stdin otherwise), never stdin itself; with
  stdin watched, nothing reads the keys while the model works.
- Signals: `NCOPTION_NO_QUIT_SIGHANDLERS` (the program's own `atexit` restores the terminal); the
  SIGWINCH handler is notcurses' own and resize arrives as `NCKEY_RESIZE`.
- Output: notcurses writes to its own stream (`dup(1)`), never to the captured `stdout`; in this
  build `sb_write_fn` records output and does not write fd 1.
- Suggestions in the notcurses UI (`term_nc.c`, `sug_*`): the same `term_suggest` list as the ANSI
  UI, drawn in rows of its own under the field, above the agents strip and the bar (the ANSI order; `sug_height()` gives the rows, `nc_render()` takes them from the conversation while the list is open, and the `term_confirm`/`term_select` menus, `draw_menu()`, are boxes in the same place, sized by `menu_height()`/`menu_lines_room()`, two rows of conversation kept); refreshed by `sug_sync()` when the line or cursor
  changes (each key in the busy poll, each prompt loop pass); its keys are `sug_key()` (Tab, ↓/↑,
  Enter on a row, Esc closes), shared by both loops. Not yet: the `@file` list is the same code path
  but is not tested in this UI, and the `/model` picker (`cmd_model_picker`, through `term_select` → `nc_select`) lists the Ollama
  models and the Anthropic ones (`anthropic:ID`), as the ANSI picker does; xAI and OpenAI are advisors only and are not in it.
- Folds and mouse in the notcurses UI (long tool results, multi-line bash commands and the advisor's advice all fold there now, through the same `term_fold_begin()`): `notcurses_mice_enable(BUTTON_EVENT)` in `nc_start()`; `nc_getkey()`
  takes mouse reports out of the key stream (`nc_mouse()`): a left click on a fold's head calls
  `term_fold_toggle()` (the same toggle as the ANSI screen), the wheel scrolls the pane. A click finds its
  line through `g_pane_line`, the row map `draw_pane()` makes, valid only while `term_sb_epoch()` is the one
  it was made at (lines dropped or cleared since: no click acts until the next paint). The suggestion list's
  rows are not in the map. Selecting text with the mouse needs Shift in most terminals, as in the ANSI UI.
- Not in the notcurses UI yet (say so when touched): the viewer (PgUp scrolls the pane), the `←` agents menu,
  images. Paste tokens: a click on one opens the paste view (`draw_pasteview()`, `pv_run()`, the same keys as the
  ANSI viewer), Ctrl-O unfolds one (`tok_unfold()`), and a token is drawn as the caret is (`draw_field()`). The
  notcurses input layer (vendor/notcurses) hides one thing from the paste reader, so it differs from the ANSI
  reader: an arrow or editing key inside a paste gives its CSI text (`[D` for Left: a SS3 `ESC O D` gives `[D`, not
  `OD`). Two changes in the vendored input make the paste match the ANSI reader: a CRLF is one break (an Enter keeps
  its byte), and a byte that is not UTF-8 is U+FFFD, one per byte, and a full input queue makes the input thread wait
  instead of dropping a key (`load_ncinput()`; a paste is thousands of keys, see its README).
- Close buttons: every modal has a clickable `[×]` that is its Esc: the banner (top border), the `/usage` overlay (top border), the
  paste view (right end of the title row) and the menus (`term_confirm`, `term_select`, the task list: top border in notcurses, the right end
  of the first band row on the ANSI screen). `close_x_cols()` (common.h) is the one place that says which three cells; the drawer records them
  (`g_mx[]` in term.c, `g_mx_*` in term_nc.c), the click is checked only while its modal is up, and the ANSI key is `K_CLOSE_X`, which each modal
  loop turns into `K_ESC` (so a confirm answers "no", as Esc does); notcurses' `nc_getkey()` returns Esc for it, and the overlay is closed at
  once. A new modal gets one the same way. The inline (no band) ANSI menus have none: they are not boxes.
- Start-up in the notcurses UI: `nc_start()` runs `notcurses_init` in a child process first (`nc_probe()`, 2 s): a child that does not come back is killed, the terminal is restored and the ANSI UI runs. The vendored core has the DA1 fix for tmux 3.4+ (`\e[?1;2;4c`, upstream #2736, PR #2926), so under tmux the probe passes; the probe stays as the safety net for a terminal that still hangs.
- History in the notcurses field: `g_browse` says a browse is on; `nc_unbrowse()` is the one reset. The position alone is not enough (it is set before a sent line is added to the history). The list yields ↑/↓ while browsing.
- The notcurses field prints nothing it sent: `nc_echo_line()` (term.c) puts the line returned by `term_readline`/`term_ask_line` into the conversation, through `term_user_band()`, as the ANSI editor does.
- Keys in the notcurses UI (`nc_key_code()`): kitty keyboard protocol (flags 27, Ghostty sends the typed
  character as associated text). A shifted or AltGr key is its typed character (`eff_text`), and Ctrl+letter
  is the capital in `id` (Ctrl+a = id 'A' + Ctrl), so either case.

## Commits

- Every feature or fix gets its own git commit, made when it is done and checked (both builds
  warning-free, `make test` passing). Not one commit for several features.
- Commit only the files of that change. The working tree may hold other people's uncommitted work:
  stage by path (`git add <files>`), never `git add -A`, and leave their changes alone.
- Commit messages say what changed and why, in the imperative mood, with the subject under ~72 columns.
- Push only when the user asks for it.

## Build, run, test

```sh
make                 # builds ./corbie  (needs cc, make, libcjson-dev, libcurl4-openssl-dev; links with -pthread; with libunistring-dev, zlib1g-dev and libncurses-dev found, also the notcurses screen, built from vendor/notcurses)
make release         # clean, optimized, stripped build (RELEASE_CFLAGS)
make test            # C unit tests (tests/test_unit.c): what a change has to pass; no model, no network
make test-unit       # the same, by name (what CI runs)
make test-integration  # pty (and tmux) integration tests (tests/test_integration.py): on request only, some reach the hosted APIs (costs money)
./corbie           # starts with a model from Ollama ($OLLAMA_HOST or 127.0.0.1:11434) or a provider key (ANTHROPIC_API_KEY: anthropic:claude-opus-5 for the run); with neither it starts and says "no model yet" (g_cfg.model is "" then, nothing is sent, /model picks one)
```

`make test` needs no real model: `tests/fake_ollama.py` is a scripted stand-in (`TOOL_TODO` a todo_write list, `TOOL_BG` a background `sleep 317`, `TOOL_MCP` / `TOOL_MCP_FAIL` a call of the echo and shout tools of `tests/fake_mcp.py`, a scripted MCP server for the stdio transport)
(keywords in the user message such as `TOOL_BASH`, `TOOL_WRITE`, `TOOL_FETCH`, `TOOL_SEARCH`, `TOOL_ADVISOR`, `OPTIONS_REPLY` (a reply that ends in a fenced `options` block, the second option recommended), `CODE_REPLY` (a reply with a fenced python block), `TOOL_WRITE_CODE` (writes `hello.py`, previewed coloured), `TOOL_LOOP` (a bash call on every model call of the request, until the loop guard stops it), `TOOL_BINARY` (a bash command whose output is not UTF-8), `CACHE` (the prompt was partly cached: Anthropic's `cache_read_input_tokens`/`cache_creation_input_tokens` in usage, OpenAI's and xAI's `prompt_tokens_details.cached_tokens`), `SLOW`, `ERROR` select a
canned response; an advisor call is recognised by its system prompt and reads `SLOWADVICE`, `CUTADVICE`,
`THINKTAG`, `EVICTMAIN` from the brief — and, as `/orchestrate`'s planner and reviewer, `ONETASK`, `NOPLAN`, `REJECT`, `NEVERACCEPT`, `PARALLEL` (two slow tasks that may overlap; `/_inflight` says how many unstreamed calls were under way at once) — and answers a review (before the request ends) or a check (of the
first change) with a finding unless the request says `APPROVE`. Its models differ in what they can do: `fake-thinker` is on/off,
`fake-levels` is gpt-oss-like, `fake-declared` lists its levels in `/api/show`, `fake-stale` is
under-reported by `/api/tags`, `fake-remote:cloud` exists for `/api/show` only and answers 401. It also
stands in for the hosted APIs — `/xai/v1` and `/openai/v1` (Chat Completions), `/anthropic/v1` (Messages),
key `test-key`, the requests logged at `/_provider_requests` without the key; `grok-fake`, `gpt-fake`,
`claude-fake-opus`, `claude-opus-5` (refusal fallbacks) and `claude-fake-haiku` (a thinking budget), which
read `SLOWADVICE`, `CUTADVICE`, `REFUSEADVICE`. A Messages request that is the model doing the work
(`tools` present, or messages that are not one user string) is scripted the same way — `TOOL_BASH`,
`TOOL_WRITE` and `TOOL_ADVISOR` become `tool_use`, a later `tool_result` is answered `Tool said:`,
and `stream: true` is SSE — while a consultation stays one user string. `fake_ollama.serve(port, cert, key)` serves the same
behind TLS for the https tests). Run the tests after every change to `src/`; `make test` (the unit suite) must pass and the
build must be warning-free with `-Wall -Wextra`.

## Layout

| file | owns |
|---|---|
| `src/common.h` | every shared declaration; one header, sectioned per module |
| `src/util.c` | `highlight_line()` (basic syntax highlighting of one line of a fenced block, by its language: keywords, strings, comments, numbers; only escapes are added), `git_branch()` (the branch of a directory, for the status bar's location line), `sbuf` string buffer, file helpers, config load/save, permission-mode names, the file finder behind `@mentions` (`mention_start()`, `file_complete()` — the tree below the directory typed so far, read breadth first with a bound and kept for a moment, matched by name: starts with / contains / letters in order — and `file_find_named()` for a bare name as sent; `mention_spans()`: the mentions that name something that is there, which the input field colours — a stat each, it runs with every redraw), the same for a `!line` (`shell_word()`: which word the cursor is in and whether a command goes there, `command_complete()`: the programs in `$PATH`, `shell_escape()`: a path as the shell wants it written), URL checks + HTML-to-text + search-result extraction for `web_fetch`/`web_search` (pure logic, so `test_unit` can reach it), the per-model effort table (`effort_get`/`effort_set`, saved as `effort.<model>=<level>`; `effort_supported()` 1/0/-1 and `effort_resolve()`, which sends a high/xhigh/max the model lacks as the strongest level it has), and the pure half of the advisor: `transcript_text()` (the conversation as one quoted text), `advisor_brief()`, `advisor_plan_for()` (same model / cloud / hosted API / another local model — window, `keep_alive`, reply cap, byte budget), the guidance levels (`ADVISOR_GUIDANCE`: consultations per request, answer length, brief size, the automatic review and check) and `advisor_approves()` (an `LGTM` answer), and the rate limits of a response (`ratelimit_header()` parses the `anthropic-ratelimit-*` headers that `http.c` hands back in `http_result.rl` and `http_job`; `ratelimit_set()`/`ratelimit_get()` keep the last per model, shown by `/usage`), the prices behind `/usage` (`model_price()`: `LIST_PRICES`, the providers' list prices per million tokens with the month they were read in its comment, matched by a model's name or a dated snapshot of it and never by a looser prefix; each row has a cached-input column, which `model_cache_read_ratio()` returns for OpenAI, xAI and Anthropic (Anthropic's 10%, 5%, 2.5%), and `model_cache_write_ratio()` 1.25 for Anthropic; `usage_cost_usd()` prices a model's input, cache reads and writes and output; `price_set()`, saved as `price.<model>=<in>/<out>`); `suggest_clean()` (the suggested next message as shown: first line, no quotes, `User:` label or control byte, at most `SUGGEST_MAX`; NULL for `NO_SUGGESTION`, an empty reply or one cut off by its cap); pasted text (`paste_is_block()`: a block is more than `PASTE_LINES` lines or `PASTE_BYTES` bytes, counted as the editor shows it; `paste_clean()`: what a bracketed paste's bytes become as typed text, CRLF/CR to LF, tab kept, controls and DEL dropped, a bad byte to U+FFFD; `paste_tokens()`/`paste_expand()`: the `[Pasted text #K +N lines]` tokens of the editor, and their texts back on submit) |
| `src/http.c` | the HTTP client over libcurl's multi interface: http and https, streamed line delivery, extra headers (`http_headers`), the interrupt fd watched while waiting, the 100 ms idle tick, a timeout on silence rather than on the whole request, `http_busy()` (a request is in flight), `http_job_*` (a POST performed by a thread of its own — the only threads there are; `easy_setup()` is what it shares with `http_request()`), `http_url()` (it never writes a scheme in: a host without one goes to libcurl as written, whose default is http; a portless plain-http host is Ollama's 11434) |
| `src/provider.c` | the hosted APIs. xAI, OpenAI (`xai:`/`grok:`, `openai:`/`chatgpt:`), DeepSeek (`deepseek:`) and Kimi (`kimi:`/`moonshot:`) speak Chat Completions: advisors, and workers of `/goal` (`cc_messages()`/`cc_turn_body()`/`cc_turn_reply()` translate the conversation and its tool calls at the wire); Anthropic (`anthropic:`/`claude:` + model) speaks Messages and can be the advisor or the model that does the work. Keys from `XAI_API_KEY`/`OPENAI_API_KEY`/`ANTHROPIC_API_KEY` and base URLs from `…_BASE_URL`, the model check (`provider_model_info()`: Anthropic's capabilities become a `model_info` — effort levels, adaptive or budget thinking, window, `max_output`), the list of a provider's models for the suggestions (`provider_list_models()`), one non-streamed consultation (`provider_chat()`), and the working-model turn (`provider_turn()` streamed; `provider_turn_request()`/`provider_turn_reply()` unstreamed for a worker — the conversation is translated to the Messages API at the wire and stays in today's shape everywhere else). Pure halves for the tests: `provider_request_body()`, `provider_parse_reply()`, `provider_parse_model()`, `provider_turn_body()`, `provider_turn_reply()`. Hosts the user adds (`/model add HOST URL [KEY_ENV]`, config `host.NAME=URL|KEY_ENV`, main.c `cmd_model_host()`): `provider_host_add()`/`provider_host_remove()` keep a fixed table of `PROVIDER_HOSTS_MAX` after the built-in `PROVIDERS`; `provider_at()` and `provider_find()` see both, with `provider_def.host` set for the added ones (chat-completions only, `example`/`known_as` NULL); the name, URL and key rules are `provider_name_ok()`/`provider_host_url_ok()` and are unit-tested. Gzipped request bodies: `util.c` `gzip_body()` (zlib, `WITH_ZLIB`, `ZLIB=auto` in the Makefile), `gzip_refused()`, `gzip_probe_verdict()` and the config's `gzip.NAME=1|0` (`gzip_get()`/`gzip_set()`); `http.c` `http_request_bytes()`/`http_job_start_bytes()` carry the length and `Content-Encoding`; `provider.c` `wire_gzip()` sends a body of 4 KB+ gzipped to a provider whose setting is on, retries a refusal plain once (`provider_gzip_refused()` turns it off for the session), `provider_gzip_probe()` is `/gzip probe`; the `/goal` worker restarts a refused job plain (`worker_restart_plain()` in main.c); `cmd_gzip()` in main.c. Ollama is never gzipped |
| `src/term.c` | the markdown printer breaks prose at word boundaries on a full screen (`md_prose()`, `md_word_flush()`: the word is held until its end or any escape, and `md_state.col` starts after the `● ` marker, set in `ollama.c`); the chrome as sent is cached per row (`chrome_send()`, `g_chrome[]`: a row whose text did not change is not sent again; cleared by `sb_pause(true)`, `conv_repaint`, `chrome_append`, `term_fullscreen` and a resize in `layout_sync`; a keystroke then sends the text row and the caret, about 140 bytes instead of 1 KB); the markdown printer buffers a fenced code line and prints it highlighted by the fence's language (`md_code_line()`); the bottom chrome is the input field and the status bar on the last row: the folder and branch (`loc_text()`: the working folder as an absolute path, and its git branch, in a muted rose, `LOC_ROSE`, the path cut from the left, at most two thirds of the row) sit at the right end of the bar; the mode is on the field's lower rule (`mode_text()`, `term_mode_text(sbuf, cols)`: its hint is dropped when the row is too narrow for it), right-aligned two cells in from the edge; the bar has no busy label: a model's thinking and waiting are the conversation's spinner (`ollama.c` `on_idle()`), and a tool run shows its header when it is done; the model is `model_short()` (no provider prefix, no `:latest`); the two bottom rows have the window's own background (no fill: they read as part of the window; `sb_bar_row()` pads the row with spaces so a reset in the text leaves nothing undrawn, and notcurses draws the bar on its default background too)` re-applies it after each reset and pads the row with spaces: an erase-to-end-of-line with the background made tmux drop the next row's background, and a full-width row has the same effect in tmux's capture); notcurses draws the same colour flat (`C_BAR_A`), not the gradient) — `fs_reserved_for()` counts them, `bar_draw()` draws the bar on the row above the location line; raw mode, key decoding (`read_key`/`csi_key`, incl. Shift+Tab = `CSI Z`, and the mouse: reports are asked for in full-screen mode, `MOUSE_ON`, so the wheel arrives as `K_WHEEL_*` — over the conversation only, `mouse_key` — instead of as ↑/↓ keys; `stdin_wait()` waits in `pselect` so a resize is seen at an idle prompt), full-screen mode (alternate screen, scroll region, and the chrome the app keeps at the bottom — the input field (`field_draw`: a rounded box, `│ ❯ text │` with `╭╮╰╯` corners (`field_edge()`, `FIELD_X0`, `field_width()` = cols − 4; the notcurses frame is `draw_field()`, text from column 4, `field_text_width()` = cols − 6). The text wraps at word boundaries in both UIs: `term_wrap()` is the one walker (a row breaks after its last space; a longer word is cut), used by the ANSI field, the editor's cursor, and the notcurses rows (`field_starts()`). The notcurses caret is a light cell with a dark letter, drawn by us (its hardware cursor is not used); reverse video does not show there; its edges are green while the prompt waits; it grows to `FIELD_MAX_ROWS`; `put_mentions` colours an `@path` that is there, with escapes only, so the wrapping is that of the plain text), the suggestion list under it (`sug_*`, `g_sug_rows`: what `term_suggest` — main.c — offers for the `/command`, option, `@file` or word of a `!line` at the cursor; a preview until ↓/Tab steps into it — for the editor's text at the prompt, and for the pending type-ahead while the model works: `ta_sug_sync()` keeps that list in step from `term_status_refresh()`, `ta_sug_key()` acts on its keys at once) and the status bar — `layout_sync` re-cuts the region whenever the terminal or the chrome changes size and paints the conversation into it again from the scrollback model (`conv_repaint`, which leaves the cursor where output continues by printing its row up to it) — mode · model · queued messages · session tokens · ctx · busy spinner via `term_busy()`/`term_busy_tick()` (at the right end; `● ready` in green when idle); `chrome_append()` re-appends both after any `\x1b[J`), line editor (draws into the field and owns the cursor there — `g_field_focus`; conversation output from inside it goes through `ed_conv_out()` at `conv_pos()`, the screen row the scrollback model says the transcript reached), prompt history (latest 100, `hist_*`), the message queue (`term_queue_*`: Enter while busy turns type-ahead into a queued message; pending type-ahead is echoed live in the input field and Shift+Tab while busy cycles the mode at once; `term_queue_pop_plain()` steps over a queued `/`/`!` line to the message behind it; a queued message never stops work in flight), `term_confirm` menu, `term_select` picker, the task list (`scroll_modal_impl`) and the `term_confirm` question (on the full-screen ANSI screen these are held in the *band*: rows of the chrome between the field and the bar, where the suggestion list goes and instead of it — `g_band_text`/`g_band_n`/`g_band_rows`, `band_set()`/`band_clear()`/`band_room()`/`band_draw()`, sized by `field_sync_rows()` and counted with the suggestions in `fs_reserved_for()`/`field_top()`; a menu sets its text and calls `term_status_refresh()`, which re-cuts the region and draws it. Its height is fixed when it opens, so a filter does not move the conversation. On a terminal too short for the band, the picker and the question are inline, printed where the output stands and redrawn over themselves; `/usage`'s overlay and the paste viewer are still boxes over the conversation), pasted text (a paste of more than 3 lines or 800 bytes is the token `[Pasted text #K +N lines]` in the editor buffer, `paste_insert()`/`paste_del()`; the text is kept in `g_paste[]` per prompt and put back by `paste_result()` → util.c `paste_expand()` on submit; a click on the token (`mouse_key` notes it in `g_click_frow`, `paste_click()`) opens `paste_modal()`, a scrollable window over the region), `term_agents_menu` (← on an empty prompt line: main.c's sub-agents list, run through `ed_hook_run`, the idle hook's way; `term_show_block()` shows a log as an open fold and moves the viewer to its head), the suggested next message, the placeholder of an empty prompt (`term_hint`, `term_hint_set()`/`term_hint_clear()`, `term_typed_ahead()`): muted in the full-screen field (`field_draw`) and inline (`ed_refresh`, only when it fits the line); Tab takes it as text; the first key that is not Tab, an arrow, Home, End, a mouse or wheel event or a resize clears it, and Esc clears it without counting toward the rewind's Esc; `term_readline()` clears it on entry, so it only ever holds what this prompt's idle work set; idle work at the prompt (`term_idle_hook`/`term_idle_ms`: the hook may ask to be called again: `idle_armed` is set again after it runs (`prompt_arm()` in main.c sets `term_idle_ms`) — `read_key_wait()` gives up after the timeout and `ed_idle_run()` releases the field, runs the hook with the terminal in its during-a-turn state, and redraws — and a message queued while the hook ran is returned as the submitted line, so the REPL runs it right away), streaming markdown printer, folds (`term_fold_begin`/`term_fold_end`, `fold_click`: a long tool result as one clickable line), scrollback for the wheel and PgUp/PgDn (stdout is hooked via `fopencookie`/`funopen` in `sb_hook_stdout` and conversation output is modelled as lines — `sb_feed`; the chrome/menus/viewer draw with `sb_pause(true)`, while the submitted line is printed as real output; `g_view_top` ≥ 0 means scrolled back: `sb_write_fn` then records conversation output without painting it, and `view_scroll`/`view_paint`/`view_leave` move, draw and leave the window; `the overlay (`term_overlay_show()`/`term_overlay_close()`: `/usage` on a full-screen conversation, a centred box from `overlay_box()` painted over the rows by `conv_repaint()`/`view_paint()`; output is recorded, not painted, while it is up, as while scrolled back; Esc closes it first, then the viewer and the model; the notcurses UI draws it as a modal over the pane (`nc_overlay_set()`, `draw_overlay()`; Esc closes it)), `view_note` is the line under it that says where it is, a chrome row of its own for as long as it is up — called from `ta_type` while the model works, and from `scroll_view`, which adds the keyboard, at the prompt) |
| `vendor/notcurses/` | the notcurses 3.0.17 core the frontend runs on, copied from upstream (`src/lib`, `src/compat`, two `src/media` files, the public headers), with the tmux 3.4+ DA1 fix (PR #2926) and the Enter byte (`load_ncinput()`) and the U+FFFD for a bad byte (`process_input()`) in `src/lib/in.c`; built by the Makefile when libunistring, zlib and terminfo are there. Its own README lists what was copied, the changes from upstream, and how to update it; do not edit it for our needs beyond those |
| `src/term_nc.c` | the notcurses frontend (the placeholder: `draw_field()` draws `term_hint` muted from column 5 of an empty field; `readline_loop()` clears it, takes it on Tab and on Esc clears it only, and re-arms `idle_at` after `term_idle_hook()`) (built with the vendored core; stubs otherwise): planes for the conversation, the input field, the status bar and location line, overlay menus for `term_confirm`/`term_select`; `nc_sgr_parse()`, the pure SGR parser behind the pane (no notcurses types, so `test_unit` reaches it in both builds). Entry points are the `nc_*` functions declared in common.h's `term_nc` section |
| `src/theme.c` | the colour themes: `THEMES` (the default is the palette notcurses has always drawn), `theme_set()` (`theme=NAME` in the config), `theme_fg_code()` (the theme's colour of a basic code: 30-37, 90-97), `theme_map_bytes()` (the ANSI screen maps the basic codes of every write to the terminal through it, `sb_write_fn`, carrying an escape sequence a write ends inside; `38;2`/`38;5` parameters are left as they are, except the orange), and `th_fg()`/`th_bg()` for the chrome's RGB; `user_bg` and `user_text` are the ground and the text of a line the user sent, drawn by `term_user_band()` (term.c: the ANSI editor's submitted line and queued messages; in the notcurses UI `nc_echo_line()` does the submitted line, since `nc_readline` prints nothing). They are stored as theme roles, SGR 1002 (ground) and 1001 (text) (`THEME_ROLE_*`, `theme_role()`), which `theme_map_bytes` and `nc_sgr_parse` turn into the current theme's colours, so a theme switch recolours the sent lines already on screen too; re-applied after each reset, padded to the width. `term_nc.c` reads the same table (`nc_sgr_parse()`). `window` is the window's own ground and the default text colour: 0 (the default theme) leaves the terminal's own; the bright theme paints white. The full-screen ANSI screen paints it (`theme_paint_window()` from `term_fullscreen`, `theme_base_sgr()` before the clear, and `theme_map_bytes` maps a reset, 39 and 49 to it, so an erase takes it too); notcurses sets its standard plane's base cell and `chans()` takes it for the defaults |
| `src/tools.c` | the `exit` tool (`tools_exit_asked()`: the model judged the user is leaving; main.c's `process_input` ends the program after that turn, the typed `exit` is handled there too); `preview_lines()` takes the file's extension as the language of a `write_file` preview (highlighted when the line fits); tool definitions + implementations (incl. `task`, which calls back into `main.c`, and `todo_write`: the task list, `tools_todos_get/set()`, `tools_todo_render()`, saved as `todos` in the session file and not offered to sub-agents), `confirm()` (mode-aware; 4th menu option saves a rule to the config directory's `permissions/` file for the project, `tools_permissions_*`; never read from the project tree), checkpoints for `/rewind` (`tools_checkpoint_*`: pre-write file states per request, and per *step* of one — `tools_checkpoint_diff()` — for `/orchestrate`'s reviews), shell runner (`run_shell`, also how `web_get()` drives `curl`/`wget` for `web_fetch`/`web_search`); background bash (`bg_start()`: the output to a TMPDIR file, the job in its own process group with no inherited descriptors; `bash_job` = `t_bash_job()` reads and kills; `bg_reap()` with WNOHANG; `tools_bg_init()` sets atexit and SIGTERM/SIGHUP cleanup, `tools_bg_kill_all()` is also run before `/ui` re-execs; sub-agents may not start one), and the sandbox (`sandbox_command()`: the bwrap line, the directory and the command as shell words; `sandbox_available()` asks once with a trial run and says why not; `t_bash()` wraps the model's commands, not the `!` lines, and `bg_start()` gets the wrapped line; `/sandbox` = `cmd_sandbox()`; the config key `sandbox=bwrap|off`), the project's hooks (`tools_hooks_*`, kept in the config dir's `hooks/`; `hook_match()` pure; `hook_pre()`/`hook_post()` run around the model's calls in `tools_execute()`, `hook_run()` through `run_shell()` with the event on stdin; `tools_hook_event()` for start/stop), and the project's permission rules (`perm_rule_verdict()`, pure, with `glob_match()`: allow lines as before, and `deny|ask TOOL PATTERN` guard rules with `perm_verdict()` — deny beats ask beats allow; `confirm()` normalises an edit's path with `perm_path_norm()` and checks deny/ask before `always_flag`, YOLO and the mode, `confirm_body()` does the prompting) |
| `src/ollama.c` | `/api/chat` streaming (options: `num_ctx`, `temperature`, `keep_alive` — a bare number goes out as a JSON number, the server refuses `"-1"` — and per-call `ollama_call` overrides: `think`/`followup`/`num_predict`/status-bar label for the memory, compact and sub-agent calls, `model`/`info`/`num_ctx`/`keep_alive`/`idle_ms` for a call to another model), `/api/show` (`model_info`: capabilities, family, `RENDERER`, and what `think` may be set to — the server's `thinking.values` from Ollama 0.34.3, else `model_think_profile()`'s small table), `think_decide()` (the one place that says what is sent as `think`), `ollama_chat_request()`/`ollama_chat_reply()` (the same call unstreamed, as a body and a parsed answer, for a worker whose HTTP runs on a thread), tool-call accumulation, text tool-call recovery, `/api/ps` model placement |
| `src/skills.c` | SKILL.md discovery (order, first found wins: `.corbienest`, `.agents`, `.claude` in the project; the config dir; `~/.agents/skills`; the bundled skills; `~/.claude/skills`), frontmatter parsing, `/NAME` expansion, scaffolding. The bundled skills are the repository's `skills/` folder (source `bundled`, read last, read-only): the Makefile defines `BUNDLED_SKILLS_DIR` ($(PREFIX)/share/corbienest/skills, which `make install` fills) and `BUNDLED_SKILLS_SRC` (the checkout's skills/) for skills.o alone, with a `.skills-*` stamp. Ship only skills that work in any project (`skills/long-tests` does; a project-specific one belongs in `.corbienest/skills`) |
| `src/stream.c` | stream-json lines, pure: `stream_messages()` writes one JSON line per message from an index on (tool calls queued and matched to their results, system messages left out), `stream_input_text()` reads one `--input-format stream-json` line; main.c writes the buffer to the real stdout |
| `src/agents.c` | sub-agent files: `.corbienest/agents/NAME.md`, `.claude/agents/NAME.md` (this project), `<config>/agents/NAME.md` (yours) and `~/.claude/agents/NAME.md` (Claude Code's, last); frontmatter name, description, tools (a subset of the sub-agent tools), model; the body is the role. `agents_load()`, `agent_find()`, `agent_allows()`, `agents_prompt_section()` (the main prompt names them). `task` takes `agent=NAME`; `run_subagent()` (main.c) applies the role, the tool subset and the model of that run (a parameter of `run_subagent_as()`: `g_cfg.subagent` is not touched); Claude Code's tool names and the aliases sonnet/opus/haiku are understood |
| `src/mcp.c` | MCP servers over stdio: the servers (`mcp/<hash>.json` in the config dir, `mcp/global.json`; never the project tree), spawn with fork+execve (no shell; pipes close-on-exec; own process group; PDEATHSIG; the environment with the server's keys replaced), JSON-RPC lines with `await()` (poll on the pipe, `term_poll_interrupt()` for Esc, a cancel notification, server requests answered: ping ok, others refused), `connect_one()` (initialize, notifications/initialized, tools/list paged by nextCursor), `mcp_call()` (tools/call, 120 s limit, isError → TOOL_ERROR), tool names `mcp__SERVER__TOOL` sanitised to `[a-zA-Z0-9_-]{1,64}` with a hash when cut (`mcp_tool_name()`), cleanup by atexit and the SIGTERM/SIGHUP handler (`mcp_kill_signal()`). Calls are confirmed as kind `CONF_MCP` (allow rule `mcp NAME`); workers, sub-agents and plan mode do not get the tools |
| `src/main.c` | the suggested next message: `suggest_next()` runs after a finished reply (`g_sug_due`, set in the REPL when `process_input` returns 0; one try per reply, `g_sug_seen`) as one quiet `model_chat` with `messages_with_system()` plus `SUGGEST_ASK` (think off, `SUGGEST_TOKENS` cap, the same tools, tool-call replies discarded, dropped when anything was typed or queued meanwhile; never touches `g_messages`, the session or the memory); `prompt_idle()` is the one idle hook (the suggestion, then memory's flush at its deadline `g_mem_due`, set from the prompt's start), `prompt_arm()` sets `term_idle_ms` from what is due; `cmd_suggest()` is `/suggest on|off` (config `suggest=`). REPL (`process_input`), slash commands (`SLASH_CMDS` with the line the suggestion list shows for each, `SLASH_ARGS` with their options; `suggest()` is the `term_suggest` hook, `suggest_advisor()` its half for `/advisor`'s and `/model`'s model names — the hosted providers by prefix (`/model` only Anthropic), the installed models, a provider's own list behind its prefix — `suggest_shell()` its half for a `!line`: earlier `!lines` that go on from what is typed, programs where a command goes, paths by the start of their name elsewhere), `@file` mentions (`expand_mentions`), system prompt, sub-agents (`run_subagent`, hooked into `tools.c`'s `task` tool via `tools_subagent`; each run keeps a plain-text log in its `subagent_rec` — `agent_begin()`/`agent_say()`, capped by util.c's `agent_log_add()` — which `agents_menu()` shows by ←), agent loop (`run_turn`, a wrapper that fires the stop hooks after `run_turn_body`, and which injects queued messages via `inject_queued()` between tool rounds, so they reach the model on its next call; a queued message never skips a call of the round in flight — and calls `shrink_context()` before every model call — elide old tool results in place at ≥`ELIDE_PCT` (`elide_old_tool_results()`, also run by `begin_request()`), auto-compact at ≥`AUTO_COMPACT_PCT` (`maybe_auto_compact()` → `compact_conversation(resuming, request)`; the first call of a turn does not compact a request over `COMPACT_REQUEST_MAX` bytes, a pasted block, which is sent whole and compacted after the reply (`shrink_context()`, `first_call`); mid-turn `resuming` is true, so the compacted conversation ends with a *user* turn — the summary plus `pending_request()`, the user's own words — and the model carries on with the half-done job; at the prompt it ends with an assistant turn instead, so what the user types next follows one) — both judged by `ctx_estimate()`, since a whole tool round lands between two measurements: it adds the bytes appended since the last `prompt_eval_count` divided by `g_bytes_per_token`, which `ctx_measured()` calibrates on each measured call. Overflowing is not a soft failure: Ollama drops messages from the front until the prompt fits, and once the user's own request is gone the model answers a conversation without the question in it (qwen3.8 refuses outright — "no user query found in messages"), so `tool_result_message()` caps any single result at `TOOL_RESULT_PCT` of the window and `run_turn` recovers from that error by eliding, then compacting, and retrying. A reply with neither text nor a tool call (`reply_is_empty()` — Ollama's parser dropping a badly rendered tool call looks like this) would end the request in silence, so `run_turn` says so, drops the empty message and asks once more), `/ctx` picker (model max from `/api/show`), sessions (`session_save()` after each request to `config_dir()/sessions/<id>.json`, `--continue`/`--resume`/`/resume`; `typed_note()` records each line the user typed with the message it became (`typed` in the file): `session_load()` puts those lines in the ↑ history, and `replay_conversation()` shows the conversation again, for an interactive run only), project memory (`.corbienest/memory.md`: `load_memory()` into the system prompt; `memory_note()` after each request counts it and `memory_flush()` runs the quiet extraction (`memory_work()`: one unstreamed call through `memory_call()`, `ollama_call` overrides; interactively `memory_spawn()` hands it to a worker process, `memory_here()` runs it in-process for `-p`, and `memory_reap()` collects a worker at the prompt through the background check (`term_bg_ready` = `memory_done()`, `term_bg_run` = `memory_reap()`, every `TERM_BG_MS`, typed or not: the field's text does not hold it up; term.c `K_BG`, term_nc.c `bg_at`)) every `memory_every` requests (with suggestions on and `memory_idle` > 0 that flush waits for the idle hook, so the suggestion request goes to the server first: one request at a time there), after `memory_idle` seconds of an untouched prompt (`memory_arm_idle()` sets `term_idle_ms` before each `term_readline()`; the editor calls back into `memory_idle_hook()`), and before the conversation goes away — exit, `/clear`, `/compact`, `/cd`, `/resume`; `/memory`; the lock is `memory_lock()`, one file per project in the config dir, held across read, call and write; the write is skipped if memory.md changed meanwhile), `/effort` (`effort_command()`, which also serves `/advisor effort`; `/think` says *when* the model thinks, `/effort` how hard), the advisor (`consult()` — one toolless call to `g_cfg.advisor`, through `ollama_chat()` or `provider_chat()`, that is shown `advisor_brief()`, accounted in the session totals but never in the context estimate; its advice is a fold (`term_fold_begin`, `fold_body()`: the head says how long it is, a click shows it; the preview only where there is no screen to click) — behind `run_advisor()`, hooked into `tools.c`'s `advisor` tool via `tools_advisor` and counted per request in `g_advisor_uses`, and behind the guidance's automatic consultations: `advisor_review()` when a request that changed files ends, `advisor_check()` before its first `write_file`/`edit_file`; `/advisor`, `/advisor guidance`), `/subagent` (`cmd_subagent()`: the model the sub-agents run on, saved as `subagent=`, checked with its server and refused when it is advisor-only; `/subagent effort` sets its effort, kept per model; `run_subagent()` calls it through `ollama_call.model`/`.info` and books its tokens with `account_as()` under that model's name), the options picker (`options_choose()`: the fenced `options` block of a finished reply, offered once per reply by its conversation length, called from the REPL loop after the queue drains; the choice is the next message), `/orchestrate` (`cmd_orchestrate()`: the advisor model plans — `orch_ask()` over `advisor_call()`, the one toolless call `consult()` makes too; the plan is text, read by `orch_parse_plan()` in `util.c` — then `orch_schedule()` runs the tasks on the main model, those whose `after` set is accepted at the same time up to `/workers`, and whose `OWNS:` paths overlap no running task (`orch_schedule()`, `owns_overlap()` in util.c) — a task's `ROLE:` skill sets its worker's prompt and model (`worker_begin()`, `worker_role_model()`) and the plan's `CONTRACT:` is shown to each worker — `worker_begin()`/`worker_step()`/`worker_review()`, each worker a conversation of its own whose model call is an `http_job`; the review is shown `tools_checkpoint_diff()`, what the task's checkpoint *step* did to the files), `/hooks` (`cmd_hooks()`, the list kept in tools.c), `/agents` (`cmd_agents()`: list, reload), `/mcp` (`cmd_mcp()`: add, remove, reconnect; `tools_rebuild()` rebuilds the tool list with the servers' tools), stream-json (`stream_flush()` after each reply and tool result writes the new messages as JSON lines to the real stdout, `g_stream_fd`; `stream_run()`/`stream_prompt()` run the requests, `stream_input_text()` reads `--input-format stream-json`; what the run prints is sent to /dev/null, as in json mode), `/usage` (`usage_add()` from `account()`/`account_as()`: tokens per model as each API counted them, kept in the session file; `cmd_usage()` prices the hosted ones — an estimate, and it says so), banner (a framed box, the crow and a spark round its border, shown as a modal: at startup the transcript keeps one line and the box is the window (`banner_modal()`), `/banner` brings it back (`cmd_banner()`); the box is `box_lines()`, rebuilt on every tick by `banner_tick()`, which only runs while the modal is up (`term_banner_open()`). On the ANSI screen the modal is the overlay (`term_banner_show()`/`term_banner_draw()`: the overlay state with a bare box, its close button `[×]` (`BANNER_X_FROM_RIGHT`, three cells: a click on any of them), a click on it is `K_BANNER_X`, Esc closes it); notcurses draws it over the pane (`nc_banner_set()`, `draw_banner()`), a click on the × or Esc closes it. The tick runs only while the top-level prompt waits (`term_readline()`, `read_key_prompt()`, notcurses' `readline_loop()`), any line sent closes the modal first (`term_readline()`: a reply must not land under it), and the first real task (not a `/` or `!` line) stops the animation for good, on the settled frame, so a later `/banner` shows it still; not in `-p`, pipes, or `--benchmark`), `--benchmark` (`run_benchmark`: for each size in `ctx_size_list()` — shared with the `/ctx` picker, which passes `all` for a local model so every standard size up to 1M is offered (those past the trained length marked) — `bench_ctx()` does a warm-up call (load time, `/api/ps` placement) + N quiet capped generations, tok/s from Ollama's counters; no session, memory or tools; `--draft N` = `options.draft_num_predict`, `ollama_model_draft()` reads the model's own) |

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
- Everything printed to stdout in full-screen mode is recorded for PgUp scrollback. Transient
  drawing (redraws that move the cursor up, menus, spinners that stay put) must either be
  bracketed with `sb_pause(true)`/`sb_pause(false)` or use only `\r` + `\x1b[2K`/`\x1b[J` and
  cursor-up (which the model understands). Never write to fd 1 directly.
- The cursor during a turn rests in the input field, on the type-ahead's caret (`want_box()`, `chrome_park()`,
  `g_ta_caret_row/col` in term.c), so the user sees where typing goes. Transcript output must not land there:
  `sb_write_fn` first moves the cursor to the transcript's end (`transcript_move()` with `conv_pos_x(..., false)`:
  never flush inside the hook) and parks it back after the bytes. `g_away` says the cursor is not known to be at
  the transcript's end: set it whenever the cursor moves into the box, clear it only on a move to the transcript
  (a stale true costs one move; a stale false sends text into the box). Anything that draws below the transcript
  (a menu, the viewer, the paste modal, a question) is wrapped in `park_hold()`/`park_free()`. The cursor is parked
  again only after a chunk that ends between sequences (`tail_incomplete()` in util.c): a chunk can end anywhere. In notcurses the
  hardware cursor is enabled on the caret cell (`nc_render`, `g_caret_y`).
- The bottom chrome (input field + status bar) is drawn with absolute cursor moves and always
  with the capture paused: it is not conversation content, and the model would misread the
  jumps. Never query the terminal for the cursor to find the conversation again — a pty with
  no emulator behind it (the tests) never answers `CSI 6n`; use `conv_pos()`, which derives the
  row from the scrollback model (flush stdout first: the model only sees what left stdio).
- The scrollback model is what the conversation region is *meant* to show, and `layout_sync()`
  holds the screen to it: on any change of geometry it paints the region again
  (`conv_repaint`) instead of trusting what the terminal — or tmux — made of the old picture.
  It returns true when it did; a caller with something transient drawn below the transcript
  (a menu, the inline editor) starts over from the cursor it left (`drawn = 0`). So whatever
  stays on screen as conversation must go through stdout unpaused (or `sb_note`), or the next
  repaint drops it.
- A `bash` call is one grey line (`● bash(summary)` + the output's fold, `main.c`: the header is not printed
  before the call but as the result's `lead`, `print_result_folded(…, lead)`; a denial is `… denied`). Its
  approval is not echoed (`term_confirm_quiet`, set by `confirm()`): a one-line command the user is asked
  about is in the question itself (`cmd_in_question()`), so no `$` line. A command of several lines the user
  is asked about is shown in full as a `$` block before the question (`print_command()` in `tools.c`), since
  an approved command is shown in full; one not asked about is folded (first line as the head, the command as
  the body). `confirm_will_ask()` mirrors `confirm()`'s early returns and must be kept in step with it.
- A fold is part of the scrollback model, not of the screen: `term_fold_begin(head)` …
  `term_fold_end()` record the body as lines that are `hidden` (`g_sb_meta`), and `sb_rows()` —
  what the region, the viewer and `conv_pos()` are all computed from — leaves them out, so a
  closed fold cannot put the model and the screen at odds. The body is written into the model
  without being painted (`g_fold_rec`, like output while scrolled back), so it must be plain
  text: no escapes or `\r`, which would move the model's cursor (`print_result_folded()` in
  `main.c` strips them). A click is noted by `mouse_key()` (`g_click_row`) and acted on by
  whoever read the key — the editor, the viewer, the type-ahead (`fold_click()`) — never from
  inside a menu, and the region is painted again from the model as after a resize.
- The wheel and PgUp/PgDn belong to the conversation, ↑/↓ to the input field (history, the
  lines of the message, the suggestion list) — never let a mouse report act as a key there,
  and never type one into the field: `read_key()` returns `K_MOUSE` for what nothing acts on.
  While scrolled back (`g_view_top >= 0`) conversation output is recorded, not painted, so
  anything that needs the live screen — a question, a menu, an interrupt — calls `view_leave()`
  first; Esc while scrolled back leaves the viewer and nothing more.
- Anything that waits (HTTP streaming, shell commands) must poll `term_poll_interrupt()`: it is
  what keeps type-ahead alive and turns Enter into a queued message, and only Esc and Ctrl-C stop
  the work. Queued messages are only consumed by `main.c` — between tool rounds (`inject_queued()`,
  which takes the plain messages and steps over any queued `/`/`!` line, since that one is the
  REPL's) and after the turn (`process_input` loop); after an interrupt they go back to the editor
  (`term_queue_to_editor()`), never silently dropped. A queued message never stops or cuts a call
  in flight: a shell command, a sub-agent, a worker, a consultation runs to its end and the message
  waits for the next model call (or the next request). The prompt's idle work is busy in the same
  way (the suggestion is a model call of its own) but is *not* inside that REPL loop, so
  `readline_impl` submits the oldest queued line itself once `ed_idle_run()` returns — otherwise
  a message queued during that call would sit there until the user pressed Enter a second time.
  Anything else that runs work at the prompt has to do the same. The memory update is a worker
  process now and does not hold the prompt at all.
- While scrolled back the status bar stays the status bar. Where the window is and how to get
  back goes on a line of its own under the conversation (`view_note`; `fs_reserved_for()` gives
  it a row while `g_view_top >= 0`): the bar is where the mode, the tokens and what the model is
  doing are looked for, and a bar that turned into "thinking · scrollback · rows …" was reported
  as a rendering error. Only a terminal too short for the input field still says it in the bar.
- The suggestion list has two owners, one at a time. With the editor in the field it is the
  editor's (`sug_update()`); while the model works it belongs to the pending type-ahead
  (`g_ta_sug_for` is the text it was worked out for — once per change of that text, not per
  refresh). `sug_clear()` forgets the owner, and whoever takes the field over calls it first
  (`readline_impl`, `ed_idle_run`): a list built for other text has offsets that are wrong for
  this one, so the editor must never act on an inherited one. The
  list's keys while busy — Tab, ↓/↑, Enter on a highlighted row, Esc — are taken out of the
  type-ahead and act at once (`ta_sug_key()`); Esc closes the list before it stops the model,
  as it leaves the viewer before it does. `term_suggest` is therefore called from inside the
  poll of a live request too, like a while-busy command: it reads (the command table, the
  disk, `$PATH`, the history) and changes nothing. The model names behind `/advisor` and `/model` are the one
  thing it asks the network for (`model_list_get()`: Ollama's `/api/tags`, a provider's
  `/models`, silently, kept for a minute, a failure included) — and never while a request is in
  flight (`http_busy()`): http.c's settings are globals, so there it offers what it already has.
- `CORBIENEST_TRACE=FILE` records a session for `tools/trace_replay.py`: every write to the
  terminal (`sb_write_fn`, the one place output leaves from in full-screen mode), every byte
  read from it (`in_read()` — never `read()` stdin directly, or the trace has a hole), and the
  layout, viewer and activity changes (`trace_note()`). A drawing problem that shows in one
  terminal only is found by replaying its trace, not by guessing: ask for one. The file holds
  the conversation, so it is created 0600 and the banner says when one is being written; keep
  it a recording — nothing may behave differently because it is on.
- Enter while busy on a slash command first goes to `term_run_while_busy` (main.c's
  `run_slash_while_busy` / `slash_runs_while_busy`), which runs it there and then when it only
  reports state or flips a setting. Such a command runs *inside* the poll of a live request:
  it must not touch `g_messages`, start an HTTP request (ollama.c's callbacks are global — guard
  with `g_while_busy`, as `check_model_placement()` does), ask the user anything, or `chdir`.
  Anything printed from there calls `term_line_break()` first (the model may be mid-sentence).
- Tool confirmations obey `g_cfg.mode` (`MODE_MANUAL`, `MODE_ACCEPT_EDITS`, `MODE_PLAN`,
  `MODE_AUTO`); route new mutating tools through `confirm(what, CONF_EDIT|CONF_BASH|CONF_FETCH, ...)`
  in `tools.c`. A new kind needs its own `perm_match()` case, its rule text (`bash <words>`,
  `fetch <host>`) and the two labels in `confirm()` — and `/permissions` in `main.c` has to be
  able to name it. Plan mode must stay read-only: `tools_for_mode()` in `main.c` drops
  `write_file`/`edit_file` from what the model sees; `web_fetch` stays, reading is not writing.
- `web_fetch`/`web_search` shell out to `curl`/`wget` through `web_get()` — their size caps and
  wget fallback predate libcurl in `http.c`, and moving them over is a change of its own. They do
  not follow redirects (curl without `-L`, wget `--max-redirect=0`): a 3xx is an error that names
  the address, which the model fetches as a call of its own, so every hop is checked.
  Their URL checks, HTML-to-text
  and result extraction live in `util.c`, not as statics in `tools.c`, because `tests/test_unit`
  links every object but `main.o` and only sees what `common.h` declares — put pure logic there
  so it can be tested. The search engine is a URL template (`search_url`, `%s` = the query), so
  nothing in the code assumes DuckDuckGo: the extractor works off the shape every result page
  has (a link, repeated for the display URL and the snippet, often behind a redirect).
- What is sent as `think` is decided in `think_decide()` and nowhere else; callers only say what
  kind of call it is — `ollama_call.think = 0` for one that needs no thinking, `.followup` for one
  that continues the prompt of the call before it (a tool round, a compaction). The distinction
  is the prompt cache: the models that have effort levels write the level into the *top* of the
  prompt (gpt-oss `Reasoning: high`, qwen3.8 `Reasoning effort is set to …`), so a level that
  changes between two calls makes the server read the whole conversation again. Hence a level
  of the model's own goes with every call that shares the prompt, a model that cannot stop
  thinking is never sent `false` mid-prompt, and only on/off flips between the first call of a
  request and its tool rounds (not for glimmer, `think_off_top`: it writes "none" up there too). A model without the thinking capability is never sent anything
  but `false` — the server answers anything else with a 400.
- Capabilities and effort levels come from `/api/show` (`g_model_info`), not `/api/tags`: the
  list reports a model as it was pulled. A call to another model passes `ollama_call.model`
  together with that model's own `.info`; never read `g_model_info` for it.
- The advisor is a tool like `task`, but its failures must never end the turn: every outcome of
  `run_advisor()` is a tool result. Never remove a tool to enforce a limit — the tool list is
  part of the prompt, so that would cost the cache mid-task; answer with an error result
  (`advisor_guidance()->uses`). For the same reason the tool's description says nothing that
  depends on the guidance: how often it may be consulted is in the system prompt. Its prompt
  tokens go through `account()` only, never into `last_prompt_tokens`/`ctx_measured()`: that is
  another model's prompt, and counting it would make auto-compact fire on a conversation that is
  nowhere near full.
- A consultation corbienest makes itself (guidance strong/max) enters the conversation as an
  `advisor` call on the agent's own reply plus its result (`advisor_inject()`, or the held-back
  change's own tool result) — never as a `user` message: the user did not say it, and a
  conversation of calls and results is the one shape every chat template renders. Each happens
  at most once per request (`g_advisor_reviewed`/`g_advisor_checked`/`g_advisor_stepped_in`, reset in
  `begin_request()`), whatever comes of it, so it cannot loop. A small model does not ask when
  it should, so being told to is not what the levels rest on: corbienest asks for it — from `normal`,
  `advisor_step_in()` once `ADVISOR_FAIL_ROUNDS` rounds of tool calls in a row had a failure
  (`tool_result_failed()`); from `strong`, the review (a request that changed `ADVISOR_REVIEW_LINES` lines or
  `ADVISOR_REVIEW_FILES` files, any change at `max`; `tools_request_changes()`).
  To make a level lean harder, add a trigger of that kind; stronger wording alone changes little.
  The agent's own limit (`advisor_guidance()->uses`) is per request, and so that a request of an
  hour is not held to the number that suits a question, `advisor_window_roll()` starts the count
  — and the one stepping-in — again every `g_cfg.advisor_window` seconds (`/advisor window`, in
  minutes; 0 = never). The clock is what bounds it; nothing else may reset the count mid-request
  but a message of the user's.
- A request body must be valid JSON, and every tool call needs its result right after it: the
  Messages API refuses both, and so does the fake (`_messages`, `do_POST`). Every tool output is made
  valid UTF-8 in `tools_execute()` and `tool_result_message()`; a cut of text goes through `utf8_cut()`,
  never a byte offset. The loop guard does not stop a request when `/max_iters` rounds have run: that round runs, `add_loop_note()` adds a user message that starts with `[loop guard]` (asking for a short review and for what should change), and the count starts again; `/rewind` skips such notes (`is_loop_note()`). A request still going after `LOOP_GUARD_CHECKS` (3) questions is stopped. A stopped request (that backstop, interrupt) answers the calls it did not run
  (`answer_calls_unrun()`), and `begin_request()` runs `messages_repair()` for a conversation that
  already has a gap (an old session). `make test` covers the helpers (`wellformed`); the integration
  suite covers the whole path (loop guard, then `--continue`; binary output).
- Threads exist in one place, `http_job_*` in `http.c`, and do one thing: perform a prepared
  POST on a handle of their own and store the answer. Nothing else in this program is
  thread-safe and nothing else needs to be — keep it that way. `/orchestrate` runs several
  workers at once by making each a state machine on the main thread (`orch_schedule()` steps one
  whenever its job is done): a worker's tools, `confirm()`, every `printf`, `account()`, the
  checkpoint step and the reviews all happen there, one at a time. So a job's thread never
  calls into `term.c`, `tools.c`, `ollama.c` or `main.c`, reads no global and no environment
  (`easy_setup()` does that on the main thread before the thread starts), has all signals
  blocked, and is stopped through its `cancel` flag. The call is asked for unstreamed
  (`ollama_chat_request()`/`ollama_chat_reply()`, or `provider_turn_request()`/`provider_turn_reply()` when the working model is Anthropic) for the same reason: a streamed one would
  want to draw. A worker has its own message list — never `g_messages`, and its
  prompt tokens never reach `ctx_measured()` — and the run enters the conversation as two
  messages, the request and one reply that says what came of each task, so a turn still ends
  with an assistant message. The orchestrator sees plans, reports and diffs, never a worker's
  transcript: that is what keeps the expensive model's tokens few, so do not hand it one.
  Workers write through `confirm()`; plan mode shows the plan and runs nothing.
- API keys live in the environment only. Never write one into the config, a session, the
  history, a log or an error message; `provider.c` builds the auth headers for the one request
  and frees them, and the fake server never logs them either. A key only travels over TLS or to
  this machine: `base_insecure()` refuses a plain-http (or scheme-less) base URL for any other
  host — `url_cleartext()`/`url_is_local()` in `util.c`, which the banner's warning about an
  unencrypted Ollama host uses too. No code writes an `http` scheme into a URL: Ollama's plain
  HTTP is libcurl's default for a host without one, and a literal "http" in a URL that is being
  built is what code scanning flags (cpp/non-https-url).
- Slash commands: add to `SLASH_CMDS` (with its one line for the suggestion list; fixed option
  words go into `SLASH_ARGS`), `handle_slash()`, `cmd_help()` and the README table.
  Unknown `/name` falls through to skills, so keep built-in names distinct from likely skill names.
- The fake Ollama answers memory-extraction calls with `NO_CHANGE` unless a user message contains
  `REMEMBER_ME`; the test config sets `memory=0` (and `memory_idle=0`, so a pending extraction never
  fires from a slow `expect()`) — only the dedicated memory test pays for the extra call (it sets
  `/memory every 1` so the call is observable after each request). Extraction is a different
  prompt and evicts Ollama's prompt cache for the conversation — keep it batched; the idle flush
  only spends that cache when the user is not using it anyway.
- Anything persisted goes into `~/.config/corbienest/config` via `config_save()`; keep old keys
  readable (e.g. `yolo=1` still maps to `mode=auto`).
- Several sessions may share these files, so every file corbienest keeps its *own* state in is
  written with `write_whole_file_atomic()` (temp file + `rename(2)`): the config, `memory.md`,
  `permissions`, the session JSON. Never `fopen(..., "w")` one of them — a reader in another
  session would see it truncated, and so would you after a crash. Files the *user* asked for
  (`write_file`, `edit_file`, `/save`, skill scaffolding) keep plain `write_whole_file()`: they
  are theirs, and replacing the inode is not ours to do. The history file is the exception to
  the whole-file rule — it is append-only (`hist_save()` writes just the entries added since
  the last save, `O_APPEND`), because rewriting it from one session's window would drop what the
  others typed; `hist_compact()` folds it back to `HIST_MAX` once it passes `HIST_FILE_MAX`.
- Tests: extend `tests/test_unit.c` for pure logic and `tests/test_integration.py` (pty
  `Session`) for anything the user sees. Integration checks read `clean()`ed output — the
  editor redraws in place, so assert on stable phrases, not layout. A repaint puts old
  transcript text into the byte stream again: `expect()` skips it (`fresh()`), `since()` does
  not — so never assert that a phrase of the transcript is *absent* from `since()` after
  something that changes the layout; look at the request, or at `listed()` for the suggestions.
  The tmux test needs `tmux` and skips itself without it.

## Skills policy

Skills live in `.corbienest/skills/NAME/SKILL.md` (frontmatter `name`/`description`, body with
`$ARGUMENTS`). When you create a skill or a helper program for one:

1. Write helpers in **C** (compile with `cc` / a Makefile in the skill directory).
2. Use **Rust** only if the helper genuinely needs third-party libraries.
3. **Python** is a last resort only.

The same rule is given to the model in corbienest's system prompt (`skills_prompt_section()`).

## Don'ts

- Don't add dependencies (libcurl is the one there is; notcurses only as the optional build above) or a build system beyond the Makefile.
- Don't print raw model/tool output without going through the existing preview helpers.
- Don't block on stdin outside `term.c`.
- Don't reflow or reformat unrelated code in a change; diffs should stay reviewable.
