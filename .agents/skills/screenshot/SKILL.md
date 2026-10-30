---
name: screenshot
description: Regenerate the README screenshot (docs/demo.svg) from a real frame of the notcurses screen
---
Regenerate the README picture, `docs/demo.svg`, from the current build: the notcurses screen at
startup, with the banner. The frame is drawn by corbienest itself in a pseudo-terminal, read back by a
small screen emulator, written as a transcript (`docs/demo.ansi`), and converted by `tools/ansi2svg.py`.
Nothing is typed by hand except the one host value described in step 6.

Argument (optional): a different model name for the status line. The default is `qwen3-coder:30b`.

Do the steps in order. Use the bash tool; run commands from the repository root.

1. Build corbienest with notcurses and the helpers.
   `make` (must not warn), then `make -C .agents/skills/screenshot`.
   `make` builds the notcurses screen when libnotcurses is installed; the picture needs it.

2. Make the demo project the status line shows: a git repository on `main`, so the location line
   reads a folder and a branch, and no personal path appears in the picture.
   ```sh
   rm -rf /tmp/demo && mkdir -p /tmp/demo/projects/myapp && cd /tmp/demo/projects/myapp \
     && git init -q -b main && echo "# myapp" > README.md && git add README.md \
     && git -c user.email=demo@example.com -c user.name=demo commit -qm init && cd -
   ```

3. Start a fake Ollama server on a free port. Use a port that is not 11434: a real Ollama may be
   listening there, and it must be left alone. Pick 20xxx, for example 20601, and keep its pid.
   ```sh
   python3 tests/fake_ollama.py 20601 >/tmp/shot-fake.log 2>&1 & echo $! > /tmp/shot-fake.pid
   sleep 1
   ```

4. Capture the screen for five seconds (the banner is drawn at startup; seven seconds if the title
   is not in the frame after step 5).
   ```sh
   .agents/skills/screenshot/capture 20601 /tmp/shot-screen.bin 100 34 qwen3-coder:30b 5 \
     /tmp/demo/projects/myapp "$PWD/corbie"
   ```
   Then stop the fake server, and only that one: `kill "$(cat /tmp/shot-fake.pid)"`.

5. Draw the frame into a transcript, and check the picture's text before converting it.
   ```sh
   .agents/skills/screenshot/screen /tmp/shot-screen.bin 100 34 docs/demo.ansi
   sed 's/\x1b\[[0-9;]*m//g' docs/demo.ansi | sed -n 5,26p
   ```
   It must show the box's top edge, the three block-letter title rows (`▄▀▀ ▄▀▀▄`), the version
   line, a blank row under the crow, then the model, host, cwd and ui lines. If the title is missing,
   the frame was taken too early: repeat steps 3 to 5 with a longer wait.

6. Put the default host address in place of the fake server's port, the same width, so the box
   still lines up. The port is the one in step 3 (five digits).
   ```sh
   sed -i 's/127\.0\.0\.1:20601/127.0.0.1:11434/' docs/demo.ansi
   grep -c 20601 docs/demo.ansi    # expect 0
   ```

7. Convert the transcript to the picture, and check that the host line is the default one.
   ```sh
   python3 tools/ansi2svg.py docs/demo.ansi docs/demo.svg
   sed 's/\x1b\[[0-9;]*m//g' docs/demo.ansi | grep "host:"
   ```

8. Commit the two files by path, and nothing else; push only when the user asks for it.
   ```sh
   git add docs/demo.ansi docs/demo.svg
   git commit -m "docs: screenshot of the notcurses screen (<what changed>)"
   ```
   If the picture did not change (`git diff --stat docs/` is empty), say so and commit nothing.

Report what the frame showed (step 5), which host address the picture carries, and that the picture was
checked as text: the image itself has to be looked at by a person, in a browser or on GitHub.

Notes:
- The helpers are C, in this directory: `capture` runs the program in a pty and answers the terminal
  queries notcurses makes at startup; `screen` is the emulator (cursor moves, erase, colours) and writes
  the transcript. The Makefile builds both with `-Wall -Wextra`.
- tmux does not work for this: notcurses stalls in it. The pty capture is the way.
- Kill only the processes you started (the fake server pid from step 3, the `capture` child is killed by
  `capture` itself). Never kill by name: other corbienest sessions may be running.
- The temporary directory `/tmp/corbie-shot-*` that `capture` makes for the config is safe to delete.
