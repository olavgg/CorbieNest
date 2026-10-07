#!/usr/bin/env python3
"""Play a corbienest trace back into this terminal, or say what is in it.

A drawing problem that shows in one terminal and not in another has to be seen
to be fixed. corbienest records what it draws and what it is sent when asked to:

    CORBIENEST_TRACE=/tmp/corbie.trace corbienest    # reproduce the problem, then quit

and this plays the recording back, in any terminal resized to the size it was
made in (the first line of the trace says which):

    tools/trace_replay.py /tmp/corbie.trace               # as it happened
    tools/trace_replay.py /tmp/corbie.trace --fast        # without the waiting
    tools/trace_replay.py /tmp/corbie.trace --until 42.5  # stop at 42.5 s and hold the picture
    tools/trace_replay.py /tmp/corbie.trace --from 40 --step   # from 40 s on, one write per Enter
    tools/trace_replay.py /tmp/corbie.trace --info        # sizes, layout changes, keys and mouse
    tools/trace_replay.py /tmp/corbie.trace --excerpt 40 45   # those five seconds, to paste into a report

The trace is text, one record per line: "<ms> <kind> <payload>", the kinds being
O (written to the terminal), H (output held back while scrolled back), I (read
from the terminal), L (layout), V (scrollback viewer), B (activity label). See
"trace" in src/term.c. It holds the conversation and everything typed.
"""
import argparse
import os
import re
import sys
import time

ESC = re.compile(rb"\\(e|n|r|t|\\|x[0-9a-fA-F]{2})")
PLAIN = {b"e": b"\x1b", b"n": b"\n", b"r": b"\r", b"t": b"\t", b"\\": b"\\"}
MOUSE = re.compile(rb"\x1b\[<(\d+);(\d+);(\d+)([Mm])")
# what a replay may leave switched on when it stops half way: the alternate screen, the scroll
# region, the mouse, bracketed paste, a hidden cursor, colours
RESTORE = b"\x1b[0m\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[r\x1b[?25h\x1b[?1049l"


def unescape(payload):
    return ESC.sub(lambda m: PLAIN.get(m.group(1)) or bytes([int(m.group(1)[1:], 16)]), payload)


def records(path):
    """(seconds, kind, payload) for every line of the trace; O/H/I payloads as the bytes they were."""
    with open(path, "rb") as f:
        for line in f:
            parts = line.rstrip(b"\n").split(b" ", 2)
            if len(parts) < 2:
                continue
            try:
                t = float(parts[0]) / 1000.0
            except ValueError:
                continue
            kind, payload = parts[1].decode("ascii", "replace"), parts[2] if len(parts) > 2 else b""
            yield t, kind, unescape(payload) if kind in "OHI" else payload


def describe_input(data):
    """A wheel notch or a click, in words, next to the bytes the terminal sent for it."""
    notes = []
    for m in MOUSE.finditer(data):
        b, col, row, press = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4) == b"M"
        if b & 64:
            what = "wheel " + ("down" if b & 1 else "up")
        else:
            what = ("press" if press else "release") + " button %d" % (b & 3)
        notes.append("%s at row %d, column %d" % (what, row, col))
    return "   (" + "; ".join(notes) + ")" if notes else ""


def show(data):
    return repr(data)[2:-1]


def info(path):
    """The trace without what was drawn: short enough to read, and to paste."""
    written = held = 0
    last = 0.0
    typed, typed_at, typed_end = b"", 0.0, 0.0   # input arrives a byte at a time: one line per burst of it

    def flush():
        nonlocal typed
        if typed:
            print("%9.3f  in      %s%s" % (typed_at, show(typed), describe_input(typed)))
        typed = b""

    for t, kind, payload in records(path):
        last = t
        if kind == "I" and typed and t - typed_end < 0.02:
            typed, typed_end = typed + payload, t
            continue
        if kind in "OH":
            written, held = written + (len(payload) if kind == "O" else 0), held + (len(payload) if kind == "H" else 0)
            continue
        flush()
        if kind == "I":
            typed, typed_at, typed_end = payload, t, t
        else:
            name = {"#": "trace", "L": "layout", "V": "viewer", "B": "busy"}.get(kind, kind)
            print("%9.3f  %-7s %s" % (t, name, payload.decode("utf-8", "replace")))
    flush()
    print("%9.3f  end     %d bytes written to the terminal, %d held back while scrolled back" % (last, written, held))


