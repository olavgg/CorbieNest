"""Run one section of tests/test_integration.py and report only its checks.

    python3 tests/run_section.py "a pasted block is a token"          # with the sections it needs
    python3 tests/run_section.py --alone "a pasted block is a token"  # the prelude and this section only

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

def build(lines, starts, ends, keep, target):
    """The test file with only the sections in `keep` (indices into starts) present, the others emptied, so every
    line keeps its number. The first kept section's checks are muted until the target's, which are reported."""
    kept_lines = set()
    for k in keep:
        kept_lines.update(range(starts[k], ends[k]))
    first, last_kept = keep[0], keep[-1]
    out = []
    for i, l in enumerate(lines):
        if i >= starts[0] and i not in kept_lines:
            out.append(""); continue
        extra = ""
        if i == starts[first]:
            extra += "; _real_check = check; check = lambda *a, **k: True"
        if i == starts[target]:
            extra += "; check = _real_check"
        out.append(l + extra if extra else l)
    out.append('print(f"{passed} checks passed, {failed} failed")')
    out.append("sys.exit(1 if failed else 0)")
    return "\n".join(out)

def main():
    args = sys.argv[1:]
    alone = "--alone" in args
    args = [a for a in args if a != "--alone"]
    if len(args) != 1:
        print(__doc__); return 2
    lines = open(SRC, encoding="utf-8").read().split("\n")
    starts, ends = sections(lines)
    titles = [lines[s][len('print("'):-2] for s in starts]
    key = args[0]
    if key.startswith("="):   # an exact title: =test interactive: /advisor
        hits = [k for k, t in enumerate(titles) if t == key[1:]]
    else:
        hits = [k for k, t in enumerate(titles) if key in t]
    if len(hits) != 1:
        print(f"{len(hits)} sections match {args[0]!r}:"); [print("  ", titles[k]) for k in hits]; return 2
    target = hits[0]
    # the target runs with everything before it; sections after it are not needed
    keep = [target] if alone else list(range(target + 1))
    code = build(lines, starts, ends, keep, target)
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
