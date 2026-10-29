#!/usr/bin/env python3
"""Start corbie --ui notcurses on a pty that plays the terminal, and check its start-up.

notcurses sends ~260 queries when it starts (the 256 palette entries, OSC 4, then the
device attributes, DA1) and reads the replies from the same stream as the keys. A
terminal may write a reply in pieces (iTerm2 does with its palette replies), and a
reply cut between two reads used to come out as typed text, "]4;194;rgb:d7d7/ffff/d7d7"
in the input field, and left notcurses' parser in a state that misread every escape
after it (a click on the banner's [×] did nothing; with asserts on, it aborted; during
the init, it waited for ever). This plays such a terminal: it answers the queries, and
can cut the replies in two with a pause between the halves.

    tools/nc_pty_check.py                          # whole replies
    SPLIT=12 GAP=0.002 tools/nc_pty_check.py       # every reply cut after 12 bytes, 2 ms apart
    ROUND=2 ONLY=191 SPLIT=1 GAP=0.01 DA1FIRST=1 tools/nc_pty_check.py
                                                   # one reply cut right after its ESC (iTerm2 on macOS: a
                                                   # 1 KB read cuts the replies anywhere)
    ROUND=2 ONLY=194 SPLIT=12 GAP=0.01 DA1FIRST=1 tools/nc_pty_check.py
                                                   # only palette entry 194 of the second init (the
                                                   # first is nc_probe's child) cut, DA1 answered first
    CLICK=10,100 tools/nc_pty_check.py             # click row 10, column 100 (1-based) after 4 s
    CLICK=esc tools/nc_pty_check.py                # press Esc after 4 s

Settings (environment): SPLIT (0: whole replies), GAP (seconds between the halves),
ROUND (cut only in that init, 1 = the probe's child), ONLY (cut only that palette
entry), DA1FIRST (answer DA1 before the palette replies still queued), CLICK, TRACE
(a CORBIENEST_TRACE file for the run), SECS
(how long to run, default 6), ROWS/COLS (40x120). The first argument is the binary
(default ./corbie); the output is kept in $OUT (default nc_pty_check.out in the temporary
folder that is HOME for the run), and the path is printed.

Exit status: 1 if a reply's text reached the screen or notcurses did not start (the
ANSI fallback, or no notcurses banner), or (with pyte installed, which plays the
output into a screen) the banner closed with no CLICK; 2 if
CLICK was given and the banner is still up at the end; 0 otherwise. It runs with HOME in a fresh temporary folder and no model.
Timing-dependent: a manual check, not part of make test. Cutting all 256 palette
replies of the probe's init costs 256 x GAP and more: past nc_probe's 2 s (GAP=0.01,
say) the ANSI fallback is the right outcome, so keep GAP at 0.002 for those runs.
"""
import fcntl
import os
import pty
import re
import select
import struct
import sys
import tempfile
import termios
import threading
import time

SPLIT = int(os.environ.get("SPLIT", "0"))
GAP = float(os.environ.get("GAP", "0.003"))
ROUND = int(os.environ.get("ROUND", "0"))
ONLY = os.environ.get("ONLY")
DA1FIRST = bool(os.environ.get("DA1FIRST"))
CLICK = os.environ.get("CLICK")
SECS = float(os.environ.get("SECS", "6"))
ROWS, COLS = int(os.environ.get("ROWS", "40")), int(os.environ.get("COLS", "120"))
binary = sys.argv[1] if len(sys.argv) > 1 else "./corbie"
binary = os.path.abspath(binary)

home = tempfile.mkdtemp(prefix="ncpty")
env = dict(os.environ, HOME=home, TERM="xterm-256color", OLLAMA_HOST="127.0.0.1:9",
           XDG_CONFIG_HOME=os.path.join(home, ".config"))
for k in ("ANTHROPIC_API_KEY", "XAI_API_KEY", "OPENAI_API_KEY", "DEEPSEEK_API_KEY",
          "MOONSHOT_API_KEY", "TMUX", "CORBIENEST_TRACE"):
    env.pop(k, None)
if os.environ.get("TRACE"):
    env["CORBIENEST_TRACE"] = os.path.abspath(os.environ["TRACE"])   # the run's trace (N records: what notcurses decoded)

pid, fd = pty.fork()
if pid == 0:
    os.chdir(home)
    os.execve(binary, [binary, "--ui", "notcurses"], env)
fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))

REPLY_TEXT = b"rgb:d7d7/ffff/d7d7"
QUERY = re.compile(rb"\x1b\]4;\d+;\?|\x1b\]1[01];\?|\x1b\[6n|\x1b\[c|\x1b\[>0q")


