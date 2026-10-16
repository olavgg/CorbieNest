"""Run one section of tests/test_integration.py and report only its checks.

    python3 tests/run_section.py "a pasted block is a token"   # any part of a section's title

The sections share state (a session started in one is used by the next), so the sections a
chosen one depends on run first, silently: their checks are not reported or counted. Only the
chosen section's checks are printed. This is a debugging aid, not a substitute for sections that
stand on their own. Run from the repository root; the binary must be built."""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "test_integration.py")

def sections(lines):
    starts = [i for i, l in enumerate(lines) if l.startswith('print("test ')]
    return starts, starts[1:] + [len(lines)]

def build(lines, starts, ends, target):
    """The file with every section's checks muted except the target's; the target is the last one that runs."""
    out, top_end = [], starts[0]
    out += lines[:top_end]
    out.append("_QUIET_CHECKS = True\n_real_check = check\ncheck = lambda *a, **k: True")
    for k, st in enumerate(starts):
        if k == target:
            out.append("check = _real_check")
        out += lines[st:ends[k]]
    out.append('print(f"{passed} checks passed, {failed} failed")')
    out.append("sys.exit(1 if failed else 0)")
    return "\n".join(out)

def main():
    if len(sys.argv) != 2:
        print(__doc__); return 2
    lines = open(SRC, encoding="utf-8").read().split("\n")
    starts, ends = sections(lines)
    titles = [lines[s][len('print("'):-2] for s in starts]
    hits = [k for k, t in enumerate(titles) if sys.argv[1] in t]
    if len(hits) != 1:
        print(f"{len(hits)} sections match {sys.argv[1]!r}:"); [print("  ", titles[k]) for k in hits]; return 2
    target = hits[0]
    # the target runs with everything before it; sections after it are not needed
    keep = list(range(target + 1))
    sub_starts = [starts[k] for k in keep]; sub_ends = [ends[k] for k in keep]
    code = build(lines, sub_starts, sub_ends, target)
    fd, path = tempfile.mkstemp(prefix="_section_", suffix=".py", dir=HERE)
    with os.fdopen(fd, "w", encoding="utf-8") as f: f.write(code)
    try:
        print("running:", titles[target])
        r = subprocess.run([sys.executable, "-u", path], cwd=os.path.dirname(HERE))
        return r.returncode
    finally:
        os.unlink(path)

if __name__ == "__main__":
    sys.exit(main())
