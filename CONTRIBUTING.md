# Contributing to Corbie Nest

Thanks for taking a look. Corbie Nest is a small C program on purpose — patches that keep it
small are the most welcome ones.

## Build and test

```sh
sudo apt install libcjson-dev     # or: brew install cjson
make                              # builds ./corbienest
make test                         # C unit tests + pty integration tests
```

`make test` needs no GPU, no model and no network: `tests/fake_ollama.py` is a scripted
stand-in for Ollama. Both suites must pass and the build must be warning-free:

```sh
make clean && make CFLAGS="-O2 -g -Wall -Wextra -Werror -std=gnu11" && make test
```

That is exactly what CI runs.

## Ground rules

- **No new dependencies.** C11, libc, POSIX and cJSON. No libcurl, no ncurses, no readline.
  HTTP is raw sockets (`src/http.c`), the UI is ANSI escapes (`src/term.c`).
- **Read [AGENTS.md](AGENTS.md) first.** It is the working guide for this codebase: what each
  file owns, and the conventions that are easy to break (width-aware drawing, never reading
  stdin outside `src/term.c`, everything that waits must poll `term_poll_interrupt()`,
  scrollback bookkeeping, where settings are persisted).
- **Tests come with the change.** Pure logic goes in `tests/test_unit.c`; anything the user can
  see or type goes in `tests/test_integration.py`, which drives the real binary through a pty
  against the fake Ollama.
- **A new slash command** touches `SLASH_CMDS`, `handle_slash()`, `cmd_help()` and the README
  table — all four.
- One feature per commit, with a subject line that says what the user gets.

## Pull requests

Open an issue first for anything large or for a change in direction; small fixes can go
straight to a PR. Say which model(s) you tried it with — behaviour varies a lot between them.

## Reporting bugs

Include your OS, `corbienest --version`, the model, and what the status bar / stats line showed.
If the terminal is left in a bad state, the exact terminal emulator matters too.

## License

By contributing you agree that your contributions are licensed under the Apache License 2.0,
the same terms as the rest of the project (see [LICENSE](LICENSE)).
