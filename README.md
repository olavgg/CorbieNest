# Corbie Nest

A Claude-Code-style terminal coding agent written in plain C. It runs **local models** through
Ollama, and **hosted models** from Anthropic, xAI, OpenAI, DeepSeek and Kimi. It streams responses,
reads, writes and edits files, searches the codebase, and runs shell and git commands. Every mutating action is shown to you and
confirmed before it runs.

![corbienest on the notcurses screen: the banner with the block-letter title, the input field pinned to the bottom, and the status bar with the model, the tokens, and the folder and git branch.](docs/demo.svg)

## Features

- **Local or hosted models.** Ollama models, or Anthropic's Claude as the model that does the work
  (`--model anthropic:claude-haiku-5-5`). A stronger model can be consulted as an *advisor*
  (`--advisor`), locally, on Ollama's cloud, or through xAI, OpenAI or Anthropic.
- **Tools with confirmation.** Read, write and edit files, grep, run shell and git commands,
  and look documentation up on the web. Four permission modes, from `manual` to `auto`.
- **Sub-agents.** A read-only agent investigates a question in its own context and reports
  back. A strip above the status bar lists the running ones, and `↓` opens their logs.
- **Orchestration and goals.** `/orchestrate` plans a request as tasks that the advisor reviews;
  `/goal` races candidates in git worktrees against a metric.
- **Sessions, memory, skills.** Sessions resume with `--continue` or `/resume`. The project keeps
  a memory file, and skills are reusable `SKILL.md` instructions.
- **Full-screen terminal UI.** A rounded input field, a status bar with the model, tokens, context
  and git branch, and a conversation with folded tool output. It also has an optional notcurses
  screen.

The complete reference is in [DOCUMENTATION.md](DOCUMENTATION.md).

## Build from source

Dependencies: a C11 compiler, `make`, [cJSON](https://github.com/DaveGamble/cJSON) and
[libcurl](https://curl.se/libcurl/) 7.66 or newer. That is all. The optional
[notcurses](https://github.com/dankamongmen/notcurses) screen is compiled in when pkg-config
finds `notcurses`.

```sh
# Debian 13 (trixie) / Ubuntu 26.04 (resolute)
sudo apt install build-essential libcjson-dev libcurl4-openssl-dev

# Fedora 44
sudo dnf install gcc make cjson-devel libcurl-devel

# RHEL 10 / AlmaLinux 10 / Rocky Linux 10 (cjson comes from EPEL)
sudo dnf install epel-release
sudo dnf install gcc make cjson-devel libcurl-devel

# macOS
brew install cjson                     # libcurl comes with macOS
```

```sh
make                  # builds ./corbienest
make release          # optimized, stripped binary
sudo make install     # optional: installs to /usr/local/bin
make test             # the unit tests: no model or network needed
```

Build options:
- `make NOTCURSES=0` leaves the notcurses screen out even when it is installed.
- `make NOTCURSES=1` requires it and fails without it.
- On macOS with Homebrew: `make CFLAGS="-O2 -std=gnu11 -I$(brew --prefix)/include" LDLIBS="-L$(brew --prefix)/lib -lcjson -lcurl"`.

## Quick start

1. Install [Ollama](https://ollama.com), start it and pull a model with tool support:

   ```sh
   ollama serve
   ollama pull qwen3-coder:30b
   ```

2. Run corbienest in a project directory:

   ```sh
   cd your-project
   ./corbienest                       # or: ./corbienest --model qwen3-coder:30b
   ```

3. Type a request and press Enter. Approve each file change or command when asked.
   Type `/help` for the commands, `/mode` to change the permission mode, and `/quit` (or Ctrl-D)
   to leave.

To use Claude instead, export `ANTHROPIC_API_KEY` and start with
`./corbienest --model anthropic:claude-haiku-5-5`.

## Documentation

- [Usage and options](DOCUMENTATION.md#usage)
- [Slash commands](DOCUMENTATION.md#slash-commands) and [input tricks](DOCUMENTATION.md#input-tricks)
- [Permission modes and tools](DOCUMENTATION.md#permission-modes)
- [The advisor](DOCUMENTATION.md#the-advisor), [sub-agents](DOCUMENTATION.md#sub-agents),
  [orchestrating](DOCUMENTATION.md#orchestrating) and [goals](DOCUMENTATION.md#goals)
- [Skills](DOCUMENTATION.md#skills), [sessions](DOCUMENTATION.md#sessions) and
  [project memory](DOCUMENTATION.md#project-memory)
- [Configuration](DOCUMENTATION.md#config), [tests](DOCUMENTATION.md#tests) and
  [layout](DOCUMENTATION.md#layout)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the build, test and style rules, and
[AGENTS.md](AGENTS.md) for how the code is laid out. In short: no new dependencies,
`make test` must pass, and the build must be warning-free.

## License

Apache License 2.0. See [LICENSE](LICENSE).

Copyright 2026 The Corbie Nest authors, Olav Gjerde.
