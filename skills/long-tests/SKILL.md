---
name: long-tests
description: Run long test suites carefully: a cheap gate first, one detached run, sub-agents only for separate investigations, and no re-runs that waste time or the prompt cache
---
Run the test suite $ARGUMENTS. With no argument, run the project's quick suite (the unit tests); with
`integration`, or a section's title, the long suite or that section. Find the commands in the project's
own documentation (AGENTS.md, README, the Makefile or the package scripts): they say which suite is the
cheap gate, which is the long one, and whether the long one reaches real services. If you cannot tell,
ask the user before starting it.

The point of this skill is to spend the least time and tokens that still give a true answer.
Each step below exists for one of those.

1. Gate first, cheaply. Run the quick suite yourself, in the foreground. It should take seconds.
   Fix its failures before a long run: a long run that starts on a broken build only wastes time.

2. Build once, then leave the binary alone. Build with the project's own command, without a clean
   between runs (a clean rebuilds everything, and a test may read the binary while it runs). Do not
   edit the source or the tests while a long run is going: the run reads them as it goes, and a
   half-edited file makes failures that mean nothing.

3. One detached run for the whole suite, never several. Start it with a timeout where the system has
   one (macOS has no `timeout`: leave the timeout out there), unbuffered output, and keep its pid:
   ```sh
   nohup <the long suite's command> > /tmp/long.log 2>&1 < /dev/null &
   echo $! > /tmp/long.pid
   ```
   (`<the long suite's command>` is a placeholder: use the command the project documents.) Poll the log with
   `tail -1` and `sleep` in steps well under the tool's time limit. A failure that looks like a timing
   flake is rerun once, on its own, not the whole suite again. A single section runs by its exact title,
   when the suite has sections.

4. Sub-agents: only for investigations that are separate from each other, and never for running the
   suite. Each sub-agent starts with an empty context, so it pays again for everything it reads, and
   two of them running the suite at once fight over ports, terminals and temporary files. Give one a
   narrow task with everything it needs in the prompt: the failing check's exact message, the log path,
   the file to read, and what to report (at most ten lines: the cause, the file and line, and whether
   the failure is in this change or was there before). Keep the prompt short. Use a read-only sub-agent,
   so it cannot change the tree while the run reads it.

5. Compare with a baseline once, not per failure. When failures need a baseline, make one worktree at
   the commit before the change (`git worktree add /tmp/base <commit>`), copy the same test files into
   it, and run the suite there once in the background. Compare the failures by their message text up to
   the first colon: line numbers and temporary directory names change from run to run. Remove the
   worktree when done (`git worktree remove --force`).

6. Keep the prompt cache warm while a suite is running. The system prompt and the tool list are cached;
   changing either between requests makes the next request pay for them again. So during a run do not
   change the model or its effort, the tools, or the mode from a session that is driving the tests, and
   do not add or remove skills, agents, servers or hooks in the middle of it.

7. Processes. Stop only what you started: the pid in your pid file. Never `pkill` by name, and not
   `pgrep -f` alone (it matches the shell that runs it).

8. Report the real numbers. Take them from the log: how many checks passed and how many failed, and the
   failing messages grouped by cause. Say which failures were there before the change (with the
   baseline's numbers) and which are new. Do not say a suite is green while any check fails; say how
   many fail.
