# Vendored notcurses core

This directory is a copy of the parts of [notcurses](https://github.com/dankamongmen/notcurses)
3.0.17 that corbienest's notcurses frontend (`src/term_nc.c`) runs on. The Makefile builds it
into the binary, so the screen works without a notcurses package installed, and with a
tmux 3.4+ that the packaged 3.0.17 hangs under.

Licence: Apache 2.0 (`LICENSE`, `COPYRIGHT`), as upstream. The files are unchanged from the
upstream tarball except where listed below.

## What is here

| path | from upstream | what it is |
|---|---|---|
| `src/lib/*.c`, `src/lib/*.h` | `src/lib/` | the notcurses core (rendering, input, planes, termdesc) |
| `src/compat/*` | `src/compat/` | portability shims (`compat.c`) |
| `src/media/shim.c`, `src/media/none.c` | `src/media/` | `notcurses_init()` and the no-multimedia visual implementation (the `USE_MULTIMEDIA=none` build) |
| `include/notcurses/*.h` | `include/notcurses/` | the public headers |
| `tools/version.h.in`, `tools/builddef.h.in` | `tools/` | CMake's templates; the Makefile generates `build/notcurses/version.h` and `builddef.h` from them with `sed` |

Not copied: the ffmpeg and oiio multimedia engines (`src/media/ffmpeg.c`, `oiio.*`), the C++
and FFI bindings, the demos, tests and tools, and the man pages.

## Changes from upstream

- `src/lib/in.c`: the DA1 and XTSMGRAPHICS patterns from PR #2926 (commit 9e43618, by Brian
  Duggan), against upstream issue #2736. tmux 3.4+ answers the primary device attributes query with
  `CSI ? 1 ; 2 ; 4 c`, which 3.0.17 cannot parse, so `notcurses_init()` waits for ever; the patch
  adds that reply and, as the PR describes, keeps the `CSI ? 1 ; 3 ; 1024 S` XTSMGRAPHICS reply that tmux also sends out of the input (not tested separately here).
  Upstream has not released it as of this copy.
- `src/lib/in.c`, `load_ncinput()`: an Enter keeps the byte it came from, CR or LF, as the key's
  effective text (`eff_text[0]`), which upstream does not keep. The paste reader in corbienest needs it: a
  pasted CRLF is one line break, a lone LF another. Upstream turns both into `NCKEY_ENTER` and forgets which
  it was. Four lines, `if(ni->eff_text[0] == 0){ ni->eff_text[0] = ni->id; }` before the id is set to
  `NCKEY_ENTER`. The key id stays `NCKEY_ENTER`; only `eff_text` of an Enter changes (CR or LF instead of the
  key's own value), and corbienest is the only reader of it. Reapply it when the copy is updated (look for
  `corbienest:` in that file).
- `src/lib/in.c`, `process_input()`: a byte that is not UTF-8 is U+FFFD, one for each invalid byte, not the code
  point of its value (upstream gives U+00FF for a lone 0xFF and drops a truncated sequence). The ANSI reader
  makes the same U+FFFD (`utf8_fix`), so a pasted bad byte shows the same on both screens. Three marked changes
  (`corbienest:`): the ASCII branch, the `mbrtowc` failure, and the log line. Reapply it with the Enter change.
- `src/lib/in.c`, `load_ncinput()`: a full input queue (`isize`, BUFSIZ events) makes the input thread wait for the
  reader instead of dropping the key. Upstream drops it with a warning, and a paste is ~150k keys: the reader is
  busy between drains, so a 3000-line paste lost four in five of its bytes (and with them the paste's end marker).
  The wait is a `pthread_cond_wait` on `icond` in a cleanup-pushed loop (so a cancel still works); a drain or EOF
  is not waited for, and the reader broadcasts `icond` when it frees a slot in `internal_get()` (marked
  `corbienest:` in that file). Reapply it with the others.
  Not during init (`initdata` set): `notcurses_init()` reads no keys until the input thread has handed the
  replies over, so a wait there never ended; during init a full queue drops the key, as upstream does.

Nothing else is changed. `make` compiles these files with the flags in the top-level Makefile
(`NC_VFLAGS`): they are not held to our `-Werror`, and the two warnings upstream's own code has
(`-Wclobbered`, `-Wdiscarded-qualifiers`) are left out for them alone.

## Build requirements

libunistring (headers and `-lunistring`: the core segments graphemes with it), zlib (the banner
and kitty graphics), and terminfo from ncurses. On Debian/Ubuntu:
`sudo apt install libunistring-dev zlib1g-dev libncurses-dev`. The Makefile builds the frontend
when all three are found (`NOTCURSES=auto`) and leaves it out otherwise.

## Updating

Copy the same paths from a newer upstream tarball, reapply the change above where upstream has
not taken it, and check the upstream file list in `CMakeLists.txt` (`NCCORESRCS`, `COMPATSRC`,
and the `src/media` files `USE_MULTIMEDIA=none` compiles). Then `make clean && make` and
`make test`, and the tmux start-up check: `./corbienest --ui notcurses` in tmux must show
`ui: notcurses` in the banner.
