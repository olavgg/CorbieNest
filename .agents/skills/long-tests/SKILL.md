---
name: long-tests
description: Run long test suites carefully: a cheap gate first, one detached run, sub-agents only for separate investigations, and no re-runs that waste time or the prompt cache
---
Run the test suite $ARGUMENTS (default: `make test`, the unit suite; for `integration` or a section
title, the steps below apply to `tests/test_integration.py`). The integration suite takes about 40
minutes. It points the hosted providers at the fake server with a test key, so it normally costs no
money; but a section that does not set its own `*_BASE_URL` could reach a real API. Check a new
section for that before running it, and ask the user first if you are not sure.

The point of this skill is to spend the least time and tokens that still give a true answer.
Each step below exists for one of those.

1. Gate first, cheaply. Run the unit suite (`make test`) yourself, in the foreground. It takes seconds.
   Fix its failures before a long run: a long run that starts on a broken build only wastes an hour.

2. Build once, then leave the binary alone. `make` (no `make clean` between runs; a clean rebuilds
   everything and the tests read the binary while they run). Do not edit files under `src/` or
   `tests/` while a long run is going: the run reads them as it goes, and a half-edited file makes
   failures that mean nothing.

3. One detached run for the whole suite, never several. Start it with a timeout and unbuffered
   output, and keep its pid:
   ```sh
   nohup timeout 3000 python3 -u tests/test_integration.py > /tmp/integ.log 2>&1 &
   echo $! > /tmp/integ.pid
   ```
   Poll the log with `tail -1` and `sleep` in steps of at most ten minutes. The last line is `rc=`
   when it is done. A failure that looks like a timing flake is rerun once, on its own, not the whole
   suite again. A single section runs by its exact title:
   `python3 -u tests/run_section.py "=test interactive: /cost"`.

4. Sub-agents: only for investigations that are separate from each other, and never for running the
   suite. Each sub-agent starts with an empty context, so it pays again for everything it reads, and
   two of them running the suite at once fight over ports, pty and temporary files. Give one a narrow
   task with everything it needs in the prompt: the failing check's exact message, the log path, the
   file to read, and what to report (at most ten lines: the cause, the file and line, and whether the
   failure is in this change or was there before). Keep the prompt short: a long brief is paid for on
   every call it makes. Use the read-only sub-agent, so it cannot change the tree while the run reads it.

5. Compare with a baseline once, not per failure. When failures need a baseline, make one worktree at
   the commit before the change (`git worktree add /tmp/base <commit>`), copy the same test file into
   it, and run the suite there once in the background. Compare the failures by their message text up to
   the first colon: line numbers and temporary directory names change from run to run. Remove the
   worktree when done (`git worktree remove --force`).

6. Keep the prompt cache warm while a suite is running. Corbie Nest caches the system prompt and the
   tool list; changing either between requests makes the next request pay for them again. So during a
   run do not change the model, `/think`, `/effort`, `/ctx`, the tools, or the mode from a session that
   is driving tests, and do not add or remove skills, agents, MCP servers or hooks in the middle of it.

7. Ports and processes. Fake servers use a port of their own per run (20xxx, not 11434, which may be a
   real Ollama). Stop only what you started: the pid in your pid file, never `pkill` by name, and not
   `pgrep -f` alone (it matches the shell that runs it).

8. Report the real numbers. Take them from the log: `N checks passed, M failed`, and the failing
   messages grouped by cause. Say which failures were there before the change (with the baseline's
   numbers) and which are new. Do not say a suite is green while any check fails; say how many fail.