def reply_for(q):
    m = re.match(rb"\x1b\]4;(\d+);\?", q)
    if m:
        return b"\x1b]4;" + m.group(1) + b";" + REPLY_TEXT + b"\x1b\\"
    if q.startswith(b"\x1b]10;"):
        return b"\x1b]10;rgb:dddd/dddd/dddd\x1b\\"
    if q.startswith(b"\x1b]11;"):
        return b"\x1b]11;rgb:1111/1111/1111\x1b\\"
    return {b"\x1b[6n": b"\x1b[1;1R", b"\x1b[c": b"\x1b[?62;4c",
            b"\x1b[>0q": b"\x1bP>|iTerm2 3.5.0\x1b\\"}.get(q, b"")


lock = threading.Lock()
queue = []      # (init round, reply)
rounds = [0]


def write_all(data):
    while data:
        try:
            data = data[os.write(fd, data):]
        except BlockingIOError:
            time.sleep(0.0005)


def writer():
    """the terminal's replies, in order; with SPLIT, a cut one is written in two halves GAP apart"""
    try:
        while True:
            with lock:
                item = queue.pop(0) if queue else None
            if item is None:
                time.sleep(0.0005)
                continue
            rnd, data = item
            cut = (SPLIT and len(data) > SPLIT and (not ROUND or rnd == ROUND)
                   and (not ONLY or data.startswith(b"\x1b]4;" + ONLY.encode() + b";")))
            if cut:
                write_all(data[:SPLIT])
                time.sleep(GAP)
                write_all(data[SPLIT:])
            else:
                write_all(data)
    except OSError:
        return


threading.Thread(target=writer, daemon=True).start()

out = bytearray()
scan = 0
t0 = time.time()
clicked = False
while time.time() - t0 < SECS:
    r, _, _ = select.select([fd], [], [], 0.01)
    if r:
        try:
            b = os.read(fd, 65536)
        except BlockingIOError:
            continue
        except OSError:
            break
        if not b:
            break
        out += b
        last = None
        for m in QUERY.finditer(out, scan):
            last = m
            q = m.group(0)
            if q == b"\x1b]4;0;?":
                rounds[0] += 1
            with lock:
                if DA1FIRST and q == b"\x1b[c" and (not ROUND or rounds[0] == ROUND):
                    queue.insert(0, (rounds[0], reply_for(q)))
                else:
                    queue.append((rounds[0], reply_for(q)))
        scan = max(last.end() if last else scan, len(out) - 16)   # a query cut by this read is found next time
    if CLICK and not clicked and time.time() - t0 > 4:
        if CLICK == "esc":
            write_all(b"\x1b")
        else:
            y, x = map(int, CLICK.split(","))
            write_all(b"\x1b[<0;%d;%dM" % (x, y))
            time.sleep(0.05)
            write_all(b"\x1b[<0;%d;%dm" % (x, y))
        clicked = True
try:
    os.kill(pid, 9)
except ProcessLookupError:
    pass

data = bytes(out)
out_path = os.environ.get("OUT") or os.path.join(home, "nc_pty_check.out")
with open(out_path, "wb") as f:
    f.write(data)
print("output: %s" % out_path)
# what was drawn as text: escape sequences out (the queries notcurses sent are among them)
text = re.sub(rb"\x1b\[[0-9;?<>=]*[ -/]*[@-~]", b"", data)
text = re.sub(rb"\x1b\][^\x1b\x07]*(\x1b\\|\x07)", b"", text)
text = re.sub(rb"\x1bP[^\x1b]*\x1b\\", b"", text)
leaked = b"rgb:" in text or b"]4;" in text
fallback = b"ANSI UI" in data
started = re.search(rb"ui:\s+notcurses", text) is not None   # the banner's "ui: notcurses" line
aborted = b"Assertion failed" in data
print("init rounds %d, %d bytes out" % (rounds[0], len(data)))
print("reply text on screen: %s · ANSI fallback: %s · notcurses banner: %s · assertion: %s"
      % (leaked, fallback, started, aborted))
status = 1 if (leaked or fallback or not started or aborted) else 0
# the screen itself (pyte: pip install pyte) tells two more things: a key made of a reply (its Esc closes the banner)
# and what a CLICK did
try:
    import pyte
except ImportError:
    pyte = None
    print("the screen checks need pyte (pip install pyte); not checked")
if pyte and status == 0:
    screen = pyte.Screen(COLS, ROWS)
    pyte.ByteStream(screen).feed(data)
    up = any("[×]" in row for row in screen.display)
    shown = "\n".join(screen.display)
    on_screen = "rgb:" in shown or "]4;" in shown      # notcurses draws cell by cell: the bytes alone can miss a reply's text
    print("banner up at the end: %s · reply text on the screen: %s" % (up, on_screen))
    if on_screen:
        status = 1
    if CLICK:
        if up:
            status = 2
    elif not up:
        status = 1        # nothing was pressed, so something read as a key closed it
sys.exit(status)