def excerpt(path, start, end):
    """The records between two moments as they are in the file, after the state they start from:
    the header and the last layout, viewer and activity records before them."""
    state = {}
    with open(path, "rb") as f:
        for line in f:
            parts = line.split(b" ", 2)
            try:
                t = float(parts[0]) / 1000.0
            except ValueError:
                continue
            kind = parts[1] if len(parts) > 1 else b""
            if t < start:
                if kind in (b"#", b"L", b"V", b"B"):
                    state[kind] = line
            elif t <= end:
                for k in (b"#", b"L", b"V", b"B"):
                    if k in state:
                        sys.stdout.buffer.write(state.pop(k))
                sys.stdout.buffer.write(line)


def recorded_size(path):
    for _, kind, payload in records(path):
        m = re.search(rb"(\d+) rows x (\d+) columns", payload) if kind == "#" else None
        return (int(m.group(1)), int(m.group(2))) if m else None
    return None


def main():
    ap = argparse.ArgumentParser(description="Play a corbienest trace (CORBIENEST_TRACE=FILE) back into this terminal.")
    ap.add_argument("trace")
    ap.add_argument("--info", action="store_true", help="list sizes, layout changes and input instead of replaying")
    ap.add_argument("--excerpt", nargs=2, type=float, metavar=("FROM", "TO"), help="print the records between two moments (seconds), to paste")
    ap.add_argument("--fast", action="store_true", help="do not wait between writes")
    ap.add_argument("--speed", type=float, default=1.0, help="play N times as fast (default 1)")
    ap.add_argument("--from", dest="start", type=float, default=0.0, metavar="SEC", help="write everything before SEC at once")
    ap.add_argument("--until", type=float, default=None, metavar="SEC", help="stop at SEC and hold the picture")
    ap.add_argument("--step", action="store_true", help="after --from, one write per Enter (its time and size on stderr)")
    ap.add_argument("--no-hold", action="store_true", help="do not wait for Enter at the end (for scripts)")
    a = ap.parse_args()
    if a.info:
        return info(a.trace)
    if a.excerpt:
        return excerpt(a.trace, a.excerpt[0], a.excerpt[1])

    out = sys.stdout.buffer
    tty = sys.stdout.isatty()
    want = recorded_size(a.trace)
    if tty and want:
        have = os.get_terminal_size()
        if (have.lines, have.columns) != want:
            sys.stderr.write("the trace was made in %d rows x %d columns, this terminal has %d x %d: it will not look the same\n"
                             % (want[0], want[1], have.lines, have.columns))
            time.sleep(2)
    keys = open("/dev/tty", "rb", buffering=0) if tty and (a.step or not a.no_hold) else None
    prev, stopped = None, False
    try:
        for t, kind, payload in records(a.trace):
            if kind != "O":
                continue
            if a.until is not None and t > a.until:
                stopped = True
                break
            if t >= a.start:
                if a.step and keys:
                    sys.stderr.write("\x1b7\x1b[1;1H\x1b[7m %.3f s · %d bytes · Enter \x1b[0m\x1b8" % (t, len(payload)))
                    sys.stderr.flush()
                    keys.readline()
                elif not a.fast and prev is not None and t > prev:
                    time.sleep(min((t - prev) / max(a.speed, 0.001), 5.0))
                prev = t
            out.write(payload)
            out.flush()
    except KeyboardInterrupt:
        stopped = True
    if keys and not a.no_hold:
        keys.readline()   # the picture stays until Enter
    if tty:
        out.write(RESTORE)
        out.flush()
        if stopped:
            print("(replay stopped; the terminal is back to normal)")


if __name__ == "__main__":
    main()
