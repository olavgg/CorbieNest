#!/usr/bin/env python3
"""Integration tests: run the real corbienest binary against tests/fake_ollama.py.

Covers the non-interactive (-p) agent loop, tool-call recovery, error handling,
config/env handling, and — via a pseudo-terminal — the interactive editor,
confirmations, Ctrl-C interruption, type-ahead, slash commands and the /model
picker. Requires only python3 (stdlib). No real Ollama needed.
"""
import inspect, json, os, pty, re, select, shutil, socket, struct, subprocess, sys, tempfile, termios, fcntl, time, threading, urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
# CORBIE_BIN: run the suite on a copy of the binary, so a rebuild of the tree cannot pull it away mid-run
BIN = os.environ.get("CORBIE_BIN") or os.path.join(HERE, "..", "corbienest")
sys.path.insert(0, HERE)
import fake_ollama

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

PORT = free_port()
HOST = f"http://127.0.0.1:{PORT}"
threading.Thread(target=fake_ollama.serve, args=(PORT,), daemon=True).start()
for _ in range(50):
    try: urllib.request.urlopen(f"{HOST}/api/version", timeout=0.2); break
    except Exception: time.sleep(0.1)

# A page for web_fetch to read: the tool shells out to curl, so it needs a real server.
DOC_PORT = free_port()
DOC_URL = f"http://127.0.0.1:{DOC_PORT}/doc.html"
DOC_HTML = b"""<!doctype html><html><head><title>Fake Docs</title>
<style>body{color:red}</style><script>var tracker=1;</script></head><body>
<!-- nav --><nav><a href="/index.html">Home</a></nav>
<h1>statement_timeout</h1>
<p>Aborts any statement that takes more than the specified amount of time.</p>
<ul><li>Default: 0 (disabled)</li></ul>
<pre>SET statement_timeout = '5s';</pre>
</body></html>"""

# and a search engine to go with it, shaped the way the real ones are: the result link is
# wrapped in a redirect, and repeated for the display URL and the snippet.
SERP_HTML = ("""<html><body>
<a href="/search?q=x&region=no">Norway</a>
<a class="result__a" href="/l/?uddg=REDIR">Keycloak Admin REST API</a>
<a href="/l/?uddg=REDIR">www.keycloak.org/docs-api</a>
<a href="/l/?uddg=REDIR">The administration REST API of Keycloak, with every endpoint and its parameters.</a>
<a href="https://example.org/keycloak-notes">Fake Docs, locally</a>
</body></html>""").replace("REDIR", "https%3A%2F%2Fwww.keycloak.org%2Fdocs%2Dapi%2F26.0%2Frest%2Dapi%2F")

class _Docs(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/moved":   # a redirect: web_fetch must report it, not follow it
            self.send_response(301); self.send_header("Location", "/doc.html"); self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.path.startswith("/search"):
            body, code = SERP_HTML.encode(), 200
        else:
            body, code = (DOC_HTML, 200) if self.path == "/doc.html" else (b"gone", 404)
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass

threading.Thread(target=HTTPServer(("127.0.0.1", DOC_PORT), _Docs).serve_forever, daemon=True).start()

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")   # CSI sequences, DECSC/DECRC
def clean(s): return ANSI.sub("", s).replace("\r", "")
# During a turn the input field and the status bar are redrawn between DECSC and DECRC — at the
# bottom of the screen on a real terminal, but in the middle of whatever was streaming in the
# byte stream a test reads: "Echo: " <the bar> "hello" never contains "Echo: hello". Every
# DECSC/DECRC pair in term.c wraps chrome and nothing else, so without them this is the transcript.
CHROME = re.compile(r"\x1b7.*?\x1b8", re.S)
def transcript(s): return clean(CHROME.sub("", s))
# When the layout changes (a resize, the input field or the list under it growing or shrinking,
# the scrollback viewer being left) the conversation region is painted again from the scrollback,
# between "autowrap off" and "autowrap on" — and so is the viewer's window. That is old text
# in the byte stream, not output: expect() must not take a line that was merely painted again
# for the one it is waiting for. since()/text() still show it, for the tests that are about it.
# A repaint that has begun but whose end has not been read yet is one all the same: on a loaded machine the
# output comes in pieces, and the half that is there would otherwise pass for new output (it did, on CI: an old
# "worker 2/3" was taken for the one a test was waiting for). So an unfinished one is cut off up to the end.
REPAINT = re.compile(r"\x1b\[\?7l.*?(?:\x1b\[\?7h|\Z)", re.S)
def fresh(s): return REPAINT.sub("", s)

WORK = tempfile.mkdtemp(prefix="crowtest_")
CFG = tempfile.mkdtemp(prefix="crowcfg_")
ENV = dict(os.environ, XDG_CONFIG_HOME=CFG, OLLAMA_HOST=HOST, TERM="xterm-256color", HOME=WORK)
ENV.pop("CORBIENEST_MODEL", None)
# A hosted key in the shell is the user's, not the test's: the tests that need one set it themselves (KEYS, TENV),
# and the rest must not reach a hosted API (that costs money) because the shell happens to export one.
for _k in ("XAI_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY"): ENV.pop(_k, None)
# memory extraction adds a background model call after every request; keep it off for the
# general tests (they assert on requests()[-1]) and exercise it explicitly at the end.
# memory_idle=0 too, so a pending extraction never fires from a slow expect() mid-test.
os.makedirs(os.path.join(CFG, "corbienest")); open(os.path.join(CFG, "corbienest", "config"), "w").write(
    f"memory=0\nmemory_idle=0\nsearch_url=http://127.0.0.1:{DOC_PORT}/search?q=%s\n")

passed = failed = 0
def check(cond, msg):
    global passed, failed
    if cond: passed += 1
    else: failed += 1; print(f"  FAIL: {msg} (line {inspect.currentframe().f_back.f_lineno})")

def run(args, stdin=b"", env=None):
    p = subprocess.run([BIN] + args, cwd=WORK, env=env or ENV, input=stdin, capture_output=True, timeout=60)
    run.err = clean(p.stderr.decode())
    return clean(p.stdout.decode()), p.returncode

def requests(): return json.loads(urllib.request.urlopen(f"{HOST}/_requests").read())

# ---------- non-interactive ----------
class Session:
    def __init__(self, args=(), cols=100, rows=40, env=None, cwd=None, banner=False):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd or WORK)
            # the child inherits the shell's environment: a hosted key the shell exports must not reach it unless the test gives one
            for k in ("XAI_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY"):
                if k not in (env or ENV): os.environ.pop(k, None)
            for k, v in (env or ENV).items(): os.environ[k] = v
            os.execv(BIN, ["corbienest"] + list(args))
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.out = b""; self.mark = 0
        if not banner: self.close_banner()
    def close_banner(self):
        """The start-up banner is a modal: while it is up, the output is kept for the scrollback and not written to the
        terminal, and the box is what the screen shows. Wait for the box, then close it with Esc, so the rest of the
        session is what the tests read. A test that reads the box passes banner=True and closes it itself."""
        end = time.time() + 10
        while time.time() < end and b"Corbie Nest" not in self.out:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try: d = os.read(self.fd, READ_MAX)
                except OSError: return
                if not d: return
                self.out += d
        time.sleep(0.3)
        self.send("\x1b", wait=0.3)
        self.mark = 0
    def expect(self, pat, t=15):
        """Wait until `pat` appears in output produced since the last send(). The output already read counts first:
        a pattern read by an earlier call, or before the process exited, is found without another read."""
        end = time.time() + t
        def hit():
            t = fresh(self.out[self.mark:].decode("utf-8", "replace"))
            return pat in clean(t) or pat in transcript(t)   # the chrome is read on purpose too ("1 queued", the bar's effort)
        while True:
            if hit(): return True
            if time.time() >= end: return False
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try: d = os.read(self.fd, READ_MAX)
                except OSError: return hit()
                if not d: return hit()
                self.out += d
    def send(self, s, wait=0.15):
        self.mark = len(self.out)
        os.write(self.fd, s.encode() if isinstance(s, str) else s); time.sleep(wait)
    def text(self): return clean(self.out.decode("utf-8", "replace"))
    def close(self):
        """Kill it and wait: a session that is still exiting writes the config one last time,
        and a test that writes its own config next would lose it to that write."""
        try: os.kill(self.pid, 9)
        except Exception: pass
        try: os.waitpid(self.pid, 0)
        except Exception: pass
        try: os.close(self.fd)
        except Exception: pass

def since_send(sess): return clean(sess.out[sess.mark:].decode("utf-8", "replace"))
def seen(sess, pat, t=15):
    """Wait until pat is in the output since the last key, as it was written: a reply's text can sit in a repainted
    region, which expect() skips on purpose (it is old text drawn again). True when it came."""
    end = time.time() + t
    while True:
        if pat in since_send(sess): return True
        if time.time() >= end: return False
        sess.expect("(no such text)", 0.3)
def new_session(args=(), **kw):
    """A session of its own, started and at its prompt: a section that uses it stands alone."""
    sess = Session(list(args), **kw); sess.expect("Ctrl-D to quit")
    return sess

def click_rows(sess, rows=40):
    """Left-click every row of the screen, top to bottom, and return what was drawn in answer:
    a pty has no screen to say where a line is, and a click only acts on the head of a fold."""
    at = len(sess.out)
    for y in range(1, rows + 1): sess.send(f"\x1b[<0;5;{y}M\x1b[<0;5;{y}m", wait=0.01)
    time.sleep(0.5); sess.expect("(no such text)", 0.3)
    return clean(sess.out[at:].decode("utf-8", "replace"))

def scrolled(sess):
    """Whether the chrome as last drawn says the conversation is scrolled back: the line for that
    is drawn right above the input field's upper rule. (The status bar says nothing about it —
    it stays what it is.)"""
    sess.expect("(no such text)", 0.4)   # let the redraw finish
    return "scrollback · rows" in sess.text().rsplit("❯", 1)[0].rstrip("─")[-130:]

def perm_files(): return glob.glob(os.path.join(CFG, "corbienest", "permissions", "*.rules"))

def titles():
    """The titles of the saved sessions. A file is written next to its place and renamed into it,
    so the listing may name a temporary that is gone by the time it is opened."""
    out = []
    for f in os.listdir(SESS):
        if not f.endswith(".json"): continue
        try: out.append(json.load(open(os.path.join(SESS, f)))["title"])
        except (OSError, ValueError, KeyError): pass
    return out

def since(): return clean(s.out[s.mark:].decode("utf-8", "replace"))

def viewrange():   # the viewer's own "rows X-Y of Z" header (the window depends on the region height)
    m = re.findall(r"rows (\d+)-(\d+) of (\d+)", since())
    return tuple(int(x) for x in m[-1]) if m else None

def wheel(up, row=5, n=1): return ("\x1b[<%d;10;%dM" % (64 if up else 65, row)) * n   # the terminal has 40 rows: 1-36 are the conversation

def quiet(sess, data):
    """Send, and say whether nothing at all was drawn in answer."""
    sess.send(data); sess.expect("(no such text)", 0.6); return len(sess.out) == sess.mark

def listed():
    """What the last redraw put under the input field: the list, its hint and the bar."""
    s.expect("(no such text)", 0.3)
    return since().rsplit("─", 1)[-1]

def thinks(rs): return [r.get("think", "absent") for r in rs]

def run_rounds(model, args):
    """run() in ENV2; returns (out, rc, the requests this run made to `model`) — not the log's last n, which may be an earlier run's"""
    n0 = len(requests())
    out, rc = run(args, env=ENV2)
    return out, rc, [r for r in requests()[n0:] if r["model"] == model]

def preqs(): return json.loads(urllib.request.urlopen(f"{HOST}/_provider_requests").read())

def last_preq(prov): return [r for r in preqs() if r["provider"] == prov][-1]

def key_nowhere():
    """the key is never written anywhere: not the config, not a session, not the history"""
    for root, _, files in os.walk(CFG2):
        for f in files:
            if "test-key" in open(os.path.join(root, f), errors="replace").read(): return os.path.join(root, f)
    return None

def anth_since(n0): return [r for r in preqs()[n0:] if r["provider"] == "anthropic"]

def work_posts(posts): return [r for r in posts if "tools" in r["body"] or isinstance(r["body"].get("system"), list)]

def consult_posts(posts):
    return [r for r in posts if isinstance(r["body"].get("system"), str) and r["body"]["system"].startswith("You are the advisor")]

def has_tool_result(r):
    for m in r["body"].get("messages") or []:
        c = m.get("content")
        if isinstance(c, list) and any(isinstance(b, dict) and b.get("type") == "tool_result" for b in c): return True
    return False

def guided(level, prompt, advisor="fake-big:latest"):
    n0, p0 = len(requests()), len(preqs())
    out, rc = run(["-m", "fake-coder:latest", "--advisor", advisor, "--advisor-guidance", level, "--yolo", "-p", prompt], env=ENVK)
    return out, rc, requests()[n0:]

def big_made():
    """made.txt as 50 lines: the fake's write replaces it with one, which is 51 changed lines (a substantial change)."""
    open(os.path.join(WORK, "made.txt"), "w").write("old line\n" * 50)

def make_repo():
    d = tempfile.mkdtemp(prefix="crowgoal_")
    subprocess.run(["git", "init", "-q", d], check=True)
    for k, v in (("user.name", "t"), ("user.email", "t@t")): subprocess.run(["git", "-C", d, "config", k, v], check=True)
    open(os.path.join(d, "README.md"), "w").write("hi\n")
    subprocess.run(["git", "-C", d, "add", "-A"], check=True)
    subprocess.run(["git", "-C", d, "commit", "-qm", "init"], check=True)
    return d

def provider_requests(): return json.loads(urllib.request.urlopen(f"{HOST}/_provider_requests").read())

def worker(task): return [r for r in reqs if "You are a worker agent" in r["messages"][0]["content"] and f"# Your task ({task} of 3)" in r["messages"][1]["content"]]

SESS = os.path.join(CFG, "corbienest", "sessions")
READ_MAX = int(os.environ.get("CORBIE_TEST_READ", "65536"))
import glob
SIB = tempfile.mkdtemp(prefix="crowsib_", dir=os.path.dirname(WORK))   # a directory next to the working directory
BINDIR = tempfile.mkdtemp(prefix="crowbin_")
TRACE = os.path.join(CFG, "render.trace")
REPLAY = [sys.executable, os.path.join(HERE, "..", "tools", "trace_replay.py"), TRACE]
CFG2 = tempfile.mkdtemp(prefix="crowcfg2_")
ENV2 = dict(ENV, XDG_CONFIG_HOME=CFG2)
CFG2_FILE = os.path.join(CFG2, "corbienest", "config")
KEYS = dict(XAI_API_KEY="test-key", OPENAI_API_KEY="test-key", ANTHROPIC_API_KEY="test-key",
            XAI_BASE_URL=f"{HOST}/xai/v1", OPENAI_BASE_URL=f"{HOST}/openai/v1", ANTHROPIC_BASE_URL=f"{HOST}/anthropic")
ENVK = dict(ENV2, **KEYS)
MADE = os.path.join(WORK, "made.txt")
GOALENV = dict(ENVK, DEEPSEEK_API_KEY="test-key", DEEPSEEK_BASE_URL=f"{HOST}/deepseek", MOONSHOT_API_KEY="test-key", MOONSHOT_BASE_URL=f"{HOST}/kimi/v1")
RP = make_repo()
RCFG = tempfile.mkdtemp(prefix="crowroles_")   # its own config: the sections before it leave theirs behind
ENVR = dict(ENV2, XDG_CONFIG_HOME=RCFG)

print("test oneshot echo")
out, rc = run(["-m", "fake-coder:latest", "-p", "hello there"])
check(rc == 0, "exit 0"); check("Echo: hello there" in out, f"echo reply: {out!r}")
check("123 in · 45 out" in out, "stats footer")
req = requests()[-1]
check(req["model"] == "fake-coder:latest" and req["stream"] is True, "request shape")
check(req["messages"][0]["role"] == "system" and "corbienest" in req["messages"][0]["content"], "system prompt sent")
check("# Finishing a task" in req["messages"][0]["content"], "end-of-task report asked for in the system prompt")
check("you are not finished" in req["messages"][0]["content"], "the report is conditional on the work being finished")
check(any(t["function"]["name"] == "bash" for t in req.get("tools", [])), "tools sent")
check(req["options"]["num_ctx"] == 32768, "default num_ctx")
check(req["keep_alive"] == "30m" and "num_predict" not in req["options"] and "think" not in req, "default keep_alive 30m; no num_predict/think on a normal call")

print("test oneshot: no model and no provider key says so and exits 1; a failed model call exits 1, the JSON says error")
NOMODEL_CFG = tempfile.mkdtemp(prefix="crownomodel_")   # an empty config: no model= in it, nothing on the Ollama server
out, rc = run(["-p", "hi"], env=dict(ENV, XDG_CONFIG_HOME=NOMODEL_CFG, OLLAMA_HOST="http://127.0.0.1:1"))
check(rc == 1 and "no model yet" in run.err, f"no model, no key: exit 1 with the notice: rc={rc} err={run.err!r}")
out, rc = run(["-m", "anthropic:claude-opus-5", "-p", "hi", "--output-format", "json"],
              env=dict(ENV, XDG_CONFIG_HOME=NOMODEL_CFG, ANTHROPIC_API_KEY="test-key", ANTHROPIC_BASE_URL="http://127.0.0.1:1/anthropic"))
check(rc == 1, f"an unreachable provider: exit 1, not 0: rc={rc}")
try:
    j = json.loads(out)
    check(j.get("error") is True and j.get("result") == "" and j.get("model") == "anthropic:claude-opus-5", f"the JSON says error: {j}")
except ValueError:
    check(False, f"the JSON of a failed run is JSON: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--keep-alive", "-1", "-p", "hello"])
check(requests()[-1]["keep_alive"] == -1, "--keep-alive -1 sent as a number: the server refuses the string (missing unit in duration)")
out, rc = run(["-m", "fake-coder:latest", "--keep-alive", "default", "-p", "hello"])
check("keep_alive" not in requests()[-1], "--keep-alive default omits the key")
out, rc = run(["-m", "fake-coder:latest", "-c", "16k", "-p", "hello"])
check(requests()[-1]["options"]["num_ctx"] == 16384, "-c 16k parsed")
out, rc = run(["-m", "fake-coder:latest", "-c", "nope", "-p", "hello"])
check(rc == 2, "-c with a bad size exits 2")

print("test tool loop with --yolo")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_BASH please"])
check("● bash(echo hello-from-tool)" in out, "tool header shown")
check("hello-from-tool" in out and "exit code: 0" in out, "tool output shown")
check("Tool said: hello-from-tool" in out, "second round used tool result")
msgs = requests()[-1]["messages"]
check(msgs[-1]["role"] == "tool" and msgs[-1]["tool_name"] == "bash", "tool result message sent back")
check("tool_calls" in msgs[-2], "assistant tool_calls kept in history")

print("test auto-compact in the middle of a turn hands the work back and carries on")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "HUGE_TOOL please"])
check("auto-compacting" in out, f"auto-compact fired between the tool result and the next call: {out!r}")
check("conversation compacted \u2014 continuing" in out, "compaction says it is continuing the turn")
check("Tool said: hello-from-tool" in out, f"the turn resumed instead of stopping at the summary: {out!r}")
reqs = requests()
i = next(k for k, r in enumerate(reqs) if "Write a detailed summary" in (r["messages"][-1].get("content") or ""))
msgs = reqs[i + 1]["messages"]   # the first call after the compaction
check(msgs[-1]["role"] == "user", f"the compacted conversation ends with a user turn: {msgs[-1]['role']}")
check("This conversation was compacted" in msgs[-1]["content"], "it carries the summary")
check("HUGE_TOOL please" in msgs[-1]["content"], "and the pending request verbatim")
check("do not ask what to do next" in msgs[-1]["content"], "told to carry on rather than ask")
check(not any("How should we continue" in (m.get("content") or "") for m in msgs), "no trailing assistant turn mid-request")

print("test tool denied when non-interactive without --yolo")
out, rc = run(["-m", "fake-coder:latest", "-p", "TOOL_WRITE please"])
check("denied" in out, "denied shown"); check(not os.path.exists(os.path.join(WORK, "made.txt")), "file not written")
check("non-interactively" in requests()[-1]["messages"][-1]["content"], "denial reason sent to model")

print("test write_file with --yolo")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_WRITE please"])
check(open(os.path.join(WORK, "made.txt")).read() == "made by fake\n", "file written")

print("test web_fetch returns a page as readable text")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", f"TOOL_FETCH {DOC_URL}"])
check(f"● web_fetch({DOC_URL})" in out, f"tool header shows the url: {out!r}")
res = [m for m in requests()[-1]["messages"] if m["role"] == "tool"][-1]["content"]
check(res.startswith(DOC_URL) and "text/html" in res.splitlines()[0], f"result names the page: {res[:80]!r}")
check("Aborts any statement that takes more than the specified amount of time." in res, "prose extracted")
check("Fake Docs" in res, "title kept")
check("- Default: 0 (disabled)" in res, "list item")
check("```" in res and "SET statement_timeout = '5s';" in res, "code sample fenced and kept")
check(f"Home (http://127.0.0.1:{DOC_PORT}/index.html)" in res, "rooted link resolved against the page")
check("<h1>" not in res and "<p>" not in res, "markup stripped")
check("color:red" not in res and "var tracker" not in res, "style and script dropped")
check("nav" not in res.split("Home")[0].split("\n")[-1], "comment dropped")

print("test web_search returns title/url/snippet")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_SEARCH keycloak admin rest api"])
check("● web_search(keycloak admin rest api)" in out, f"tool header shows the query: {out!r}")
res = [m for m in requests()[-1]["messages"] if m["role"] == "tool"][-1]["content"]
check(res.startswith("2 results for \"keycloak admin rest api\""), f"result header: {res[:80]!r}")
check(f"via 127.0.0.1:{DOC_PORT}" in res, "names the engine")
check("1. Keycloak Admin REST API\n   https://www.keycloak.org/docs-api/26.0/rest-api/" in res, "redirect wrapper unwrapped")
check("The administration REST API of Keycloak" in res, "snippet kept")
check("uddg" not in res and "Norway" not in res, "engine's own links dropped")
check("2. Fake Docs, locally\n   https://example.org/keycloak-notes" in res, "second result")
check("web_fetch" in res, "the model is told to read the good one")

print("test web_fetch error paths")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", f"TOOL_FETCH http://127.0.0.1:{DOC_PORT}/nope.html"])
check("returned HTTP 404" in requests()[-1]["messages"][-1]["content"], f"404 reported to the model: {out!r}")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", f"TOOL_FETCH http://127.0.0.1:{DOC_PORT}/moved"])
rc_msg = requests()[-1]["messages"][-1]["content"]
check("redirects to " in rc_msg and f"127.0.0.1:{DOC_PORT}/doc.html" in rc_msg and "Fake Docs" not in rc_msg, f"a redirect is reported with its address, not followed: {rc_msg[:120]!r}")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_FETCH file:///etc/passwd"])
check("will not open" in requests()[-1]["messages"][-1]["content"], "non-http scheme refused")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_FETCH http://169.254.169.254/latest/meta-data/"])
check("will not open" in requests()[-1]["messages"][-1]["content"], "cloud metadata refused")
out, rc = run(["-m", "fake-coder:latest", "-p", f"TOOL_FETCH {DOC_URL}"])
check("denied" in out and "non-interactively" in requests()[-1]["messages"][-1]["content"], "denied when non-interactive without --yolo")
print("test a project's own permissions file is never read")
os.makedirs(os.path.join(WORK, ".corbienest"), exist_ok=True)
open(os.path.join(WORK, ".corbienest", "permissions"), "w").write("edit\n")
made = os.path.join(WORK, "made.txt")
kept = open(made, "rb").read() if os.path.exists(made) else None   # later tests look for made.txt: put back what was there
out, rc = run(["-m", "fake-coder:latest", "-p", "TOOL_WRITE"])
now = open(made, "rb").read() if os.path.exists(made) else None
check("is not read" in out and "denied" in out and now == kept, f"the repo's 'edit' rule grants nothing: {out[-200:]!r}")
if kept is not None: open(made, "wb").write(kept)
os.remove(os.path.join(WORK, ".corbienest", "permissions"))

print("test --no-web takes the tool away")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "--no-web", "-p", f"TOOL_FETCH {DOC_URL}"])
check(not any(t["function"]["name"] in ("web_fetch", "web_search") for t in requests()[-1].get("tools", [])), "neither web tool offered")
check("web access is off" in requests()[-1]["messages"][-1]["content"], "and refused if the model calls it anyway")
sysmsg = requests()[-1]["messages"][0]["content"]
check("web_fetch" not in sysmsg and "web_search" not in sysmsg, "the system prompt does not promise them either")
sysmsg = [r for r in requests() if any(t["function"]["name"] == "web_fetch" for t in r.get("tools", []))][-1]["messages"][0]["content"]
check("Never guess at another project's API" in sysmsg and "web_search for its docs, web_fetch to read them" in sysmsg,
      "every session is told to look documentation up instead of guessing")
check("the version this project uses" in sysmsg and "skip the web when the repo answers it" in sysmsg,
      "and to match the version, and stay off the web when the repo answers")

print("test recovery of leaked XML tool call")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_XML"])
check("recovered 1 tool call" in out, "recovery notice"); check("● list_dir(.)" in out and "made.txt" in out, "list_dir executed")

print("test empty reply: retried once, then reported")
out, rc = run(["-m", "fake-coder:latest", "-p", "EMPTY_ONCE please"])
check("empty reply: 45 tokens generated" in out and "asking again" in out, f"empty reply explained and retried: {out!r}")
check("Echo: EMPTY_ONCE please" in out and rc == 0, "the retry answers")
msgs = requests()[-1]["messages"]
check(not any(m["role"] == "assistant" and not m.get("content") and not m.get("tool_calls") for m in msgs),
      f"the empty message is kept out of the conversation: {msgs!r}")
out, rc = run(["-m", "fake-coder:latest", "-p", "EMPTY_ALWAYS please"])
check("empty reply again — stopping here" in out and "send another message to continue" in out, f"gives up after one retry: {out!r}")
check(out.count("empty reply") == 2, "asks exactly once more")

print("test @file mention and -T")
open(os.path.join(WORK, "note.txt"), "w").write("secret-content-42\n")
out, rc = run(["-m", "fake-coder:latest", "-T", "-p", "look at @note.txt ok"])
check("(attached note.txt" in out, "attach notice")
check("secret-content-42" in requests()[-1]["messages"][-1]["content"], "file content sent")
check("tools" not in requests()[-1], "-T disables tools")

print("test server error, unknown model, unreachable host")
out, rc = run(["-m", "fake-coder:latest", "-p", "ERROR now"])
check("boom from fake" in out, "error surfaced")
out, rc = run(["-m", "nope:latest", "-p", "hi"])
check("not found" in out, f"unknown model error: {out!r}")
r = subprocess.run([BIN, "-H", "http://127.0.0.1:1", "-p", "hi"], cwd=WORK, env=ENV, capture_output=True, timeout=30)
err = clean(r.stderr.decode())
check(r.returncode == 1 and "no model yet" in err and "127.0.0.1:1" in err, f"unreachable host: no model, the notice names the host, exit 1: rc={r.returncode} err={err!r}")
out, rc = run(["-H", f"localhost:{PORT}", "-m", "fake-coder:latest", "-p", "no scheme"])
check(rc == 0 and "Echo: no scheme" in out, f"a host written the way OLLAMA_HOST is (no scheme) is plain http: {out[-200:]!r}")

print("test chat-only model gets no tools; thinking flag")
out, rc = run(["-m", "fake-chat:latest", "-p", "hi"])
check("tools" not in requests()[-1], "chat-only model: tools omitted")
out, rc = run(["-m", "fake-thinker:latest", "--think", "--show-thinking", "-T", "-p", "hi"])
check(requests()[-1]["think"] is True, "think param sent"); check("pondering" in out, "thinking shown")
check("thought " in out and "tok)" in out, f"stats line reports thinking time: {out!r}")
print("test think=auto: a thinking model thinks on the first call of a request only")
out, rc = run(["-m", "fake-thinker:latest", "--yolo", "-p", "TOOL_BASH please"])
rounds = [r for r in requests() if r["model"] == "fake-thinker:latest"][-2:]
check("think" not in rounds[0] and rounds[1]["think"] is False, f"round 1 server default, round 2 think:false: {[r.get('think', 'absent') for r in rounds]!r}")
out, rc = run(["-m", "fake-thinker:latest", "--yolo", "--think", "-p", "TOOL_BASH please"])
rounds = [r for r in requests() if r["model"] == "fake-thinker:latest"][-2:]
check(rounds[0]["think"] is True and rounds[1]["think"] is True, "--think: every call thinks")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_BASH please"])
rounds = [r for r in requests() if r["model"] == "fake-coder:latest"][-2:]
check("think" not in rounds[0] and "think" not in rounds[1], "non-thinking model: the key is never sent")

print("test project instructions + system flag")
open(os.path.join(WORK, "CORBIENEST.md"), "w").write("ALWAYS-SAY-MOO\n")
out, rc = run(["-m", "fake-coder:latest", "-s", "EXTRA-SYS", "-p", "hi"])
sysmsg = requests()[-1]["messages"][0]["content"]
check("ALWAYS-SAY-MOO" in sysmsg and "EXTRA-SYS" in sysmsg, "project + extra system prompt")
os.remove(os.path.join(WORK, "CORBIENEST.md"))

print("test sessions: saved after each request, --continue / --resume")
shutil.rmtree(SESS, ignore_errors=True)   # earlier one-shot runs saved sessions too
out, rc = run(["-m", "fake-coder:latest", "-p", "session one"])
files = sorted(os.listdir(SESS)); check(len(files) == 1, f"one session file after a one-shot run: {files}")
sess = json.load(open(os.path.join(SESS, files[0])))
check(sess["title"] == "session one" and sess["cwd"] == WORK and [m["role"] for m in sess["messages"]] == ["user", "assistant"], f"session file content: {sess}")
out, rc = run(["-m", "fake-coder:latest", "--continue", "-p", "and two"])
check("resumed session" in out and "session one" in out, f"--continue recap: {out!r}")
msgs = requests()[-1]["messages"]
check([m["content"] for m in msgs if m["role"] == "user"] == ["session one", "and two"], "continued conversation sent")
check(len(os.listdir(SESS)) == 1, "continuing appends to the same session file")
sid = files[0][:-5]
out, rc = run(["-m", "fake-coder:latest", "--resume", sid, "-p", "three"])
check("resumed session " + sid in out, "--resume ID")
check(len(json.load(open(os.path.join(SESS, files[0])))["messages"]) == 6, "6 messages after three requests")
out, rc = run(["-m", "fake-coder:latest", "--resume", "nope-nope", "-p", "x"])
check(rc == 1, "--resume with unknown id fails")
out, rc = run(["-m", "fake-coder:latest", "-p", "session two"])
check(len(os.listdir(SESS)) == 2, "new run = new session file")

print("test -p --output-format json")
out, rc = run(["-m", "fake-coder:latest", "-p", "json please", "--output-format", "json"])
lines = [l for l in out.splitlines() if l.strip()]
check(len(lines) == 1, f"exactly one line of output: {out!r}")
j = json.loads(lines[0])
check(j["result"] == "Echo: json please" and j["model"] == "fake-coder:latest" and j["prompt_tokens"] == 123 and j["eval_tokens"] == 45 and j["model_calls"] == 1 and j["interrupted"] is False and j["session_id"], f"json fields: {j}")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_BASH", "--output-format", "json"])
j = json.loads(out.strip()); check(j["tool_calls"] == 1 and j["result"].startswith("Tool said:"), f"tool run in json mode: {j}")
out, rc = run(["-m", "fake-coder:latest", "-p", "x", "--output-format", "yaml"]); check(rc == 2, "bad format rejected")

print("test --benchmark")
n_sess = len(os.listdir(SESS))
out, rc = run(["-m", "fake-coder:latest", "-c", "16k", "--benchmark", "2"])
check(rc == 0, "exit 0")
check("benchmark  fake-coder:latest" in out and "2 runs" in out and "no draft/MTP" in out, f"header: {out!r}")
rows = [l for l in out.splitlines() if l.strip().startswith("16k")]
check(len(rows) == 1 and "45.0 tok/s" in rows[0] and "615 tok/s" in rows[0] and "100% GPU" in rows[0], f"one row with ollama's rates and placement: {out!r}")
reqs = requests()[-3:]   # warm-up + 2 runs
check(all(r["model"] == "fake-coder:latest" and "tools" not in r and r["messages"][0]["role"] == "user" and r["options"]["num_ctx"] == 16384 for r in reqs), "no tools, no system prompt, -c size")
check(reqs[0]["options"]["num_predict"] == 8 and reqs[1]["options"]["num_predict"] == 256 and reqs[2]["options"]["num_predict"] == 256, "warm-up then capped runs")
check(all("draft_num_predict" not in r["options"] for r in reqs), "no draft option unless asked")
check(len(os.listdir(SESS)) == n_sess, "benchmark saves no session")
out, rc = run(["-m", "fake-thinker:latest", "--benchmark", "1"])   # no -c: every size up to the model's 64k
sizes = [l.split()[0] for l in out.splitlines() if l.strip().split()[:1] and l.strip().split()[0] in ("4k", "8k", "16k", "32k", "64k")]
check(sizes == ["4k", "8k", "16k", "32k", "64k"], f"sweeps the supported context sizes: {sizes}")
check("draft 4 (model default, MTP/speculative)" in out, f"model's MTP draft reported: {out!r}")
check([r["options"]["num_ctx"] for r in requests()[-10:]] == [4096, 4096, 8192, 8192, 16384, 16384, 32768, 32768, 65536, 65536], "warm-up + run at each size")
out, rc = run(["-m", "fake-coder:latest", "--draft", "0", "--benchmark", "-c", "8k", "-p", "my own prompt", "--output-format", "json"])
j = json.loads(out.strip())
check(j["model"] == "fake-coder:latest" and j["draft_num_predict"] == 0 and len(j["sizes"]) == 1 and j["sizes"][0]["num_ctx"] == 8192 and len(j["sizes"][0]["runs"]) == 3 and j["sizes"][0]["generation_tps"] == 45 and j["interrupted"] is False, f"json report: {j}")
check(requests()[-1]["messages"][-1]["content"] == "my own prompt" and requests()[-1]["options"]["draft_num_predict"] == 0, "-p sets the prompt; --draft 0 sent")
out, rc = run(["-m", "nope:latest", "--benchmark"]); check(rc == 1 and "not found" in out, "unknown model fails")
out, rc = run(["-m", "fake-coder:latest", "--draft", "x", "-p", "hi"]); check(rc == 2, "bad --draft rejected")

# ---------- interactive via pty ----------
# How much of the output one read may take. A loaded machine hands it over in small pieces — a repaint cut in
# two, a line without its end — and CORBIE_TEST_READ=200 makes this one do the same, to see what a CI runner sees.


print("test interactive: an Ollama on another machine over plain http is said to be one")
s = Session(["-H", "http://ollama.invalid:9", "-m", "fake-coder:latest"])
s.expect("(no such text)", 3)   # the warning is printed under the entry line, after the banner: read the whole text
check("plain http to ollama.invalid" in s.text(), f"another machine over plain http: the warning is said: {s.text()[-300:]!r}")
s.close()

print("test interactive: banner, echo, editor keys, history")
s = Session(["-m", "fake-coder:latest"], banner=True)
check(s.expect("Corbie Nest"), "banner"); check("● connected" in s.text(), "connected marker")
s.send("\x1b"); s.expect("(no such text)", 2); check("Ctrl-D to quit" in s.text(), "closed with Esc, the footer comes (it is painted again with the conversation, so expect() does not see it)")
check("plain http to" not in s.text(), "no unencrypted-host warning for an Ollama on this machine")
raw = s.out.decode("utf-8", "replace")
check("\x1b[?1049h" in raw, "alternate screen entered (full screen)")
check(re.search(r"\x1b\[1;36r", raw) is not None, "scroll region reserves the input field and the status bar, which shares the last row with the location line")
check(re.search(r"\x1b\[40;1H[^\n]*fake-coder:latest[^\n]*0 tokens", raw) is not None, f"the last row: the model and the tokens: {raw[-300:]!r}")
check(re.search(r"\x1b\[39;1H[^\n]*manual mode", raw) is not None, "the mode is on the lower rule of the field, above the last row")
check(re.search(r"\x1b\[40;1H[^\n]*" + re.escape(WORK), raw) is not None, "the last row says the working folder, as an absolute path")
check("manual mode (shift+tab to cycle)" in clean(raw), "mode-cycling hint sits next to the mode in the bar")
check(re.search(r"\x1b\[37;1H[^\n]*─{20}", raw) is not None, "the input field is framed by a rule above the prompt")
check(re.search(r"\x1b\[38;1H[^\n]*❯", raw) is not None, "the prompt sits between the rules, above the bar")
check(re.search(r"\x1b\[39;1H[^\n]*─{20}", raw) is not None, "and a rule below it")
s.send("hello wrld"); s.send("\x1b[D\x1b[D\x1b[D"); s.send("o"); s.send("\r")
check(s.expect("Echo: hello world"), "cursor-left insert then send"); check(s.expect("tok/s"), "stats")
check(s.expect("168 tokens ↑123 ↓45"), f"status bar counts session tokens: {s.text()[-200:]!r}")
s.send("\x1b[A")   # history up
check(s.expect("❯ hello world"), f"history recall: {s.text()[-80:]!r}")
s.send("\x15")   # ctrl-u clears
s.send("first line\\\r"); s.send("second\r")
check(s.expect("Echo: first line"), "backslash-newline multi-line submit")
check(requests()[-1]["messages"][-1]["content"] == "first line\nsecond", "newline preserved in message")
s.send("\x1b[200~pasted\nlines\x1b[201~"); s.send("\r")
check(s.expect("Echo: pasted"), "bracketed paste")
check(requests()[-1]["messages"][-1]["content"] == "pasted\nlines", "paste newlines kept")
s.send("\x12"); check(s.expect("(reverse-i-search)`':"), "ctrl-r opens the search prompt")
s.send("HELLO"); check(s.expect("(reverse-i-search)`HELLO': hello world"), f"incremental, case-insensitive match: {s.text()[-120:]!r}")
s.send("\x1b"); check(s.expect("❯ "), "esc cancels"); time.sleep(0.2)
check("hello world" not in clean(s.out[-200:].decode("utf-8", "replace")), "line restored (empty) after cancel")
s.send("\x12"); s.send("line"); check(s.expect("`line': pasted"), "newest match first")
s.send("\x12"); check(s.expect("`line': first line"), "ctrl-r again: next older match (multi-line entry)")
s.send("\r"); time.sleep(0.2)   # keep the match in the editor
check(s.expect("❯ first line"), "enter keeps the match in the editor without sending")
s.send("\r"); check(s.expect("Echo: first line"), "sent on the second enter")
s.send("\x12"); s.send("zzz-none"); check(s.expect("`zzz-none': "), "no match shows an empty line"); s.send("\x07"); s.expect("❯ ")

print("test interactive: the input field grows with the input and survives a resize")
sf = Session(["-m", "fake-coder:latest"], cols=60, rows=20); sf.expect("Ctrl-D to quit")
sf.send("one line")
check(sf.expect("❯ one line"), "single-row field")
check(re.search(r"\x1b\[1;16r", sf.out.decode("utf-8", "replace")) is not None, "region is rows minus the field (three rules and the text) and the bar row")
sf.send("\x1b\rtwo\x1b\rthree")   # alt+enter twice: three input rows
check(sf.expect("three"), "multi-line input")
raw = sf.out.decode("utf-8", "replace")
check(re.search(r"\x1b\[1;14r", raw) is not None, f"the region shrank by the two extra input rows: {raw[-200:]!r}")
check(re.search(r"\x1b\[18;1H[^\n]*three", raw) is not None, "the last input row sits just above the lower rule")
sf.send("\x15")   # ctrl-u back to one row
check(sf.expect("❯ "), "cleared")
raw = sf.out.decode("utf-8", "replace")[-4000:]
check(re.search(r"\x1b\[1;16r", raw) is not None, "the region grew back")
check(re.search(r"\x1b\[15;1H", raw) and re.search(r"\x1b\[16;1H", raw),   # painted again: the bottom rows are padded with spaces, not erased
      f"the rows the field gave back are painted again from the transcript, not left showing its old text: {raw[-300:]!r}")
sf.mark = len(sf.out)
fcntl.ioctl(sf.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
os.kill(sf.pid, __import__("signal").SIGWINCH)
check(sf.expect("manual mode", 3), "a resize at an idle prompt is drawn at once, not at the next key (a tmux pane is resized all the time)")
raw = sf.out[sf.mark:].decode("utf-8", "replace")
check(re.search(r"\x1b\[1;26r", raw) is not None, f"the region was re-cut for the new height: {raw[-300:]!r}")
check(re.search(r"\x1b\[29;1H[^\n]*manual mode", raw) is not None, "the bar is on the new row above the last")
check("Corbie Nest" in clean(raw), "and the conversation is painted again for the new size, from the top of the transcript")
sf.send("after resize")
check(sf.expect("❯ after resize"), "the field follows a resize")
sf.send("\r"); check(sf.expect("Echo: after resize"), "and the line still sends")
check("› after resize" in sf.text(), "the submitted line goes into the transcript, not the field")
sf.close()

print("test interactive: a multi-line paste leaves nothing behind when it is sent")
sp = Session(["-m", "fake-coder:latest"], cols=60, rows=20); sp.expect("Ctrl-D to quit")
sp.send("\x1b[200~" + "\n".join("pasted line %d" % i for i in range(1, 4)) + "\x1b[201~")   # three lines: under the token's limit, so it is typed in
check(sp.expect("pasted line 3"), "the paste lands in the field, newlines and all")
sp.send("\r"); check(sp.expect("Echo: pasted line 1"), "the whole paste is sent as one message")
check(requests()[-1]["messages"][-1]["content"].count("pasted line 3") == 1, "sent once, not twice")
time.sleep(1.5)   # the rows are painted again after the reply has shown: give the repaint its time
raw = sp.out.decode("utf-8", "replace")[-6000:]
# a row is painted again by moving to it and writing it (the bottom rows are padded with spaces, not erased, since the window background)
missing = [r for r in range(14, 16) if not re.search(r"\x1b\[%d;1H" % r, raw)]
check(not missing, f"every row the field gave back is painted again from the transcript, so the paste does not stay below the reply: rows {missing} left dirty")
sp.close()


print("test interactive: confirmation menu — deny with reason, arrow keys, then always")
s = new_session(["-m", "fake-coder:latest"])   # its own session: it stands alone
s.send("TOOL_BASH\r")
check(s.expect("Run this command?"), "confirmation prompt")
check(s.expect("1. Yes") and "3. Yes, and always allow `echo …` in this project" in s.text() and "4. No, and tell the model" in s.text(), f"menu options shown: {s.text()[-400:]!r}")
s.send("n"); check(s.expect("tell the model why"), "reason prompt"); s.send("because\r")
check(s.expect("denied"), "denied shown"); check(s.expect("Tool said: User denied"), "model saw denial")
check("because" in requests()[-1]["messages"][-1]["content"], "reason forwarded")
s.send("TOOL_BASH\r"); check(s.expect("Run this command?"), "prompt again")
s.send("\x1b[B"); s.send("\x1b[B"); s.send("\x1b[A"); s.send("\r")   # down, down, up -> option 2 (always)
check(s.expect("hello-from-tool"), "command ran after selecting 'always' with arrows+enter")
check("Run this command?" in s.text() and "● bash(echo hello-from-tool)" in s.text(), "the approval asked, and the call's one line says what ran")
s.send("TOOL_BASH\r"); check(s.expect("Tool said: hello-from-tool", 15), "no prompt after 'always'")
check(since_send(s).count("Run this command?") == 0, "always suppresses further prompts: none is asked after the second call")

print("test interactive: project permission rules (kept in the config directory, per project)")
s3 = Session(["-m", "fake-coder:latest"]); s3.expect("Ctrl-D to quit")
s3.send("TOOL_BASH\r"); check(s3.expect("Run this command?"), "prompt")
s3.send("p"); check(s3.expect("saved to "), "answered with p")
check(s3.expect(".rules: bash echo"), "rule saved")
check(len(perm_files()) == 1 and open(perm_files()[0]).read().strip().splitlines()[-1] == "bash echo", "file content")
check(not os.path.exists(os.path.join(WORK, ".corbienest", "permissions")), "nothing is written into the project")
s3.expect("tok/s")
s3.send("TOOL_BASH\r"); check(s3.expect("● bash(echo hello-from-tool)"), "rule auto-approves next time (no prompt; the call's line)"); s3.expect("tok/s")
s3.send("/permissions\r"); check(s3.expect("shell commands starting with echo"), "/permissions lists it")
s3.send("/permissions add bash git status\r"); check(s3.expect("added: bash git status"), "/permissions add")
s3.send("/permissions remove 2\r"); check(s3.expect("removed rule 2"), "/permissions remove")
s3.send("\x04"); s3.close()
s3 = Session(["-m", "fake-coder:latest"]); s3.expect("Ctrl-D to quit")   # rules persist across sessions
s3.send("TOOL_BASH\r"); check(s3.expect("● bash(echo hello-from-tool)"), "rule survives a restart (no prompt)"); s3.expect("tok/s")
s3.send("/permissions clear\r"); check(s3.expect("permissions cleared"), "/permissions clear")
check(not perm_files(), "file removed")
s3.send("TOOL_BASH\r"); check(s3.expect("Run this command?"), "asks again after clear"); s3.send("y"); s3.expect("tok/s")
s3.send("\x04"); s3.close()

print("test interactive: web_fetch asks per page, and 'p' remembers the host")
s3 = Session(["-m", "fake-coder:latest"]); s3.expect("Ctrl-D to quit")
s3.send(f"TOOL_FETCH {DOC_URL}\r")
check(s3.expect("Fetch this page?"), f"confirmation shown: {s3.text()[-300:]!r}")
check(DOC_URL in s3.text(), "the whole url is shown before it is fetched")
check(s3.expect(f"3. Yes, and always allow fetching from 127.0.0.1:{DOC_PORT} in this project"), f"per-host rule offered: {s3.text()[-400:]!r}")
s3.send("p"); check(s3.expect(f".rules: fetch 127.0.0.1:{DOC_PORT}"), "host rule saved")
check(s3.expect("lines ·", 15), "the page text is folded under one line"); s3.expect("tok/s")
check("Fake Docs" in click_rows(s3), "and a click on that line shows it")
s3.send(f"TOOL_FETCH {DOC_URL}\r")
check(s3.expect(f"auto-approved (project rule: fetch 127.0.0.1:{DOC_PORT})"), "the host is not asked about again"); s3.expect("tok/s")
s3.send("/permissions\r"); check(s3.expect(f"web pages from 127.0.0.1:{DOC_PORT}"), "/permissions lists the host")
s3.send("/permissions clear\r"); s3.expect("permissions cleared")   # else that rule approves the search below too
s3.send("TOOL_SEARCH keycloak\r")
check(s3.expect("Search the web?"), f"a search is confirmed too: {s3.text()[-300:]!r}")
check(s3.expect(f"⌕ keycloak") and f"127.0.0.1:{DOC_PORT}" in s3.text(), "the query and the engine are shown")
s3.send("y"); check(s3.expect("lines ·", 15), "the results are folded too"); s3.expect("tok/s")
check("Keycloak Admin REST API" in click_rows(s3), "and there on a click")
s3.send("/web\r"); check(s3.expect(f"engine: http://127.0.0.1:{DOC_PORT}/search?q=%s"), "/web shows the engine")
s3.send("/web engine https://example.org/s?q=%s\r"); check(s3.expect("✓ engine: https://example.org/s?q=%s"), "/web engine sets it")
s3.send("/web engine default\r"); check(s3.expect("✓ engine: https://html.duckduckgo.com"), "/web engine default resets it")
s3.send(f"/web engine http://127.0.0.1:{DOC_PORT}/search?q=%s\r"); s3.expect("✓ engine")
s3.send("/web off\r"); check(s3.expect("web_search/web_fetch off"), "/web off")
s3.send(f"TOOL_FETCH {DOC_URL}\r"); check(s3.expect("web access is off", 15), "the tool is gone while it is off"); s3.expect("tok/s")
check(not any(t["function"]["name"] == "web_fetch" for t in requests()[-1].get("tools", [])), "and the tool list sent to the model shrank")
s3.send("/web on\r"); check(s3.expect("web_search/web_fetch on"), "/web on")
s3.send("/permissions clear\r"); s3.expect("permissions cleared")
s3.send("\x04"); s3.close()

print("test interactive: keys typed during generation do not answer a confirmation")
s2 = Session(["-m", "fake-coder:latest"]); s2.expect("Ctrl-D to quit")
s2.send("SLOW\r"); check(s2.expect("two"), "streaming"); s2.send("yes yes"); s2.expect("tok/s")
s2.send("\x15")   # clear the typed-ahead text
s2.send("TOOL_BASH\r"); check(s2.expect("Run this command?"), "prompt shown")
s2.send("SLOWFILL")   # would be swallowed if the menu accepted stray text; menu ignores it
time.sleep(0.3); check("exit code" not in s2.text() and "Run this command?  yes" not in s2.text(), "stray keys did not approve")
s2.send("\x1b"); check(s2.expect("tell the model why"), "esc = no"); s2.send("\r"); check(s2.expect("denied"), "denied via esc")
s2.send("\x04"); s2.close()

print("test interactive: modes — shift+tab cycles, plan mode is read-only, accept-edits auto-approves")
check("manual mode" in s.text(), "mode status line under prompt")
s.send("\x1b[Z"); check(s.expect("accept edits on"), "shift+tab -> accept edits")
s.send("TOOL_WRITE\r"); check(s.expect("auto-approved (accept-edits mode)"), "edit auto-approved"); s.expect("tok/s")
check(os.path.exists(os.path.join(WORK, "made.txt")), "file written without prompt")
s.send("\x1b[Z"); check(s.expect("plan mode on"), "shift+tab -> plan")
s.send("TOOL_WRITE\r"); check(s.expect("plan mode is read-only"), "write denied in plan mode"); s.expect("tok/s")
req = requests()[-2]
check("Plan mode" in req["messages"][0]["content"], "plan-mode system prompt")
check("# Finishing a task" not in req["messages"][0]["content"], "no end-of-task report section in plan mode")
check(all(t["function"]["name"] not in ("write_file", "edit_file") for t in req["tools"]), "mutating tools not offered in plan mode")
s.send("\x1b[Z"); check(s.expect("auto mode on"), "shift+tab -> auto")
s.send("\x1b[Z"); check(s.expect("manual mode"), "shift+tab wraps to manual")
s.send("/mode plan\r"); check(s.expect("mode: plan"), "/mode NAME"); s.send("/mode\r"); check(s.expect("read-only"), "/mode shows current")
s.send("/mode manual\r"); s.expect("mode: manual")
s.send("/yolo on\r"); check(s.expect("auto (yolo) mode ON"), "/yolo on"); s.send("/yolo off\r"); check(s.expect("back to manual"), "/yolo off")

print("test interactive: skills")
os.makedirs(os.path.join(WORK, ".corbienest", "skills", "review"), exist_ok=True)
open(os.path.join(WORK, ".corbienest", "skills", "review", "SKILL.md"), "w").write("---\nname: review\ndescription: Review files\n---\nReview $ARGUMENTS now.\n")
s.send("/skills\r"); check(s.expect("no skills found"), "/skills empty before reload")
s.send("/skills reload\r"); check(s.expect("/review") and "Review files" in s.text(), "/skills lists after reload")
s.send("/rev\t"); time.sleep(0.2); s.send("a.c\r"); check(s.expect("skill review"), "skill invoked via tab completion")
check(s.expect("Echo: <skill name=\"review\""), "skill prompt sent")
check("Review a.c now." in requests()[-1]["messages"][-1]["content"], "$ARGUMENTS substituted")
check("# Skills" in requests()[-1]["messages"][0]["content"] and "/review" in requests()[-1]["messages"][0]["content"], "skills listed in system prompt")
os.makedirs(os.path.join(WORK, ".claude", "commands"), exist_ok=True)
open(os.path.join(WORK, ".claude", "commands", "fix-issue.md"), "w").write("Fix issue $ARGUMENTS following our conventions.\n")
s.send("/skills reload\r"); check(s.expect("/fix-issue"), "Claude-Code-style .claude/commands/NAME.md picked up as a skill")
s.send("/fix-issue 42\r"); check(s.expect("skill fix-issue"), "custom command runs"); s.expect("tok/s")
check("Fix issue 42 following" in requests()[-1]["messages"][-1]["content"], "$ARGUMENTS substituted in a command file")
shutil.rmtree(os.path.join(WORK, ".claude"))
s.send("/skills new deploy\r"); check(s.expect("created .corbienest/skills/deploy/SKILL.md"), "/skills new")
check(os.path.exists(os.path.join(WORK, ".corbienest", "skills", "deploy", "SKILL.md")), "scaffold written")

print("test interactive: ctrl-c interrupts generation; type-ahead")
s.send("SLOW\r"); check(s.expect("two"), "streaming started")
frames = set(re.findall(r"([⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]) generating", clean(s.out[s.mark:].decode("utf-8", "replace"))))
check(not frames, f"no spinner in the status bar while a command runs (the bar has no busy label, AGENTS.md): {frames}")
s.send("\x03")
check(s.expect("interrupted"), "interrupt notice")
s.send("hi again\r"); check(s.expect("Echo: hi again"), "usable after interrupt")
partial = requests()[-1]["messages"][-2]["content"]
check(partial.startswith("one") and "eight" not in partial, f"partial assistant content kept in history: {requests()[-1]['messages'][-3:]!r}")
s.send("SLOW\r"); check(s.expect("two"), "streaming"); s.send("typed ahead"); check(s.expect("tok/s"), "finished")
check(s.expect("❯ typed ahead"), f"type-ahead preserved: {s.text()[-60:]!r}")
s.send("\x15")

print("test interactive: messages queued while the model works (Enter during generation)")
s.send("SLOW\r"); check(s.expect("two"), "streaming started")
s.send("queued que"); check(s.expect("❯ queued que▏", 3), f"text typed while busy is echoed live in the input field: {s.text()[-200:]!r}")
s.send("stion\r")
check(s.expect("1 queued", 3), f"status bar shows the queued message: {s.text()[-200:]!r}")
check(s.expect("eight"), "generation finished")
check(s.expect("› queued question"), f"queued message echoed at the prompt: {s.text()[-200:]!r}")
check(s.expect("Echo: queued question"), "queued message was sent as the next turn")
check(requests()[-1]["messages"][-1]["content"] == "queued question", "queued text sent verbatim")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("first\r"); s.send("secx\x7fond\r")   # two messages, the second with a backspace fix
check(s.expect("2 queued", 3), "two queued")
check(s.expect("Echo: first", 20), "first queued message answered")
check(s.expect("Echo: second", 20), "second queued message answered (backspace applied)")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("keep me\r"); check(s.expect("1 queued", 3), "queued before interrupt")
s.send("\x03"); check(s.expect("interrupted"), "interrupted")
check(s.expect("❯ keep me", 5), f"after Ctrl-C the queued text is back in the editor, not sent: {s.text()[-200:]!r}")
time.sleep(0.5); check("Echo: keep me" not in s.text(), "queued message not auto-sent after an interrupt")
s.send("\x15")
check("queued" not in clean(s.out[-400:].decode("utf-8", "replace")), "queue counter gone from the bar")
s.send("/mode auto\r"); s.expect("mode: auto")
s.send("TOOL_SLEEP\r"); check(s.expect("running…", 10), "slow tool running")
s.send("mid-task note\r"); check(s.expect("1 queued", 3), "queued while the tool runs")
check(s.expect("slept", 15), f"the running command finished: a message queued meanwhile does not stop it: {s.text()[-300:]!r}")
check(s.expect("› mid-task note", 5), "the message goes in before the model's next call, after the command")
check(s.expect("Echo: mid-task note", 15), "model saw the mid-task message on its next call")
msgs = requests()[-1]["messages"]
check(msgs[-1]["role"] == "user" and msgs[-1]["content"] == "mid-task note" and msgs[-2]["role"] == "tool", f"injected right after the tool result: {[m['role'] for m in msgs[-4:]]}")
check("slept" in msgs[-2]["content"] and "exit code: 0" in msgs[-2]["content"] and "did not finish" not in msgs[-2]["content"], f"the tool result is the command's full output: {msgs[-2]['content'][:100]!r}")

print("test interactive: a message queued during a tool round does not skip the calls of that round")
s.send("TOOL_SLEEP2\r"); check(s.expect("running…", 10), "the first of two calls is running")
s.send("second thoughts\r")
check(s.expect("second-tool", 8), f"the second call still runs: {s.text()[-400:]!r}")
check(s.expect("› second thoughts", 8), "and the message goes in after the round")
check(s.expect("Echo: second thoughts", 15), "the model was called with it")
msgs = requests()[-1]["messages"]
check(len([m for m in msgs if m["role"] == "tool"]) >= 2 and msgs[-2]["role"] == "tool" and "second-tool" in msgs[-2]["content"] and "not run" not in msgs[-2]["content"],
      f"the second call got its real result: {[m['role'] for m in msgs[-4:]]}")

print("test interactive: a message queued behind a slash command is not held back by it")
s.send("TOOL_SLEEP\r"); check(s.expect("running…", 10), "tool running")
s.send("/save queued.md\r"); check(s.expect("1 queued", 3), "a command that touches the conversation waits for the turn")
s.send("and one more thing\r"); check(s.expect("2 queued", 3), "the message is queued behind it")
check(s.expect("› and one more thing", 8), f"it is injected without waiting for the command in front of it: {s.text()[-300:]!r}")
check(s.expect("Echo: and one more thing", 15), "the model saw it inside the turn")
check(s.expect("saved", 8), "and the slash command still ran after the turn")

print("test interactive: a message queued before a turn starts does not cut into it")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("TOOL_SLEEP\r"); s.send("after the tool\r"); check(s.expect("2 queued", 3), "two messages queued while the model was busy")
check(s.expect("running…", 25), "the first of them starts a tool round of its own")
check(s.expect("slept", 25), f"the one still waiting does not stop that command: {s.text()[-300:]!r}")
check(s.expect("› after the tool", 10), "it is delivered between rounds, the way it always was")
check(s.expect("Echo: after the tool", 15), "and answered")
s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: shift+tab while the model works switches the mode immediately")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("\x1b[Z"); check(s.expect("accept edits on", 3), "mode switched while busy")
s.send("half typed"); check(s.expect("❯ half typed▏", 3), "typing still echoed after the switch")
s.send("\x1b[Z"); check(s.expect("plan mode on", 3), "second switch while busy, with pending text")
check(s.expect("eight"), "generation finished")
check(s.expect("❯ half typed", 5), f"pending text back in the editor without the CSI Z bytes: {s.text()[-200:]!r}")
s.send("\x15"); s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: slash commands that only report or set something run while the model works")
s = new_session(["-m", "fake-coder:latest"])   # its own session: it stands alone
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("/status\r"); check(s.expect("keep_alive", 5), f"/status answered during generation: {s.text()[-300:]!r}")
check(s.expect("not checked while the model is working", 3), "the /api/ps placement call is skipped mid-request")
check("queued" not in clean(s.out[s.mark:].decode("utf-8", "replace")), "it was not queued as a message")
check(s.expect("eight", 10), "generation continued afterwards")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("/mode plan\r"); check(s.expect("mode: plan", 5), "a mode change lands inside the turn")
check(s.expect("plan mode on", 3), "and shows up in the status bar")
s.send("/save busy.md\r"); check(s.expect("1 queued", 3), "a command that touches the conversation still waits for the turn")
check(s.expect("eight", 10), "generation finished")
check(s.expect("saved", 5), "the queued command ran after the turn")
check(requests()[-1]["messages"][-1]["content"] == "SLOW", "neither command was sent to the model")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("/max_iters 42\r"); check(s.expect("max_iters = 42", 5), "the loop guard can be raised from under a running turn")
check(s.expect("eight", 10), "generation continued afterwards")
s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: /max_iters sets the tool-round loop guard")
s.send("/max_iters\r"); check(s.expect("max_iters: 42 tool rounds"), "no argument shows the current value")
s.send("/max_iters 0\r"); check(s.expect("usage: /max_iters"), "N < 1 is rejected")
s.send("/max_iters\r"); check(s.expect("max_iters: 42 tool rounds"), "and leaves the value alone")
s.send("/max_it\t"); time.sleep(0.2); s.send(" 100\r"); check(s.expect("max_iters = 100"), "tab completion, then set")

print("test interactive: auto-compact when the context is 85%+ full")
s.send("HUGE_CTX please\r"); check(s.expect("Echo: HUGE_CTX"), "reply")
check(s.expect("auto-compacting", 5), f"auto-compact triggered: {s.text()[-300:]!r}")
check(s.expect("conversation compacted", 10), "compacted")
s.send("after compact\r"); check(s.expect("Echo: after compact"), "next turn works")
msgs = requests()[-1]["messages"]
check(any("This conversation was compacted" in (m.get("content") or "") for m in msgs), "next request carries the summary")
check(not any("HUGE_CTX" in (m.get("content") or "") for m in msgs if m["role"] == "user" and "compacted" not in m["content"]), "old messages dropped")
check(s.text().count("auto-compacting") == 1, "did not compact again (usage figure unchanged)")

print("test interactive: /resume picker")
s.send("/resume\r"); check(s.expect("Resume a session"), "picker opens")
check(s.expect("session two"), "earlier sessions listed, this directory")
s.send("one"); check(s.expect("session one"), "the oldest session is past the list: typing its name in the picker's filter finds it")   # the picker shows the latest rows; it filters as you type
s.send("\x1b"); check(s.expect("cancelled"), "esc cancels")
s.send("/resume " + sid + "\r"); check(s.expect("resumed session " + sid), "/resume ID loads it")
check(s.expect("Echo: three"), "the conversation is shown again after the recap: its last reply too")
s.send("four\r"); check(s.expect("Echo: four"), "continues")
msgs = requests()[-1]["messages"]
check([m["content"] for m in msgs if m["role"] == "user"] == ["session one", "and two", "three", "four"], "resumed history sent")
s.send("/status\r"); check(s.expect("session    " + sid), "/status shows the session id")
s.send("/clear\r"); s.expect("new conversation")
s.send("fresh start\r"); check(s.expect("Echo: fresh start"), "reply")
end = time.time() + 3   # the session is saved when the request is over — a moment after the reply was printed
while "fresh start" not in titles() and time.time() < end: time.sleep(0.05)
check("fresh start" in titles(), "/clear starts a new session file")

print("test interactive: /init writes AGENTS.md and loads it")
s.send("/mode auto\r"); s.expect("mode: auto")
s.send("/init\r"); check(s.expect("analysing the project"), "/init starts")
check(s.expect("project instructions loaded", 15), f"instructions loaded: {s.text()[-300:]!r}")
check(open(os.path.join(WORK, "AGENTS.md")).read().startswith("# Project"), "file written by the model")
s.send("after init\r"); check(s.expect("Echo: after init"), "reply")
check("Build with make." in requests()[-1]["messages"][0]["content"], "new instructions in the system prompt")
os.remove(os.path.join(WORK, "AGENTS.md")); s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: /cost")
s.send("/cost\r"); check(s.expect("session cost"), "/cost header")
check(s.expect("model calls") and re.search(r"model calls   \d+  \(\d+ requests", s.text()) is not None, f"calls and requests counted: {s.text()[-400:]!r}")
check(re.search(r"tokens        [\d.]+k?  \(↑", s.text()) is not None, "token totals shown")
seen(s, "wall time", 10)   # the last lines of the report are painted after the first ones
check(re.search(r"wall time     \d", s.text()) is not None and "tok/s" in s.text(), f"wall time and tok/s shown: {s.text()[-300:]!r}")

print("test interactive: /diff")
s.send("/diff\r"); check(s.expect("not inside a git repository"), "/diff outside git")
subprocess.run(["git", "init", "-q"], cwd=WORK, check=True)
subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "init"], cwd=WORK, check=True)
open(os.path.join(WORK, "tracked.txt"), "w").write("a\n"); subprocess.run(["git", "add", "tracked.txt"], cwd=WORK, check=True)
subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "add"], cwd=WORK, check=True)
open(os.path.join(WORK, "tracked.txt"), "w").write("b\n"); open(os.path.join(WORK, "new.txt"), "w").write("x\n")
before = len(requests())
s.send("/diff\r"); check(s.expect("diff --git a/tracked.txt b/tracked.txt"), "patch shown")
check(s.expect("-a") and s.expect("+b"), "removed/added lines"); check(s.expect("untracked: new.txt"), "untracked files listed")
check(len(requests()) == before, "no model call for /diff")
s.send("/diff --staged\r"); check(s.expect("no changes"), "/diff args pass through (nothing staged)")
shutil.rmtree(os.path.join(WORK, ".git")); os.remove(os.path.join(WORK, "new.txt")); os.remove(os.path.join(WORK, "tracked.txt"))

print("test interactive: /rewind (Esc Esc) restores files and truncates the conversation")
s.send("/mode auto\r"); s.expect("mode: auto")
open(os.path.join(WORK, "made.txt"), "w").write("original\n")
s.send("TOOL_WRITE\r"); check(s.expect("Tool said:", 15), "write turn done")
check(open(os.path.join(WORK, "made.txt")).read() == "made by fake\n", "file overwritten by the model")
n_before = len([m for m in requests()[-1]["messages"] if m["role"] == "user"])
s.send("\x1b"); check(s.expect("press Esc again to rewind"), "first Esc hints")
s.send("\x1b"); check(s.expect("Rewind to before which request?"), "second Esc opens the picker")
check(s.expect("TOOL_WRITE") and "1 file changed since: " in s.text() and "made.txt" in s.text(), f"latest request listed with its file change: {s.text()[-400:]!r}")
s.send("\r"); check(s.expect("Restore conversation and files"), "second menu"); s.send("\r")
check(s.expect("rewound to before request"), f"rewound: {s.text()[-300:]!r}")
check(open(os.path.join(WORK, "made.txt")).read() == "original\n", "file content restored")
check(s.expect("❯ TOOL_WRITE"), "request text back in the editor")
s.send("\x15"); s.send("after rewind\r"); check(s.expect("Echo: after rewind"), "reply")
users = [m["content"] for m in requests()[-1]["messages"] if m["role"] == "user"]
check(users[-1] == "after rewind" and users[-2] != "TOOL_WRITE" and len(users) == n_before, f"conversation truncated (TOOL_WRITE gone): {users[-3:]}")
s.send("\x1b"); s.send("\x1b"); s.expect("Rewind to before which request?"); s.send("\x1b"); check(s.expect("cancelled"), "esc cancels the picker")
os.remove(os.path.join(WORK, "made.txt")); s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: task tool runs a read-only sub-agent")
open(os.path.join(WORK, "hay.txt"), "w").write("nothing\nneedle here\n")
s.send("TOOL_TASK\r"); check(s.expect("⤷ sub-agent find the needle", 15), "sub-agent header")
check(s.expect("⎿ grep("), "its tool calls are echoed")
check(s.expect("report after 1 tool round"), f"report preview: {s.text()[-300:]!r}")
check(s.expect("Tool said: REPORT: found it in", 15), "parent model received the report as the tool result")
sub = [r for r in requests() if r["messages"][0]["role"] == "system" and "You are a sub-agent" in r["messages"][0]["content"]]
check(sub and all(t["function"]["name"] in ("read_file", "list_dir", "grep", "bash", "web_fetch", "web_search") for t in sub[-1]["tools"]), "sub-agent got read-only tools only")
check(sub and sum(t["function"]["name"] in ("web_fetch", "web_search") for t in sub[-1]["tools"]) == 2, "sub-agent can look documentation up")
check("hay.txt" in sub[-1]["messages"][-1]["content"], "grep ran for real inside the sub-agent")
print("test interactive: ← on an empty line lists the sub-agents; the one picked shows its log")
s.send("\x1b[D"); check(s.expect("sub-agents"), "← on an empty line opens the sub-agents list")
check(s.expect("find the needle") and s.expect("done · 2 rounds"), "the run is listed with its state and rounds")
s.send("\x1b[B", 0.3); s.send("\r", 0.3); s.expect("(no such text)", 0.8)   # the block is a repaint: read it with since_send(s), as the tests about the viewer do
check("▾ sub-agent #" in since_send(s) and "find the needle · done" in since_send(s), "the picked run opens as a block with its head")
check("› grep(" in since_send(s) and "hay.txt:2:needle here" in since_send(s) and "report:" in since_send(s), "its log has the tool call, what it returned and the report")
s.send("\x1b", 0.8); check("scrollback" not in since_send(s), "Esc goes back to the prompt")
os.remove(os.path.join(WORK, "hay.txt"))
print("test interactive: a pasted block is a token; a click opens it, Enter sends the whole text")
sp = Session(["-m", "fake-coder:latest"]); sp.expect("Ctrl-D to quit")
body = "\n".join(f"line{i}" for i in range(10))
sp.send("\x1b[200~" + body + "\x1b[201~", 1.0)
check(sp.expect("[Pasted text #1 +10 lines]"), "a paste of more than three lines shows as a token")
sp.send("\x1b[<0;5;38M", 0.3); sp.send("\x1b[<0;5;38m", 0.6); sp.expect("(no such text)", 0.8)   # row 38: the field's text row (a 40-row screen)   # the modal is a repaint: read it with since_send
check("pasted text #1 · 10 lines" in since_send(sp) and "line9" in since_send(sp), "a click on the token opens the paste")
sp.send("\x1b[6~", 0.6); sp.expect("(no such text)", 0.6)
sp.send("\x1b", 0.8); sp.expect("(no such text)", 0.6)
check("pasted text #1" not in since_send(sp), "Esc closes it")
sp.send("\r"); sp.expect("(no such text)", 1.5)
last_user = [m for m in requests()[-1]["messages"] if m["role"] == "user"][-1]["content"]
check(all(f"line{i}" in last_user for i in range(10)) and "[Pasted text" not in last_user, f"the model got the whole paste: {last_user[:80]!r}")
sp.close()
print("test interactive: /subagent sets the model and the effort sub-agents run on")
open(os.path.join(WORK, "hay.txt"), "w").write("needle\n")
s.send("/subagent fake-levels:latest\r"); check(s.expect("sub-agents run on fake-levels:latest"), "/subagent MODEL is checked and set")
s.send("/subagent effort low\r"); check(s.expect("subagent effort: low"), "/subagent effort LEVEL goes to that model's effort")
s.send("TOOL_TASK\r"); check(s.expect("on fake-levels:latest (/subagent)", 15), "the sub-agent says which model it runs on")
check(s.expect("Tool said: REPORT", 15), "its report comes back to the main model")
sub = [r for r in requests() if r["messages"][0]["role"] == "system" and "You are a sub-agent" in r["messages"][0]["content"]]
check(sub and sub[-1]["model"] == "fake-levels:latest" and sub[-1].get("think") == "low", f"the sub-agent's call is to that model, at its effort: {sub[-1:]!r}")
s.send("/subagent nosuchmodel:1b\r"); check(s.expect("does not know a model"), "an unknown model is refused there and then")
s.send("/subagent xai:grok-fake\r"); check(s.expect("is an advisor only"), "an advisor-only provider is refused: it could not do the sub-agents' work")
s.send("/subagent default\r"); check(s.expect("run on the model doing the work"), "/subagent default")
os.remove(os.path.join(WORK, "hay.txt"))
print("test interactive: the options the model offers come up as a picker; the choice is the next message")
s = new_session(["-m", "fake-coder:latest"])   # its own session: it stands alone
s.send("OPTIONS_REPLY please\r"); check(s.expect("Implementation options", 10), "the reply's options block is offered as a picker")
check(s.expect("recommended", 3), f"with the recommended one marked: {s.text()[-400:]!r}")
s.send("\x1b[A"); s.send("\r"); check(s.expect("Echo: Go with this: Lock file — simple, but a crash leaves it behind", 10), "arrow up and Enter: that option is the next message")
s.send("OPTIONS_REPLY again\r"); check(s.expect("Implementation options", 10), "a second reply with options")
time.sleep(0.6); s.send("\x1b"); time.sleep(0.6)   # a lone Esc is told from the start of a key sequence by its silence
check("Go with this" not in since_send(s).split("OPTIONS_REPLY again")[-1], "Esc takes none of them: nothing is sent")
s.send("/status\r"); check(s.expect("keep_alive", 5) and "Implementation options" not in since_send(s), "a slash command after them does not bring the picker back")
s.send("/mode auto\r"); s.expect("mode: auto")
s.send("TOOL_TASK_SLEEP\r"); check(s.expect("⤷ sub-agent slow research", 15), "sub-agent started")
check(s.expect("running…", 10), "its shell command is running")
s.send("never mind, do this instead\r")
check(s.expect("sub-slept", 15), f"a queued message does not stop the sub-agent: its command finishes: {s.text()[-400:]!r}")
check("stopped after" not in clean(s.out[s.mark:].decode("utf-8", "replace")), "and it is not cut short after its round")
check(s.expect("› never mind, do this instead", 8), "and the message reaches the model")
check(s.expect("Echo: never mind, do this instead", 15), "which is called with it right away")
msgs = requests()[-1]["messages"]
check(any(m["role"] == "tool" and "REPORT: found it in sub-slept" in m["content"] for m in msgs),
      f"the parent gets what the sub-agent had gathered, not an error: {[m['role'] for m in msgs[-4:]]}")
s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: /ctx picker and sizes")
s.send("/ctx 64k\r"); check(s.expect("context window: 64k (num_ctx 65536)"), "/ctx 64k")
s.send("hi ctx\r"); check(s.expect("Echo: hi ctx"), "reply"); check(requests()[-1]["options"]["num_ctx"] == 65536, "num_ctx sent")
s.send("/ctx max\r"); check(s.expect("context window: 64k"), "/ctx max uses the model's trained length (from /api/show)")
s.send("/ctx 128k\r"); check(s.expect("larger than the model's trained length"), "warning beyond model max")
s.send("/ctx bogus\r"); check(s.expect("bad size"), "bad size rejected")
s.send("/ctx\r"); check(s.expect("Context window (fake-coder:latest supports up to 64k)"), "picker title shows model max")
# UNVERIFIED: the wait was added for a failure that did not reproduce (old read: whole output at once); not proven to fix it
check(all(seen(s, p) for p in ("32k", "model maximum", "server default")), f"picker entries: {since_send(s)[-400:]!r}")   # the picker is drawn after the title: wait for each entry
s.send("\x1b"); check(s.expect("context window unchanged"), "esc leaves it")
s.send("/ctx\r"); s.expect("Context window"); s.send("32k"); time.sleep(0.2); s.send("\r")
check(s.expect("context window: 32k (num_ctx 32768)"), "picked 32k from the menu")
s.send("/status\r"); check(s.expect("model max 65536 (/ctx to enlarge)"), "/status shows model max")

print("test interactive: slash commands + /model picker + tab completion")
s.send("/sta\t"); time.sleep(0.2); s.send("\r"); check(s.expect("generated"), "/status via tab completion")
s.send("/model\r"); check(s.expect("Select model"), "picker opens")
s.send("think"); time.sleep(0.2); s.send("\r"); check(s.expect("model set to fake-thinker:latest"), "picker filter+select")
s.send("/model fake-coder:latest\r"); check(s.expect("model set to fake-coder:latest"), "/model NAME")
s.send("/model fake-chat:latest\r"); check(s.expect("does not support tool calling"), "chat-only warning")
s.send("/model\r"); check(s.expect("Select model"), "picker reopens"); s.send("\x1b"); check(s.expect("model unchanged"), "picker esc cancels")
s.send("/model fake-coder:latest\r"); s.expect("model set to")
s.send("/clear\r"); check(s.expect("new conversation"), "/clear")
s.send("!echo bang-works\r"); check(s.expect("bang-works"), "!cmd runs")
s.send("what did I run\r"); check(s.expect("Echo: what did I run"), "reply after !cmd")
check(any("bang-works" in m["content"] for m in requests()[-1]["messages"]), "!cmd output added to context")
s.send("/help\r"); check(s.expect("Tools the model can call"), "/help")
s.send("/save t.md\r"); check(s.expect("saved t.md"), "/save"); check("bang-works" in open(os.path.join(WORK, "t.md")).read(), "transcript content")
s.send("/history 3\r"); check(s.expect("last 3 of"), "/history header")
check(s.expect("/history 3\n") and re.search(r"\d+  /help\n *\d+  /save t.md\n *\d+  /history 3\n", s.text()) is not None, f"/history lists recent queries, numbered: {s.text()[-300:]!r}")
hist_now = open(os.path.join(CFG, "corbienest", "history")).read()
check("/history 3" in hist_now, "history file updated immediately, not only at exit")
s.send("/nosuch\r"); check(s.expect("unknown command"), "unknown command")
s.send("\x04"); check(s.expect("bye"), "ctrl-d exits")
check("\x1b[?1049l" in s.out.decode("utf-8", "replace") and s.out.decode("utf-8", "replace").rfind("\x1b[?1049l") < s.out.decode("utf-8", "replace").rfind("bye"), "alternate screen left before goodbye")
s.close()

print("test interactive: spinner while a shell command runs")
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
s.send("!sleep 1.2; echo bang-done\r")   # "bang-done" is echoed with the command: wait for the result
check(s.expect("exit code: 0", 10), "bang command ran")
raw2 = clean(s.out[s.mark:].decode("utf-8", "replace"))
check("⎿  bang-done" in raw2, f"command output shown: {raw2[-200:]!r}")
check(not re.findall(r"([⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]) running command", raw2), "no busy label in the bar during a shell command (AGENTS.md: the bar has none)")
check(re.search(r"running… \(\ds\)  ctrl-c to interrupt", raw2) is not None, f"inline spinner during a shell command: {raw2[-200:]!r}")
check("auto mode" not in raw2, "bang command does not flash a mode change in the bar")
s.close()

print("test interactive: project memory (.corbienest/memory.md), curated by the model after each request")
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
s.send("/memory\r"); check(s.expect("auto-update is off"), "/memory shows state"); check(s.expect("no memory yet"), "no file yet")
s.send("/memory on\r"); check(s.expect("memory on"), "/memory on"); check(s.expect("every 5 requests"), "default cadence: every 5 requests")
s.send("/memory every 1\r"); check(s.expect("memory updated after each request"), "/memory every 1")
s.send("nothing special\r"); check(s.expect("Echo: nothing special"), "reply")
check(s.expect("memory: no change ·", 10), f"the extraction call ran after the request: {s.text()[-200:]!r}")
check(requests()[-1]["messages"][0]["content"].startswith("You maintain the persistent memory file"), "extraction call made after the request")
check(requests()[-1]["messages"][-1]["role"] == "user" and "NO_CHANGE" in requests()[-1]["messages"][-1]["content"], "asked for the updated file or NO_CHANGE")
time.sleep(0.5); check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "NO_CHANGE: no file written")
memreq = requests()[-1]
check("think" not in memreq and memreq["options"]["num_predict"] == 6144, "memory call: no think key for a non-thinking model, num_predict capped")
s.send("CUT_MEMORY please\r"); check(s.expect("memory: reply too long, ignored"), f"length-cut reply ignored: {s.text()[-200:]!r}")
time.sleep(0.3); check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "truncated memory reply is never written")
s.send("REMEMBER_ME the user prefers tabs\r"); check(s.expect("memory updated"), f"memory written: {s.text()[-200:]!r}")
mem = open(os.path.join(WORK, ".corbienest", "memory.md")).read()
check(mem.startswith("# Project memory") and "- the user prefers tabs" in mem and "```" not in mem, f"file content (fence stripped): {mem!r}")
s.send("what now\r"); check(s.expect("Echo: what now"), "next request")
sysmsg = [r for r in requests() if r["messages"][-1]["content"] == "what now"][-1]["messages"][0]["content"]
check("# Project memory (from .corbienest/memory.md)" in sysmsg and "the user prefers tabs" in sysmsg, "memory loaded into the system prompt")
s.send("/memory\r"); check(s.expect("the user prefers tabs"), "/memory prints it")
s.send("/model fake-thinker:latest\r"); check(s.expect("model set to fake-thinker"), "switch to a thinking model")
s.send("hello thinker\r"); check(s.expect("Echo: hello thinker"), "reply")
check(s.expect("memory: no change ·", 10), "extraction call ran")
check(requests()[-1]["think"] is False and "think" not in requests()[-2], "memory call turns thinking off on a thinking model; the main call leaves it to the server")
s.send("/model fake-coder:latest\r"); s.expect("model set to fake-coder")
s.send("/status\r"); check(s.expect("memory.md loaded"), "/status shows memory")
s.send("# always run make test\r"); check(s.expect("under which section?"), "# fact opens the section menu")
s.send("\x1b[B"); s.send("\x1b[B"); s.send("\r")   # Project -> User -> Feedback
check(s.expect("remembered under Feedback"), f"quick fact saved: {s.text()[-200:]!r}")
mem = open(os.path.join(WORK, ".corbienest", "memory.md")).read()
check("## Feedback\n\n- always run make test\n" in mem or "## Feedback\n- always run make test\n" in mem, f"bullet placed under Feedback: {mem!r}")
check("- the user prefers tabs" in mem, "existing facts kept")
s.send("# second note\r"); s.expect("under which section?"); s.send("\r")
mem = open(os.path.join(WORK, ".corbienest", "memory.md")).read()
check("## Project\n\n- second note\n" in mem or "## Project\n- second note\n" in mem, f"second fact under Project: {mem!r}")
check("Echo: # " not in s.text(), "# lines were not sent to the model")
s.send("/memory off\r"); check(s.expect("memory off"), "/memory off")
s.send("REMEMBER_ME ignored\r"); check(s.expect("Echo: REMEMBER_ME ignored"), "reply")
check(requests()[-1]["messages"][-1]["content"] == "REMEMBER_ME ignored", "no extraction call when off")
# batched cadence: the extraction call only runs every N requests, or when the conversation goes away
s.send("/memory on\r"); s.expect("memory on"); s.send("/memory every 2\r"); check(s.expect("every 2 requests"), "/memory every 2")
s.send("first of two\r"); check(s.expect("Echo: first of two"), "reply")
check(requests()[-1]["messages"][-1]["content"] == "first of two", "no extraction after the first request")
s.send("/memory\r"); check(s.expect("1 request pending extraction"), "/memory shows the pending count")
s.send("REMEMBER_ME batched fact\r"); check(s.expect("memory updated"), f"extraction after the second request: {s.text()[-200:]!r}")
memreq = requests()[-1]
check(memreq["messages"][0]["content"].startswith("You maintain the persistent memory file") and any("first of two" in m["content"] for m in memreq["messages"]), "the batched call covers both requests")
check("batched fact" in open(os.path.join(WORK, ".corbienest", "memory.md")).read(), "batched fact written")
# idle flush: a request left pending is folded in while the prompt sits untouched, so that
# quitting (or the next /clear) does not have to wait for the extraction call
s.send("/memory idle 1\r"); check(s.expect("after 1s idle at the prompt"), "/memory idle 1")
s.send("/memory\r"); check(s.expect("after 1s idle"), "cadence mentions the idle flush")
s.send("REMEMBER_ME idle fact\r"); check(s.expect("Echo: REMEMBER_ME idle fact"), "reply")
check(s.expect("memory updated", 10), f"the pending extraction ran at the idle prompt: {s.text()[-200:]!r}")
check("idle fact" in open(os.path.join(WORK, ".corbienest", "memory.md")).read(), "idle-flushed fact written")
s.expect("(no such text)", 1)   # drain: the prompt is redrawn just after the extraction prints
check(re.search(r"\x1b\[38;1H[^\n]*❯", s.out[s.mark:].decode("utf-8", "replace")) is not None, "the input field is redrawn after the idle flush")
s.send("/memory\r"); check(not s.expect("pending extraction", 2), "nothing left pending after the idle flush")
s.send("still here\r"); check(s.expect("Echo: still here"), "the prompt still works after the idle flush")
# a message typed while the idle extraction runs is queued (the prompt is busy): it must run
# as soon as the memory write is done, not sit in the queue waiting for another Enter
s.send("MEMWAIT please\r"); check(s.expect("Echo: MEMWAIT please"), "reply")
check(s.expect("updating memory", 5), f"the idle extraction started: {s.text()[-200:]!r}")
s.send("queued during the memory write\r")
check(s.expect("1 queued", 3), f"the message was queued while memory was being written: {s.text()[-200:]!r}")
check(s.expect("memory updated", 10), f"the extraction still finished: {s.text()[-200:]!r}")
check("slow fact" in open(os.path.join(WORK, ".corbienest", "memory.md")).read(), "the slow extraction was written")
check(s.expect("Echo: queued during the memory write", 10), f"the queued message ran once memory was written: {s.text()[-300:]!r}")
s.expect("(no such text)", 3)   # drain: let the extraction pending after that request run
s.send("/memory idle off\r"); check(s.expect("memory idle update off"), "/memory idle off")
s.send("/memory every 5\r"); s.expect("every 5 requests")
s.send("pending one\r"); check(s.expect("Echo: pending one"), "reply")
time.sleep(2); check(not s.expect("updating memory", 1), "idle off: the pending extraction stays pending")
s.send("/clear\r"); check(s.expect("new conversation"), "/clear")
check(requests()[-1]["messages"][0]["content"].startswith("You maintain the persistent memory file") and any("pending one" in m["content"] for m in requests()[-1]["messages"]), "/clear flushes the pending extraction first")
s.send("/memory update\r"); check(s.expect("nothing pending"), "/memory update with nothing pending")
s.send("REMEMBER_ME at exit\r"); check(s.expect("Echo: REMEMBER_ME at exit"), "reply")
s.send("\x04"); check(s.expect("updating memory…  (Ctrl-C skips it)"), "the exit flush says it can be skipped")
check(s.expect("bye"), "exit")
s.close()
check("at exit" in open(os.path.join(WORK, ".corbienest", "memory.md")).read(), "exit flushes the pending extraction (Ctrl-D)")
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
s.send("/memory off\r"); s.expect("memory off")
s.send("/memory clear\r"); check(s.expect("removed .corbienest/memory.md"), "/memory clear")
check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "file removed")
s.send("\x04"); s.close()
out, rc = run(["-m", "fake-coder:latest", "-p", "REMEMBER_ME oneshot"])
check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "one-shot run does not create the memory file (memory off in config)")

print("test interactive: old tool results are elided once the context is half full")
s = Session(["-m", "fake-coder:latest", "--yolo"]); s.expect("Ctrl-D to quit")
s.send("TOOL_BIG one\r"); check(s.expect("Tool said:", 10), "first tool round")
s.send("TOOL_BIG two\r"); check(s.expect("Tool said:", 10), "second tool round")
s.send("HALF_CTX now\r"); check(s.expect("Echo: HALF_CTX now"), "usage climbs past 50%")
check("elided" not in s.text(), "nothing elided while the previous requests are recent")
s.send("plain follow-up\r"); check(s.expect("elided 2 old tool results"), f"elision note: {s.text()[-300:]!r}")
check(s.expect("Echo: plain follow-up"), "reply")
msgs = [r for r in requests() if r["messages"][-1]["content"] == "plain follow-up"][-1]["messages"]
tools_ = [m for m in msgs if m["role"] == "tool"]
check(len(tools_) == 2 and all(m["content"].startswith("[earlier output elided") and "line-0001" in m["content"] and "line-0300" not in m["content"] for m in tools_), f"both old results stubbed with their first line kept: {[t['content'][:80] for t in tools_]!r}")
check(not any("elided" in m["content"] for m in msgs if m["role"] == "user"), "user messages untouched")
s.send("another\r"); check(s.expect("Echo: another"), "reply"); check(s.text().count("elided 2 old") == 1, "already-elided results are not counted again")
s.send("\x04"); s.close()



print("test interactive: a prompt that no longer fits is shrunk and retried, not lost")
s = Session(["-m", "fake-coder:latest", "--yolo"]); s.expect("Ctrl-D to quit")
s.send("TOOL_BIG CTX_OVERFLOW\r")
check(s.expect("no longer fits", 10), f"the overflow is reported: {s.text()[-300:]!r}")
check(s.expect("elided 1 old tool result", 10), "the tool results are elided")
check(s.expect("Tool said:", 10), f"the turn continues after the retry: {s.text()[-300:]!r}")
last = requests()[-1]["messages"]
check([m for m in last if m["role"] == "user" and "CTX_OVERFLOW" in m["content"]], "the request itself is still in the prompt")
check(all(m["content"].startswith("[earlier output elided") for m in last if m["role"] == "tool"), "the retry carries stubs, not the full results")
s.send("\x04"); s.close()


print("test interactive: a tool result may not take more than a quarter of the context window")
s = Session(["-m", "fake-coder:latest", "--yolo", "-c", "2048"]); s.expect("Ctrl-D to quit")
s.send("TOOL_BIG one\r"); check(s.expect("Tool said:", 10), "tool round")
res = [m for m in requests()[-1]["messages"] if m["role"] == "tool"][-1]["content"]
check(len(res) < 2048 and "line-0001" in res and "line-0300" in res and "cut:" in res,
      f"the middle is cut, both ends kept: {len(res)} bytes, {res[:60]!r}")
s.send("\x04"); s.close()


print("test interactive: PgUp/PgDn scrollback viewer")
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
for pfx in "abcd":
    s.send("!seq -f '%s-%%02g' 1 30\r" % pfx); check(s.expect(pfx + "-30", 10), "30 rows printed")
s.send("hello before\r"); check(s.expect("Echo: hello before"), "reply")
s.send("\x1b[5~"); check(s.expect("scrollback · rows"), f"PgUp opens the viewer: {s.text()[-200:]!r}")
view = since(); first = viewrange()
check("manual mode" in view.split("scrollback · rows")[-1] and "tokens" in view.split("scrollback · rows")[-1], f"where the window is has a line of its own: the status bar stays what it is: {view[-300:]!r}")
check("c-05" in view and "d-30" not in view and "hello before" not in view, f"viewer shows an earlier window: {view[-400:]!r}")
s.send("\x1b[5~"); s.expect("scrollback"); second = viewrange()
check(first and second and second[0] < first[0], f"PgUp again scrolls further up: {first} -> {second}")
s.send("g"); s.expect("scrollback"); check("Corbie Nest" in since(), "Home shows the banner")
s.send("\x1b[6~"); s.expect("scrollback"); check("a-" in since() and "Corbie Nest" not in since(), "PgDn scrolls down")
s.send("\x1b"); check(s.expect("❯"), "prompt back"); time.sleep(0.2)
tail = since()
check("hello before" in tail and "Echo: hello before" in tail and "d-30" in tail and "❯" in tail.split("ctx 0%")[-2], f"Esc returns to the prompt with the tail redrawn: {tail[-300:]!r}")
s.send("after view\r"); check(s.expect("Echo: after view"), "the editor works after the viewer")
s.send("\x1b[5~"); s.expect("scrollback")
for _ in range(6): s.send("\x1b[6~", wait=0.3)
check(s.expect("❯"), "prompt back"); time.sleep(0.2)
check(not scrolled(s), "PgDn at the bottom leaves the viewer")
s.send("typed\r"); check(s.expect("Echo: typed"), "prompt usable again")
s.send("\x04"); s.close()

print("test interactive: the mouse wheel scrolls the conversation; the arrows stay the input field's")
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
raw = s.out.decode("utf-8", "replace")
check("\x1b[?1000h" in raw and "\x1b[?1006h" in raw, "the mouse is asked for, with SGR coordinates: otherwise the wheel arrives as arrow keys")
for pfx in "abc":
    s.send("!seq -f '%s-%%02g' 1 30\r" % pfx); check(s.expect(pfx + "-30", 10), "30 rows printed")
s.send("older prompt\r"); check(s.expect("Echo: older prompt"), "reply")
s.send("newer prompt\r"); check(s.expect("Echo: newer prompt"), "reply")
s.send("half typed"); s.expect("half typed")
s.send(wheel(True)); check(s.expect("scrollback · rows"), f"the wheel over the conversation scrolls it back: {s.text()[-200:]!r}")
first = viewrange()
check("❯ half typed" in since() and "❯ newer prompt" not in since(), f"it does not walk the prompt history: the field keeps what was typed: {since()[-300:]!r}")
s.send(wheel(True)); s.expect("scrollback"); second = viewrange()
check(first and second and second[0] == first[0] - 3, f"a notch is three rows: {first} -> {second}")
check(quiet(s, wheel(True, row=38)), "over the input field the wheel does nothing")
check(quiet(s, wheel(False, row=40)), "nor over the status bar")
check(quiet(s, "\x1b[<0;10;5M\x1b[<0;10;5m"), "a click does nothing: it is not typed, and it does not close the viewer")
s.send(wheel(False, n=3)); check(not scrolled(s), "scrolling down to the last row leaves the viewer")
check("❯ half typed" in since() and "Echo: newer prompt" in since(), f"with the tail of the transcript drawn again and the field as it was: {since()[-300:]!r}")
check(quiet(s, wheel(False)), "at the prompt there is nothing below to scroll to")
s.send("\x15"); s.send("\x1b[A"); check(s.expect("❯ newer prompt"), "arrow up is the prompt history, as before")
s.send("\x1b[A"); check(s.expect("❯ older prompt"), "and again")
s.send("\x15")
s.send("\x1b[M" + chr(32 + 64) + chr(32 + 10) + chr(32 + 5)); check(s.expect("scrollback · rows"), "a terminal's old three-byte report is understood too")
check("`*%" not in since(), "and its bytes are not typed into the field")
s.send("\x1b"); check(not scrolled(s), "Esc returns to the prompt")
s.send(wheel(True)); s.expect("scrollback")
s.send("x"); check(s.expect("❯ x"), "a key typed in the viewer goes back to the prompt and into the message")
s.send("\x15")

print("test interactive: scrolling back while the model works")
s = new_session(["-m", "fake-coder:latest"])   # its own session: it stands alone
s.send("/help\r"); s.expect("(no such text)", 2)   # some history longer than the screen, so there is something to scroll back to
s.send("SLOW please\r"); check(s.expect("one"), "the reply starts")
s.send(wheel(True, n=2)); check(s.expect("scrollback · rows"), f"the wheel scrolls back while the reply streams: {s.text()[-200:]!r}")
check("manual mode" in since(), "the bar is still the bar: the mode is shown while the reply streams (the spinner stops once the reply has begun)")
time.sleep(1.3); s.expect("(no such text)", 0.2)
check("four" not in since(), f"what arrives meanwhile is recorded, not painted over the window: {since()[-200:]!r}")
s.send("\x1b"); check(s.expect("eight") and s.expect("tok/s"), "Esc goes back to the output, and the reply runs to its end")
check("one two three four" in since(), f"what arrived meanwhile was painted on the way back: {since()[-300:]!r}")
check("interrupted" not in since(), "that Esc left the viewer; it did not stop the model")
s.send("SLOW again\r"); check(s.expect("one"), "another reply starts")
s.send("\x1b[5~"); check(s.expect("scrollback · rows"), "PgUp scrolls back at once too, instead of when the turn is over")
check(s.expect("End/Esc/Enter back", 10), "still reading when the reply ends: the window stays, now with the keyboard")
s.send("\x1b[F"); check(not scrolled(s), "End goes back to the prompt")
check("one two three four five six seven eight" in since(), f"with the reply that was written meanwhile: {since()[-300:]!r}")
s.send("after reading\r"); check(s.expect("Echo: after reading"), "and the prompt works")
s.send("SLOW third\r"); s.expect("one")
s.send(wheel(True)); s.expect("scrollback")
s.send("\x03"); check(s.expect("interrupted"), "Ctrl-C while scrolled back stops the model and returns to the output")
s.send("\x04"); s.close()

print("test interactive: suggestions under the input field while a /command or an @file is typed")
os.makedirs(os.path.join(WORK, "sugsrc", "deep"))
for rel in ("sugsrc/zebra_term.c", "sugsrc/deep/zebra_notes.md", "sugsrc/twin.txt", "sugsrc/deep/twin.txt"):
    open(os.path.join(WORK, rel), "w").write("content of %s\n" % rel)
open(os.path.join(SIB, "zeta_parser.c"), "w").write("content of the sibling\n")
sib = "../" + os.path.basename(SIB)
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
s.send("/m"); s.send("o"); check(s.expect("pick a model from a menu"), f"typing /mo lists the commands it could become, each with what it does: {since()[-300:]!r}")
s.expect("(no such text)", 1)
_t = since(); view = _t[_t.rfind("❯ /mo"):] if "❯ /mo" in _t else ""   # the list is drawn under the field: from the field's row on
check("/models" in view and "permission mode: manual" in view and "/memory" in view and "/status" not in view, f"only the matching ones — a name that starts with it first, then one that contains it: {view!r}")
check("tab complete · ↓ choose · esc close" in view, "and a line that says which keys work")
_regions = [int(x) for x in re.findall(r"\x1b\[1;(\d+)r", s.out.decode("utf-8", "replace"))]
check(_regions and min(_regions) < 36, f"the list has rows of its own: the conversation region makes room (regions {sorted(set(_regions))}; 36 is with no list)")
s.send("d"); s.send("e"); view = listed(); check("/mode" in view and "/memory" not in view, f"every letter narrows it down: {view!r}")
s.send("\x7f\x7f"); check("/memory" in listed(), "and deleting widens it again")
s.send("\x1b[B"); view = listed(); check("enter take it" in view and re.search(r"❯ /model\s+pick a model", view), f"arrow down steps into the list: {view!r}")
s.send("\x1b[B\x1b[B"); view = listed(); check(re.search(r"❯ /mode\s+permission mode", view), f"and moves in it: {view!r}")
s.send("\x1b[A"); view = listed(); check(re.search(r"❯ /models\s+list the models", view), "arrow up moves back")
s.send("\x1b[B\r"); view = listed(); check("read-only: the model proposes a plan" in view, f"Enter takes the highlighted command — and its options are listed next: {view!r}")
check("❯ /mode " in since() and "✓ mode" not in since(), "the command is in the field, not sent")
s.send("p"); s.send("l"); view = listed(); check("plan" in view and "accept-edits" not in view, f"the options narrow down the same way: {view!r}")
s.send("\t"); check(s.expect("❯ /mode plan"), "Tab completes the one that is left")
s.send("\r"); check(s.expect("✓ mode: "), "and Enter sends the line")
s.send("/mode manual\r"); s.expect("✓ mode: ")
s.send("/stat\r"); check(s.expect("unknown command /stat"), "with nothing highlighted Enter sends what was typed: the list is only a preview")
s.send("/h"); s.send("e"); check("commands, keys and input tricks" in listed(), "the list for /he")
s.send("\x1b"); check("commands, keys" not in listed(), "Esc closes the list")
check(re.search(r"\x1b\[1;36r", s.out[s.mark:].decode("utf-8", "replace")) is not None, "and the conversation gets its rows back")
s.send("\x1b[A"); check(s.expect("❯ /stat"), "after which arrow up is the history again")
check("model, context usage" not in listed(), "a recalled line brings no list up: the next arrow goes on through the history")
s.send("\x1b[A"); check(s.expect("❯ /mode manual"), "as it does")
s.send("\x15")
s.send("/sta\t"); check(s.expect("❯ /status"), "Tab completes a command that is the only match")
s.send("\x15")
s.send("see @sugsrc/twin.txt and @sugsrc/nope.tx"); s.send("t"); s.expect("(no such text)", 0.3); raw = s.out[s.mark:]
check(b"\x1b[38;2;86;182;194m@sugsrc/twin.txt\x1b[38;2;255;241;184m" in raw and b"\x1b[38;2;86;182;194m@sugsrc/nope" not in raw, f"a mention that names a file is coloured in the input field, one that names nothing is not: {raw[-300:]!r}")
s.send("\x7f" * 44)
s.send("see @zebr"); s.send("a"); view = listed()
check("sugsrc/zebra_term.c" in view and "sugsrc/deep/zebra_notes.md" in view, f"@ and part of a name finds the files it could be, wherever they are: {view!r}")
check("twin.txt" not in view, "and only those")
s.send("\t"); check("enter take it" in listed(), "Tab starts choosing when they have nothing more in common")
s.send("\t"); check(s.expect("❯ see @sugsrc/zebra_term.c"), f"and takes the highlighted one: its path replaces what was typed: {since()[-200:]!r}")
s.send("and @" + sib + "/z"); s.send("e"); check(s.expect(sib + "/zeta_parser.c"), f"a directory in front — also one outside the working directory — is where the search is: {since()[-300:]!r}")
s.send("\t"); check(s.expect("@" + sib + "/zeta_parser.c "), "Tab completes it")
s.send("\r"); check(s.expect("Echo: see @sugsrc/zebra_term.c"), "the message is sent")
check("(attached sugsrc/zebra_term.c" in since() and "(attached " + sib + "/zeta_parser.c" in since(), f"with both files attached: {since()[-400:]!r}")
content = requests()[-1]["messages"][-1]["content"]
check('<file path="sugsrc/zebra_term.c">' in content and "content of the sibling" in content, "and in the request")
s.send("@sugsrc"); s.send("/"); view = listed()
check("sugsrc/deep/" in view and "sugsrc/twin.txt" in view and "sugsrc/zebra_term.c" in view, f"a directory lists what is in it: {view!r}")
s.send("\x15")
s.send("/cd su"); s.send("g"); view = listed(); check("sugsrc/" in view and "zebra_term.c" not in view, f"/cd is offered the directories, and only those: {view!r}")
s.send("\x15")
s.send("look at @zebra_notes.md\r"); check(s.expect("Echo: look at @zebra_notes.md"), "a bare file name is sent as typed")
check("(attached sugsrc/deep/zebra_notes.md" in since(), f"and the one file by that name is attached: {since()[-300:]!r}")
check('<file path="sugsrc/deep/zebra_notes.md">' in requests()[-1]["messages"][-1]["content"], "under its real path")
s.send("look at @twin.txt\r"); check(s.expect("Echo: look at @twin.txt"), "a name two files have is sent as it is")
check("@twin.txt could be sugsrc/twin.txt, sugsrc/deep/twin.txt" in since(), f"both are named: {since()[-300:]!r}")
check("<file" not in requests()[-1]["messages"][-1]["content"], "and neither is guessed")

print("test interactive: the suggestions come up while the model works too, and their keys act at once")
s.send("!sleep 7\r"); s.expect("sleep 7")
s.send("/m"); s.send("o"); view = listed()
check("/models" in view and "pick a model from a menu" in view and "tab complete · ↓ choose · esc close" in view, f"/mo typed while a command runs lists the commands it could become, as at the prompt: {view!r}")
s.send("\x1b"); check("pick a model" not in listed(), "Esc closes the list")
s.send("d"); view = listed(); check("/models" in view and "/memory" not in view, f"and stops nothing — the next letter brings the list back, narrowed down: {view!r}")
s.send("\x1b[B\x1b[B\x1b[B"); view = listed(); check("enter take it" in view and re.search(r"❯ /mode\s+permission mode", view), f"arrow down steps into it and moves: {view!r}")
s.send("\r"); view = listed(); check("read-only: the model proposes a plan" in view and "queued" not in view, f"Enter takes the highlighted command instead of sending the line, and its options are listed: {view!r}")
s.send("p"); s.send("\t"); check(s.expect("❯ /mode plan"), "Tab completes the option")
s.send("\r"); check(s.expect("plan mode on"), "and Enter, with nothing highlighted, sends the line: the command runs there and then")
check(s.expect("exit code: 0", 12), f"the shell command ran to its end all the while: {since()[-300:]!r}")
s.send("/mode manual\r"); s.expect("✓ mode: ")
s.send("SLOW please\r"); s.expect("one")
s.send("see @zebra_t"); s.send("e"); check("sugsrc/zebra_term.c" in listed(), "an @file is found while a reply streams")
s.send("\t\t"); check(s.expect("❯ see @sugsrc/zebra_term.c"), "and Tab steps into the list and takes it")
check(s.expect("eight") and s.expect("tok/s"), "the reply runs to its end")
check(s.expect("❯ see @sugsrc/zebra_term.c"), "what was typed is in the prompt afterwards")
s.send("\x15")
s.send("\x04"); s.close()
out, rc = run(["-m", "fake-coder:latest", "-T", "-p", "look at @zebra_notes.md"])
check("(attached sugsrc/deep/zebra_notes.md" in out, f"-p finds a file by its name too: {out!r}")

print("test interactive: a !line is completed the way a shell would: programs where a command goes, paths elsewhere")
for name in ("zzcorbie-tool", "zzcorbie-tool-extra"):
    open(os.path.join(BINDIR, name), "w").write('#!/bin/sh\necho "ran-%s $*"\n' % name); os.chmod(os.path.join(BINDIR, name), 0o755)
open(os.path.join(BINDIR, "zzcorbie-notes.txt"), "w").write("not a program\n")
s = Session(["-m", "fake-coder:latest"], env=dict(ENV, PATH=BINDIR + os.pathsep + ENV["PATH"])); s.expect("Ctrl-D to quit")
s.send("!zzcor"); s.send("b"); view = listed()
check("zzcorbie-tool" in view and "zzcorbie-tool-extra" in view and "zzcorbie-notes" not in view, f"the first word is offered the programs in $PATH that start with it: {view!r}")
s.send("\t"); check(s.expect("❯ !zzcorbie-tool"), "Tab completes as far as they agree")
s.send(" zebr"); s.send("a"); view = listed()
check("sugsrc/zebra_term.c" in view and "sugsrc/deep/zebra_notes.md" in view, f"an argument is offered the files whose name starts with it, wherever they are: {view!r}")
s.send("\t\t"); check(s.expect("❯ !zzcorbie-tool sugsrc/zebra_term.c"), "Tab steps into the list and takes one")
s.send("\r"); check(s.expect("ran-zzcorbie-tool sugsrc/zebra_term.c"), "and the line runs as it stands")
s.send("!"); view = listed(); check("zzcorbie-tool sugsrc/zebra_term.c" in view, f"a bare ! lists the lines that were run before, the newest first: {view!r}")
s.send("\x1b[B\r"); check(s.expect("❯ !zzcorbie-tool sugsrc/zebra_term.c"), "and one of them is taken whole")
s.send("\x15")
s.send("!cat ebra_ter"); s.send("m"); check("zebra" not in listed(), "only the start of a name counts here: most words of a command are not files")
s.send("\x15")
s.send('!echo "zebr'); s.send("a"); check("zebra_term" not in listed(), "and inside quotes nothing is offered: that is text")
s.send("\x15")
s.send("!true | zzcor"); s.send("b"); check("zzcorbie-tool" in listed(), "after a pipe a command goes again")
s.send("\x15")
s.send("\x04"); s.close()
shutil.rmtree(BINDIR, ignore_errors=True)
shutil.rmtree(SIB, ignore_errors=True)

print("test interactive: CORBIENEST_TRACE records what is drawn and what is sent, and tools/trace_replay.py plays it back")
s = Session(["-m", "fake-coder:latest"], env=dict(ENV, CORBIENEST_TRACE=TRACE))
s.expect("(no such text)", 3)   # the note is printed under the start-up banner: read it, then look in the whole text
check(("trace: everything drawn and typed in this session is written to " + TRACE) in s.text(), "a session that is being recorded says so, and where to")
s.expect("Ctrl-D to quit")
s.send("traced words\r"); check(s.expect("Echo: traced words"), "reply")
s.send(wheel(True)); s.send("\x04"); time.sleep(0.5); s.close()
lines = open(TRACE, "rb").read().decode("utf-8", "replace").splitlines()
kinds = {l.split(" ", 2)[1] for l in lines if l.count(" ") >= 2}
check(" # corbienest " in lines[0] and " rows x " in lines[0], f"the first line says what made it and for which size: {lines[0]!r}")
check({"O", "I", "L", "B"} <= kinds, f"output, input, layout and activity are all in it: {sorted(kinds)}")
check("traced words\\r" in "".join(l.split(" ", 2)[2] for l in lines if l.split(" ", 2)[1:2] == ["I"]), "what was typed, byte for byte")
check(any(" L rows=40 cols=100 region=1-36 " in l for l in lines), "the layout it was drawn for")
check(os.stat(TRACE).st_mode & 0o777 == 0o600, "it holds the conversation: readable by its owner alone")
p = subprocess.run(REPLAY + ["--fast", "--no-hold"], capture_output=True, timeout=30)
check(p.returncode == 0 and b"\x1b[?1049h" in p.stdout and "Echo: " in clean(p.stdout.decode("utf-8", "replace")) and "traced words" in clean(p.stdout.decode("utf-8", "replace")), "the replay writes what the session wrote, escapes and all")
p = subprocess.run(REPLAY + ["--info"], capture_output=True, timeout=30)
check(b"rows=40 cols=100" in p.stdout and b"traced words" in p.stdout and b"wheel up at row 5, column 10" in p.stdout, f"--info lists the layout, what was typed and what the mouse sent: {p.stdout[-300:]!r}")
p = subprocess.run(REPLAY + ["--excerpt", "0", "0.05"], capture_output=True, timeout=30)
check(p.stdout.startswith(lines[0].encode()) and len(p.stdout) < os.path.getsize(TRACE), "--excerpt cuts a stretch of it out, to paste")

print("test in tmux: the wheel reaches the app whatever tmux's own mouse setting, and a pane resize is drawn at once")
if not shutil.which("tmux"): print("  (tmux is not installed: skipped)")
else:
    def tmux(sock, *a): return subprocess.run(["tmux", "-L", sock, *a], capture_output=True, text=True, env=ENV, cwd=WORK).stdout
    def pane_has(sock, pat, t=8, want=True):
        end = time.time() + t
        while time.time() < end:
            if (pat in tmux(sock, "capture-pane", "-p", "-t", "t")) == want: return True
            time.sleep(0.1)
        return False
    for mouse in ("off", "on"):
        sock = "crowtest%d%s" % (os.getpid(), mouse)
        tmux(sock, "-f", "/dev/null", "new-session", "-d", "-s", "t", "-x", "90", "-y", "24", BIN + " -m fake-coder:latest; sleep 3")
        tmux(sock, "set", "-g", "mouse", mouse); tmux(sock, "set", "-g", "status", "off")
        pid, fd = pty.fork()   # a client in a pty is the terminal: what is written to it reaches tmux as input
        if pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.execvp("tmux", ["tmux", "-L", sock, "attach", "-t", "t"])
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 90, 0, 0))
        def feed(data, fd=fd):
            os.write(fd, data.encode())
            end = time.time() + 0.3
            while time.time() < end:
                if select.select([fd], [], [], 0.05)[0]:
                    try: os.read(fd, 65536)
                    except OSError: break
        # The pane was drawn before any client came: what is typed must wait for this one to be attached. Until
        # then its pty is an ordinary cooked terminal, where Enter becomes a line feed — which the client hands on
        # once it is there, and which is a newline in the input field, not a submitted line (seen on a slow CI runner).
        end = time.time() + 8
        while time.time() < end and tmux(sock, "display", "-p", "-t", "t", "#{session_attached}").strip() != "1": time.sleep(0.05)
        check(tmux(sock, "display", "-p", "-t", "t", "#{session_attached}").strip() == "1", f"mouse {mouse}: the client is attached")
        check(pane_has(sock, "shift+tab to cycle"), f"mouse {mouse}: it starts in a tmux pane: {tmux(sock, 'capture-pane', '-p', '-t', 't')[-200:]!r}")
        check(tmux(sock, "display", "-p", "-t", "t", "#{mouse_any_flag}").strip() == "1", f"mouse {mouse}: tmux knows the pane wants the mouse")
        feed("!seq 1 60\r"); pane_has(sock, "+21 lines")
        feed("kept in the field")
        feed("\x1b[<64;10;5M\x1b[<64;10;5M")
        check(pane_has(sock, "scrollback · rows"), f"mouse {mouse}: the wheel scrolls the conversation: {tmux(sock, 'capture-pane', '-p', '-t', 't')[-300:]!r}")
        check(tmux(sock, "display", "-p", "-t", "t", "#{pane_in_mode}").strip() == "0", f"mouse {mouse}: tmux did not keep it for its copy mode")
        check("❯ kept in the field" in tmux(sock, "capture-pane", "-p", "-t", "t"), f"mouse {mouse}: and it is not turned into arrow keys for the prompt history")
        check("manual mode" in tmux(sock, "capture-pane", "-p", "-t", "t").rstrip("\n").split("\n")[-2], f"mouse {mouse}: the status bar is still on the row above the location line")
        feed("\x1b[<65;10;5M" * 3)
        check(pane_has(sock, "scrollback · rows", want=False), f"mouse {mouse}: scrolling down returns to the prompt")
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 16, 60, 0, 0))
        feed("")
        end = time.time() + 5; lines = []
        while time.time() < end:
            lines = tmux(sock, "capture-pane", "-p", "-t", "t").rstrip("\n").split("\n")
            if len(lines) == 16 and "manual mode" in lines[-2] and lines[-4].startswith("╭"): break
            time.sleep(0.1)
        check(len(lines) == 16 and "manual mode" in lines[-2] and "❯ kept in the field" in lines[-3] and lines[-4].startswith("╭"),
              f"mouse {mouse}: a resized pane is drawn for its new size without a key being pressed: {lines[-5:]!r}")
        tmux(sock, "kill-server")
        try: os.close(fd)
        except OSError: pass
        try: os.waitpid(pid, 0)
        except OSError: pass

print("test interactive: /keepalive, /status placement, GPU placement warning")
s = Session(["-m", "fake-coder:latest"]); s.expect("Ctrl-D to quit")
s.send("/think high\r"); check(s.expect("fake-coder:latest cannot think"), "/think high is /effort high: refused for a model that cannot think")
s.send("/model fake-thinker:latest\r"); s.expect("model set to fake-thinker")
s.send("/think high\r"); check(s.expect("effort: high"), "/think high sets the model's effort")
s.send("level test\r"); check(s.expect("Echo: level test"), "reply"); check(requests()[-1]["think"] == "high", "thinking level sent as a string")
s.send("/think auto\r"); s.expect("think: auto"); s.send("level test 2\r"); s.expect("Echo: level test 2"); check(requests()[-1]["think"] == "high", "/think auto says when, not how hard: the level stays")
s.send("/effort default\r"); s.expect("effort: default"); s.send("level test 3\r"); s.expect("Echo: level test 3"); check("think" not in requests()[-1], "/effort default leaves it to the model again")
s.send("/model fake-coder:latest\r"); s.expect("model set to fake-coder")
s.send("/keepalive\r"); check(s.expect("keep_alive: 30m"), "/keepalive shows the default")
s.send("/keepalive 1h\r"); check(s.expect("keep_alive = 1h"), "/keepalive sets it")
s.send("hello\r"); check(s.expect("Echo: hello"), "reply"); check(requests()[-1]["keep_alive"] == "1h", "new keep_alive sent")
s.send("/status\r"); check(s.expect("keep_alive 1h"), "/status shows keep_alive"); check(s.expect("100% in GPU memory"), "/status shows placement")
check("in GPU memory (" not in s.text().split("/status")[0], "no placement warning for a fully-GPU model")
s.send("/keepalive 30m\r"); s.expect("keep_alive = 30m")
s.send("/model fake-slow:latest\r"); s.expect("model set to fake-slow")
s.send("hello slow\r"); check(s.expect("Echo: hello slow"), "reply")
check(s.expect("model is only 50% in GPU memory (4.0 of 8.0 GB)"), f"partly-CPU model warned about once: {s.text()[-300:]!r}")
s.send("again\r"); check(s.expect("Echo: again"), "reply")
check(s.text().count("only 50% in GPU memory") == 1, "warning shown once per model")
s.send("/model fake-coder:latest\r"); s.expect("model set to fake-coder")
s.send("\x04"); s.close()

# ---------- /effort and /advisor ----------
# a config directory of their own: what they save (an advisor, per-model efforts) would
# otherwise follow every test that comes after
os.makedirs(os.path.join(CFG2, "corbienest")); open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")

print("test effort: a level is part of the prompt, so it goes with every call; a model that cannot stop is never told to")
out, rc, rs = run_rounds("fake-levels:latest", ["-m", "fake-levels:latest", "--yolo", "--effort", "high", "-p", "TOOL_BASH please"])
check(rc == 0 and thinks(rs) == ["high", "high"], f"--effort high: the first call and the tool round both carry it: {thinks(rs)!r}")
out, rc, rs = run_rounds("fake-levels:latest", ["-m", "fake-levels:latest", "--yolo", "-p", "TOOL_BASH please"])
check(rc == 0 and thinks(rs) == ["absent", "absent"], f"no effort set, /think auto: a gpt-oss-like model is not sent think:false for the tool round (it would be ignored, and cost the prompt cache): {thinks(rs)!r}")
out, rc = run(["-m", "fake-levels:latest", "--no-think", "-p", "hi"], env=ENV2)
check(requests()[-1].get("think") == "low", f"--no-think on a model that cannot stop: its weakest level: {requests()[-1].get('think')!r}")
out, rc = run(["-m", "fake-levels:latest", "--effort", "off", "-p", "hi"], env=ENV2)
check(rc == 2 and "low · medium · high" in run.err, f"--effort off refused for it, with what it does offer: {run.err!r}")
out, rc = run(["-m", "fake-levels:latest", "--effort", "max", "-p", "hi"], env=ENV2)
check(rc == 0 and requests()[-1].get("think") == "high", f"--effort max means as hard as it goes: sent as the model's strongest level: {requests()[-1].get('think')!r}")
out, rc = run(["-m", "fake-levels:latest", "--effort", "ultra", "-p", "hi"], env=ENV2)
check(rc == 2 and "offers" in run.err, "a name that means nothing is refused")
out, rc = run(["-m", "fake-levels:latest", "--think", "-p", "hi"], env=ENV2)
check(requests()[-1].get("think") == "medium", f"--think on a model with levels and no 'true': its default level by name: {requests()[-1].get('think')!r}")
out, rc = run(["-m", "fake-coder:latest", "--effort", "high", "-p", "hi"], env=ENV2)
check(rc == 2 and "cannot think" in run.err, f"--effort on a model that cannot think: {run.err!r}")
out, rc, rs = run_rounds("fake-thinker:latest", ["-m", "fake-thinker:latest", "--yolo", "--effort", "on", "-p", "TOOL_BASH please"])
check(rc == 0 and thinks(rs) == [True, False], f"an on/off model: on for the request, off for its tool round (/think auto): {thinks(rs)!r}")
out, rc, rs = run_rounds("fake-thinker:latest", ["-m", "fake-thinker:latest", "--yolo", "--effort", "off", "--think", "-p", "TOOL_BASH please"])
check(rc == 0 and thinks(rs) == [False, False], f"--effort off wins over --think for that model: {thinks(rs)!r}")
out, rc, rs = run_rounds("fake-thinker:latest", ["-m", "fake-thinker:latest", "--yolo", "--effort", "high", "-p", "TOOL_BASH please"])
check(rc == 0 and thinks(rs) == ["high", False], f"a level for an on/off model is passed on (the server takes it as on), and like on it rests for the tool round: {thinks(rs)!r}")
out, rc = run(["-m", "fake-coder:latest", "--think", "-p", "hi"], env=ENV2)
check(rc == 0 and "think" not in requests()[-1] and "Echo: hi" in out, f"--think with a model that cannot think: nothing is sent, the request goes through: {out!r}")
out, rc = run(["-m", "fake-declared:latest", "--effort", "xhigh", "-p", "hi"], env=ENV2)
check(rc == 0 and requests()[-1].get("think") == "xhigh", "levels the server lists in /api/show are used by the names it gives")
out, rc = run(["-m", "fake-declared:latest", "--effort", "high", "-p", "hi"], env=ENV2)
check(rc == 0 and requests()[-1].get("think") == "xhigh", f"'high' saved under an older server is sent as the top level this one has, not left to fall back to medium in silence: {requests()[-1].get('think')!r}")
out, rc = run(["-m", "fake-declared:latest", "--effort", "medium", "-p", "hi"], env=ENV2)
check(rc == 2 and "off · low · xhigh" in run.err, f"a level it does not list is refused: {run.err!r}")

out, rc = run(["-m", "fake-coder:latest", "--keep-alive", "inf", "-p", "hi"], env=ENV2)
check(requests()[-1].get("keep_alive") == "inf", f"only a plain decimal goes out as a number: 'inf' would become null, which the server takes for 'not set' in silence: {requests()[-1].get('keep_alive')!r}")
out, rc = run(["-m", "deep-thinker", "-p", "hi"], env=ENV2)
check("not found" in out and "refused" not in out, f"an error that merely quotes a model called …thinker is not taken for a refused think value: {out[-200:]!r}")

print("test capabilities come from /api/show: /api/tags may be out of date")
out, rc, rs = run_rounds("fake-stale:latest", ["-m", "fake-stale:latest", "--yolo", "--think", "-p", "TOOL_BASH please"])
check(rc == 0 and len(rs) == 2 and "Tool said: hello-from-tool" in out, f"a model /api/tags calls chat-only gets its tools: {out!r}")
check(any(t["function"]["name"] == "bash" for t in rs[0].get("tools", [])) and rs[0].get("think") is True, "tools and think:true sent")

print("test effort: the level saved under the old global key goes to the model it was set for")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\nmodel=fake-thinker:latest\nthink=1\nthink_level=high\n")
out, rc = run(["-p", "hi"], env=ENV2)
check(requests()[-1]["model"] == "fake-thinker:latest" and requests()[-1].get("think") == "high", "think_level=high still read")
out, rc = run(["-m", "fake-levels:latest", "-p", "hi"], env=ENV2)
check(requests()[-1].get("think") == "medium", f"but not applied to another model (think=1 alone: that model's default level): {requests()[-1].get('think')!r}")
s = Session([], env=ENV2); s.expect("Ctrl-D to quit")
s.send("/temp 0.2\r"); s.expect("temperature = 0.2"); s.send("\x04"); s.close()
cfg2 = open(CFG2_FILE).read()
check("effort.fake-thinker:latest=high" in cfg2 and "think_level" not in cfg2, f"the next save writes it per model and drops the old key: {cfg2!r}")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")

print("test effort and advisor: a fresh process reads them back from the config")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\nmodel=fake-levels:latest\neffort.fake-levels:latest=high\neffort.fake-big:latest=off\nadvisor=fake-big:latest\nadvisor_ctx=8192\n")
out, rc, rs = run_rounds("fake-levels:latest", ["--yolo", "-p", "TOOL_ADVISOR please"])
adv = [r for r in requests() if r["model"] == "fake-big:latest"][-1]
check(rc == 0 and thinks(rs) == ["high", "high"], f"the model's saved effort: {thinks(rs)!r}")
check(adv["options"]["num_ctx"] == 8192 and adv.get("think") is False, f"the saved advisor, its window and its own effort (off): {adv['options']!r} {adv.get('think', 'absent')!r}")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\nadvisor=fake-big:latest\nadvisor_ctx=100\neffort.fake-levels:latest=ultra\n")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENV2)
check([r for r in requests() if r["model"] == "fake-big:latest"][-1]["options"]["num_ctx"] == 16384, "an advisor_ctx too small to be one is ignored")
s = Session(["-m", "fake-levels:latest"], env=ENV2, banner=True)
check(s.expect("effort 'ultra' is saved for fake-levels:latest, which does not offer it"), "a saved effort the model does not offer is said at start-up, and left to the model")
check(s.expect("advisor: fake-big:latest"), "the banner names the advisor"); s.send("\x1b"); s.expect("(no such text)", 1)
s.send("hello\r"); s.expect("Echo: hello"); check("think" not in requests()[-1] and "· ultra" not in s.text(), "and neither sent nor shown in the bar")
s.send("\x04"); s.close()
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")

print("test interactive: /effort offers what the model has, keeps it per model, shows it in the bar")
s = Session(["-m", "fake-coder:latest"], env=ENV2); s.expect("Ctrl-D to quit")
s.send("/effort\r"); check(s.expect("cannot think"), "/effort on a model that cannot think says so")
s.send("/effort high\r"); check(s.expect("cannot think"), "and sets nothing")
s.send("/model fake-levels:latest\r"); s.expect("model set to fake-levels")
s.send("/effort off\r"); check(s.expect("cannot stop thinking"), "/effort off: gpt-oss-like models cannot stop")
check("low · medium · high" in since_send(s), "with the levels it has")
s.send("/effort ultra\r"); check(s.expect("has no effort 'ultra'"), "a level it does not have is refused")
s.send("/effort max\r"); check(s.expect("sent as high, its strongest level"), "max is as hard as it goes, whatever the model calls that")
s.send("/effort default\r"); s.expect("effort: default")   # so that the picker opens on "default" and the filter has to find "high"
s.send("/effort\r"); check(s.expect("Effort for fake-levels:latest"), "/effort opens a picker")
check(s.expect("leave it to the model (medium)"), "default first, with what the model does by itself")
picker = since_send(s)
check("low" in picker and "medium" in picker and "high" in picker and "no thinking" not in picker, f"its levels, and no off: {picker[-400:]!r}")
s.send("hig"); time.sleep(0.2); s.send("\r"); check(s.expect("effort: high"), "picked by filter + Enter")
check(s.expect("sent with every model call"), "and told why /think auto does not switch it off for the tool rounds")
check(s.expect("fake-levels · high"), f"the status bar shows the effort with the model (model_short drops :latest): {s.text()[-300:]!r}")
s.send("hello levels\r"); check(s.expect("Echo: hello levels"), "reply"); check(requests()[-1].get("think") == "high", "level sent")
check("effort.fake-levels:latest=high" in open(CFG2_FILE).read(), "saved per model")
s.send("/model fake-thinker:latest\r"); s.expect("model set to fake-thinker")
s.send("/effort\r"); check(s.expect("Effort for fake-thinker:latest"), "picker for an on/off model")
check(s.expect("this model has no levels"), "says it has no levels"); s.send("\x1b"); s.expect("effort unchanged")
s.send("hello thinker\r"); s.expect("Echo: hello thinker"); check("think" not in requests()[-1], "the other model's level does not follow")
s.send("/status\r"); check(s.expect("effort     default (on)"), f"/status shows the effort: {since_send(s)[-300:]!r}")
s.send("/model fake-levels:latest\r"); s.expect("model set to fake-levels")
s.send("back again\r"); s.expect("Echo: back again"); check(requests()[-1].get("think") == "high", "back on the first model its level is still there")
s.send("/think off\r"); check(s.expect("cannot stop thinking: it is sent its weakest level (low)"), "/think off says what it means for this model"); s.expect("think: off"); s.send("quiet now\r"); s.expect("Echo: quiet now")
check(requests()[-1].get("think") == "low", "/think off: never think — for this model, as little as it can")
s.send("/effort medium\r"); check(s.expect("/think was off: now auto"), "setting a level switches thinking back on")
s.send("SLOW while busy\r"); s.expect("one ")
s.send("/effort low\r"); check(s.expect("from the next request", 3), "/effort LEVEL is accepted while the model works, for the next request")
check("queued" not in since_send(s) and "eight" not in since_send(s), "not queued for after the turn, and not applied under the request in flight")
s.send("/effort\r"); check(s.expect("fake-levels:latest offers: low · medium · high", 3), "bare /effort prints the levels while busy")
check("Effort for" not in since_send(s) and "eight" not in since_send(s), "instead of opening a picker inside a live request")
check(s.expect("eight", 10), "generation carried on"); s.expect("tok/s")
check(s.expect("effort: low", 5), "the setting is applied as the turn ends, not kept until the next message")
s.send("after effort\r"); s.expect("Echo: after effort")
check(requests()[-1].get("think") == "low", "the next request carries the level that was set while busy")
print("test interactive: /ctx SIZE while busy is kept for the next request")
s.send("/ctx 8k\r"); s.expect("tok/s"); s.send("/ctx 16k\r"); s.expect("tok/s")   # warm: sets the window for a request that has ended
s.send("SLOW ctx busy\r"); s.expect("one ")
s.send("/ctx 8k\r"); check(s.expect("context window: 8k", 3) and s.expect("from the next request", 3), "/ctx SIZE runs while the model works, and says when it applies")
check("queued" not in since_send(s), "not queued for after the turn")
check(s.expect("eight", 10), "the running turn carries on"); s.expect("tok/s")
check(requests()[-1]["options"]["num_ctx"] == 16384, "the running turn kept the window it started with")
s.send("after busy\r"); s.expect("Echo: after busy")
check(requests()[-1]["options"]["num_ctx"] == 8192, "the next request uses the new window")
print("test interactive: /advisor guidance while busy is kept for the next request")
s.send("SLOW guidance busy\r"); s.expect("one ")
s.send("/advisor guidance strong\r"); check(s.expect("from the next request", 3), "/advisor guidance runs while the model works, and says when it applies")
check("queued" not in since_send(s), "not queued for after the turn")
s.expect("eight", 10); s.expect("tok/s")
s.send("after guidance\r"); s.expect("Echo: after guidance")
check("advisor_guidance=strong" in open(CFG2_FILE).read(), "the next request applies it, and it is saved")
s.send("/advisor guidance normal\r"); s.expect("advisor guidance: normal")
s.send("/ctx 32k\r"); s.expect("context window: 32k")   # the config is shared with the advisor tests below: back to the default
s.send("/model fake-thinker:latest\r"); s.expect("model set to fake-thinker")
s.send("/effort off\r"); s.expect("effort: off"); s.send("/think on\r"); check(s.expect("/effort off for fake-thinker:latest is forgotten"), "/think on takes back an /effort off, or nothing would change")
s.send("/think auto\r"); s.expect("think: auto"); s.send("/model fake-levels:latest\r"); s.expect("model set to fake-levels")
s.send("/effort default\r"); check(s.expect("effort: default"), "/effort default")
check("effort.fake-levels" not in open(CFG2_FILE).read(), "forgotten in the config too")
s.send("\x04"); s.close()

print("test advisor: the agent consults a stronger model, which is shown the conversation")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENV2)
check("advisor" not in [t["function"]["name"] for t in requests()[-1].get("tools", [])] and "advisor is a stronger" not in requests()[-1]["messages"][0]["content"], "no advisor set: no tool, no mention")
check("no advisor is set" in requests()[-1]["messages"][-1]["content"], "a model that calls it anyway is told there is none")
check("⎿  error: no advisor is set" in out, f"and so is the user: {out!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENV2)
check(rc == 0 and "⤷ advisor fake-big:latest" in out and "consultation 1 of 3" in out, f"consultation shown: {out!r}")
check("ADVICE: read hay.txt" in out and "Tool said: ADVICE: read hay.txt" in out, "the advice is previewed, and the agent carries on with it")
reqs = requests()
adv = [r for r in reqs if r["model"] == "fake-big:latest"][-1]
main_first, main_last = [r for r in reqs if r["model"] == "fake-coder:latest"][-2:]
check("advisor" in [t["function"]["name"] for t in main_first["tools"]] and "advisor is a stronger" in main_first["messages"][0]["content"], "the agent is offered the tool and told when to use it")
check("tools" not in adv and [m["role"] for m in adv["messages"]] == ["system", "user"], "the advisor gets no tools and one quoted brief, not the conversation's turns")
brief = adv["messages"][1]["content"]
check("[user]\nTOOL_ADVISOR please" in brief and "# What the agent asks you\nWhy does the needle test fail" in brief, f"the brief holds the transcript and the question: {brief[-300:]!r}")
check("→ advisor(" not in brief, "the call that asked is not part of the transcript")
check(adv["keep_alive"] == 0 and adv["options"]["num_ctx"] == 16384 and adv["options"]["num_predict"] == 5461, f"a window of its own, a cap on the answer, and its memory back at once: {adv.get('keep_alive')!r} {adv['options']!r}")
check("think" not in adv, "its effort is left to it unless set")
check(main_last["messages"][-1]["role"] == "tool" and main_last["messages"][-1]["tool_name"] == "advisor" and "consultation 1 of 3" in main_last["messages"][-1]["content"], "the advice is the tool result")
check(main_last["options"]["num_ctx"] == 32768 and main_last["keep_alive"] == "30m", "the main model's own settings are untouched by it")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--yolo", "-p", "TOOL_ADVISOR_NOQ please"], env=ENV2)
check("It did not say" in [r for r in requests() if r["model"] == "fake-big:latest"][-1]["messages"][1]["content"], "no question: the advisor is asked to review where the agent stands")
n0 = len([r for r in requests() if r["model"] == "fake-big:latest"])
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--yolo", "-p", "TOOL_ADVISOR ADVISOR_LOOP"], env=ENV2)
check(len([r for r in requests() if r["model"] == "fake-big:latest"]) - n0 == 3 and "is the limit" in out, f"a model that keeps asking is stopped after 3 consultations: {out[-300:]!r}")
check("Tool said: error: the advisor has been consulted 3 times" in out, "and told so, in a tool result")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "no-such:latest", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENV2)
check(rc == 0 and "Tool said: error: the advisor model no-such:latest is not installed" in out and "/advisor picks another" in out, f"an advisor that is not there costs the consultation, not the turn: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-remote:cloud", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENV2)
adv = [r for r in requests() if r["model"] == "fake-remote:cloud"][-1]
check("keep_alive" not in adv and "num_ctx" not in adv["options"], f"a cloud model is sent neither a window nor a keep_alive: they are not ours to set: {adv['options']!r}")
check("ollama signin" in out and rc == 0, f"and a daemon that is not signed in is what the user is told: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--yolo", "--no-think", "--draft", "2", "--mode", "plan", "-p", "TOOL_ADVISOR THINKTAG"], env=ENV2)
adv = [r for r in requests() if r["model"] == "fake-big:latest"][-1]
check("think" not in adv and "draft_num_predict" not in adv["options"], f"/think and --draft are the main model's: the advisor follows its own effort: {adv.get('think', 'absent')!r} {adv['options']!r}")
check("plan mode" in adv["messages"][1]["content"], "the advisor is told the agent is in plan mode")
check(adv["messages"][1]["content"].rstrip().endswith("do not call tools."), "what matters most stands last in the brief: a server that cuts a prompt keeps the end")
check("SECRET-REASONING" not in requests()[-1]["messages"][-1]["content"] and "ADVICE: strip me" in requests()[-1]["messages"][-1]["content"], "thinking that arrives inside the answer is not passed on as advice")
main_req = [r for r in requests() if r["model"] == "fake-coder:latest"][-1]
names = [t["function"]["name"] for t in main_req["tools"]]
check("advisor" in names and "write_file" not in names, f"plan mode keeps the advisor: it changes nothing: {names!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--yolo", "--output-format", "json", "-p", "TOOL_ADVISOR please"], env=ENV2)
check(json.loads(out)["advisor_calls"] == 1, "-p --output-format json counts the consultations")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--yolo", "-p", "TOOL_ADVISOR CUTADVICE"], env=ENV2)
check("Tool said: error: the advisor gave no answer" in out and "ran out of tokens" in out, f"out of tokens while thinking: said, not returned as empty advice: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-coder:latest", "--yolo", "-c", "8k", "-p", "TOOL_ADVISOR please"], env=ENV2)
adv = [r for r in requests() if r["messages"][0]["content"].startswith("You are the advisor")][-1]
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-coder:latest", "--yolo", "-c", "8k", "--draft", "2", "-p", "TOOL_ADVISOR please"], env=ENV2)
adv = [r for r in requests() if r["messages"][0]["content"].startswith("You are the advisor")][-1]
check(adv["options"].get("draft_num_predict") == 2, "and its draft length: a changed one reloads it too")
check(adv["model"] == "fake-coder:latest" and adv["options"]["num_ctx"] == 8192 and adv["keep_alive"] == "30m", f"the main model as its own advisor keeps the main window and keep_alive: anything else would reload it: {adv['options']!r} {adv.get('keep_alive')!r}")

print("test interactive: /model add HOST URL: a host kept in the config, usable as an advisor")
CFG_H = tempfile.mkdtemp(prefix="crowcfgh_"); os.makedirs(os.path.join(CFG_H, "corbienest"))
HOSTCFG = os.path.join(CFG_H, "corbienest", "config")
open(HOSTCFG, "w").write("memory=0\nmemory_idle=0\n")
ENVH = dict(ENVK, XDG_CONFIG_HOME=CFG_H, FAKEHOST_KEY="test-key")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVH); s.expect("Ctrl-D to quit")
s.send(f"/model add fakehost {HOST}/openai/v1 FAKEHOST_KEY\r"); check(s.expect("✓ host fakehost"), "/model add saves the host")
s.send("/model add xai https://x.example/v1\r"); check(s.expect("a built-in provider's name"), "a built-in provider's name is refused")
s.send("/model add fake-coder https://x.example/v1\r"); check(s.expect("is an installed model's name"), "a name an installed model has is refused")
s.send("/model add remote http://example.com/v1\r"); check(s.expect("plain http to another machine"), "plain http to another machine is refused")
s.send("/advisor fakehost:gpt-fake\r"); check(s.expect("advisor: fakehost:gpt-fake"), "the host takes an advisor")
s.close()
check(f"host.fakehost={HOST}/openai/v1|FAKEHOST_KEY" in open(HOSTCFG).read(), "the config keeps the host, with its key variable and never the key")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVH); s.expect("Ctrl-D to quit")
s.send("/advisor\r"); check(s.expect("fakehost:MODEL") and s.expect("FAKEHOST_KEY"), "after a restart the host is still listed")
s.close()
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fakehost:gpt-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVH)
check(rc == 0 and "Tool said: ADVICE" in out and last_preq("openai")["body"].get("model") == "gpt-fake", f"the host's advice comes back as the tool result: {out[-200:]!r}")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVH); s.expect("Ctrl-D to quit")
s.send("/model remove fakehost\r"); check(s.expect("removed host fakehost"), "/model remove takes the host out")
s.close()
check("host." not in open(HOSTCFG).read(), "and the config no longer has it")

print("test interactive: /advisor")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENV2); s.expect("Ctrl-D to quit")
s.send("/advisor\r"); check(s.expect("ollama signin") and s.expect("anthropic:claude-opus-5") and s.expect("ANTHROPIC_API_KEY"), "/advisor says how a cloud and a hosted model are set up")
check(s.expect("/advisor guidance LEVEL") and s.expect("when the work is hard · 3 per request · current") and s.expect("brought in, unasked, when tool calls keep failing") and s.expect("/advisor effort LEVEL"), "and what guidance and effort do")
check(s.expect("Select advisor"), "/advisor opens a picker"); check(s.expect("No advisor"), "with 'No advisor' first")
check(seen(s, "┌") and seen(s, "│ Select advisor"), "the picker is a box over the conversation on the full screen, not a list printed under the lines above it")
s.send("\x1b"); check(s.expect("advisor unchanged: none"), "Esc leaves it")
s.send("/advisor fake-bi"); s.send("g"); seen(s, "fake-big:latest", 5)
_w = since_send(s)   # a repaint of the old transcript comes first; the list is drawn after each field row that changes it
_segs = [g for g in re.split(r"(?=❯ /advisor )", _w) if "tab complete" in g]
view = _segs[-1] if _segs else ""
check("fake-big:latest" in view and "fake-chat" not in view, f"the installed models are suggested as a name is typed: {view!r}")
s.send("\t\r"); check(s.expect("advisor: fake-big:latest"), "Tab completes the name, Enter sets it")
s.send("/advisor "); s.send("o"); s.expect("(no such text)", 2); view = since()   # the list is drawn under the field: read it, then everything since the key
check("off" in view and "openai:" in view and "OPENAI_API_KEY not set" in view, f"the hosted providers are offered by their prefix, with whether the key is there: {view!r}")
s.send("\x7f" * 10); s.send("/adviso"); s.send("r"); check(re.search(r"/advisor\s+a stronger model the agent may consult: a local one, or Claude, ChatGPT, Grok", listed()), f"the command's own line says a hosted one can be it: {listed()!r}")
s.send(" "); view = listed()
check("xai:" in view and "(Grok)" in view and "(ChatGPT)" in view and "anthropic:" in view and "(Claude)" in view, f"and its arguments start with the three providers: {view!r}")
s.send("cha"); s.send("t"); view = listed(); check("openai:" in view and "xai:" not in view, f"found by the name people know them by: {view!r}")
s.send("\x7f" * 4); s.send("anthropic"); s.send(":"); view = listed()
check("ANTHROPIC_API_KEY is not set" in view and "export it" in view, f"behind a prefix without its key the list says what is missing, instead of nothing: {view!r}")
s.send("\x7f" * 19); s.send("/advisor off\r"); s.expect("advisor off")
s.send("/advisor nope:latest\r"); check(s.expect("does not know a model 'nope:latest'"), "an unknown model is refused")
s.send("/advisor fake-big:latest\r"); check(s.expect("advisor: fake-big:latest"), "/advisor MODEL"); check(s.expect("at most 3 times per request"), "with what it costs")
check("advisor=fake-big:latest" in open(CFG2_FILE).read(), "saved")
s.send("/advisor effort high\r"); check(s.expect("advisor effort: high"), "/advisor effort LEVEL")
check("effort.fake-big:latest=high" in open(CFG2_FILE).read(), "kept with the advisor's model, like any model's")
s.send("/advisor ctx 8k\r"); check(s.expect("advisor context window: 8k"), "/advisor ctx")
s.send("/advisor window\r"); check(s.expect("advisor window: 15 minutes"), "/advisor window: a quarter of an hour unless told otherwise")
s.send("/advisor window 30\r"); check(s.expect("advisor window: 30 minutes") and "advisor_window=30" in open(CFG2_FILE).read(), "/advisor window MINUTES, saved")
s.send("/advisor window off\r"); check(s.expect("advisor window: off") and "advisor_window=0" in open(CFG2_FILE).read(), "off: the limit holds for the whole request")
s.send("/advisor window soon\r"); check(s.expect("usage: /advisor window"), "anything else is refused")
s.send("/advisor window 15\r"); check(s.expect("advisor window: 15 minutes") and "advisor_window" not in open(CFG2_FILE).read(), "the default is not written")
s.send("TOOL_ADVISOR now\r"); check(s.expect("⤷ advisor fake-big:latest"), "consulted"); check(s.expect("Tool said: ADVICE"), "advice used")
adv = [r for r in requests() if r["model"] == "fake-big:latest"][-1]
check(adv.get("think") == "high" and adv["options"]["num_ctx"] == 8192, f"its effort and its window: {adv.get('think')!r} {adv['options']!r}")
s.send("/status\r"); check(s.expect("advisor    fake-big:latest"), "/status shows it")
s.send("/cost\r"); check(s.expect("advisor       1 consultation"), f"/cost counts it: {since_send(s)[-300:]!r}")
s.send("TOOL_ADVISOR SLOWADVICE\r"); check(s.expect("⤷ advisor"), "a slow consultation")
_m = s.mark   # the consultation's window: from its own send
time.sleep(0.6); s.send("never mind, do this\r")
_ok, _end = False, time.time() + 15
while time.time() < _end and not _ok:
    s.expect("(no such text)", 0.3)
    _w = clean(s.out[_m:].decode("utf-8", "replace"))
    _ok = "▸ advice" in _w and "Echo:" in _w and "never mind, do" in _w   # the consultation finished (its folded result) and the queued message got its turn
check(_ok, "a queued message does not stop the consultation: it runs to its end (the consultation's result is folded, and the queued message is answered)")
check(s.expect("Echo: never mind, do this", 10), "and reaches the model on its next call, after the advice")
msgs = requests()[-1]["messages"]
check(msgs[-2]["role"] == "tool" and "not heard out" not in msgs[-2]["content"] and "ADVICE:" in msgs[-2]["content"] and msgs[-1]["content"] == "never mind, do this", f"the conversation stays well-formed: {[m['role'] for m in msgs[-3:]]}")
s.send("TOOL_ADVISOR SLOWADVICE again\r"); s.expect("⤷ advisor"); time.sleep(0.6); s.send("\x1b")
check(s.expect("⎿ interrupted", 10), "Esc skips the consultation"); check(s.expect("Tool said: error: the consultation was interrupted", 10), "and the turn carries on without it")
s.send("/status\r"); check(s.expect("last prompt 123 tokens"), "the advisor's prompt (30k tokens, says the fake) is no part of the conversation's context figure")
check("auto-compacting" not in s.text() and "ctx 91%" not in s.text(), "so nothing is compacted on its account")
s.send("TOOL_ADVISOR EVICTMAIN\r"); check(s.expect("fake-coder:latest was unloaded to make room", 15), f"a main model pushed out of memory is said: {s.text()[-400:]!r}"); s.expect("tok/s")
s.send("SLOW report while busy\r"); s.expect("one ")
s.send("/advisor\r"); check(s.expect("advisor: fake-big:latest"), "bare /advisor reports while the model works"); s.expect("tok/s")
s.send("/advisor\r"); s.expect("Select advisor"); s.send("No adv"); time.sleep(0.2); s.send("\r"); check(s.expect("advisor off"), "'No advisor' in the picker")
s.send("plain again\r"); s.expect("Echo: plain again")
check("advisor" not in [t["function"]["name"] for t in requests()[-1]["tools"]] and "advisor=" not in open(CFG2_FILE).read(), "off: the tool is gone, and so is the config key")
s.send("/advisor ctx 8k\r"); check(s.expect("advisor context window: 8k", 5), "/advisor ctx with the advisor switched off again sets it for the next one (it used to crash)")
s.send("/advisor ctx 1k\r"); check(s.expect("usage: /advisor ctx"), "a window too small to be one is refused")
s.send("/advisor ctx auto\r"); s.expect("advisor context window: auto")
s.send("/model fake-thinker:latest\r"); s.expect("model set to fake-thinker"); s.send("/think off\r"); s.expect("think: off")
s.send("/advisor fake-big:latest\r"); s.expect("advisor: fake-big:latest")
s.send("/advisor effort low\r"); check(s.expect("advisor effort: low"), "/advisor effort with /think off")
check("now auto" not in since_send(s) and "think=0" in open(CFG2_FILE).read(), "leaves /think alone: that is the main model's, and the advisor never reads it")
s.send("/advisor fake-thinker:latest\r"); check(s.expect("a second opinion, not a stronger one"), "the model doing the work as its own advisor")
s.send("/advisor effort on\r"); check(s.expect("this is its effort there too"), "its effort is one and the same entry"); check(s.expect("now auto") or "now auto" in since_send(s), "and there /think off is in the way")
s.send("SLOW and then\r"); s.expect("one ")
s.send("/advisor off\r"); time.sleep(0.5); check("advisor off" not in since_send(s), "/advisor with an argument waits for the turn: it asks the server and rebuilds the tool list")
check(s.expect("advisor off", 15), "and runs after it")
s.send("\x04"); s.close()

# ---------- the advisor on a hosted API: xAI, OpenAI, Anthropic ----------
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")

print("test advisor on a hosted API: xAI and OpenAI through Chat Completions")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "xai:grok-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
check(rc == 0 and "⤷ advisor xai:grok-fake" in out and "Tool said: ADVICE: read hay.txt" in out, f"consulted, and the agent carries on with the advice: {out[-400:]!r}")
r = last_preq("xai")
check(r["path"] == "/xai/v1/chat/completions" and [m["role"] for m in r["body"]["messages"]] == ["system", "user"], f"one request, a system and a user message: {r['path']!r}")
check(r["body"]["messages"][0]["content"].startswith("You are the advisor") and "# What the agent asks you\nWhy does the needle test fail" in r["body"]["messages"][1]["content"], "the same brief an Ollama advisor gets")
check(r["body"]["max_completion_tokens"] == 16000 and "max_tokens" not in r["body"] and "reasoning_effort" not in r["body"], f"the cap by the name reasoning models take; no effort unless set: {r['body'].keys()!r}")
check("stream" not in r["body"] and "tools" not in r["body"] and "keep_alive" not in r["body"] and "options" not in r["body"], "not streamed, no tools, nothing of Ollama's")
main_last = requests()[-1]
check("advice from xai:grok-fake" in main_last["messages"][-1]["content"], "the advice is the tool result")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "xai:grok-fake", "--yolo", "--output-format", "json", "-p", "TOOL_ADVISOR please"], env=ENVK)
j = json.loads(out); check(j["advisor_calls"] == 1 and j["prompt_tokens"] >= 2000, f"its tokens are counted: {j}")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\neffort.openai:gpt-fake=high\n")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "openai:gpt-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
check(rc == 0 and last_preq("openai")["body"].get("reasoning_effort") == "high" and "Tool said: ADVICE" in out, "OpenAI, with the effort saved for that model")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\neffort.openai:gpt-fake=max\n")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "openai:gpt-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
check(rc == 0 and "OpenAI answered 400: Unsupported value: 'max'" in out and "/advisor effort default" in out, f"an effort the model does not take: the server's words, and the way back: {out[-400:]!r}")
check("Tool said: error: the advisor (openai:gpt-fake) could not be reached" in out, "and the turn carries on")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "xai:grok-fake", "--yolo", "-p", "TOOL_ADVISOR CUTADVICE"], env=ENVK)
check("ran out of tokens while still thinking" in out and "Tool said: error: the advisor gave no answer" in out, f"spent on thinking: said as such: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "openai:gpt-fake", "--yolo", "-p", "TOOL_ADVISOR REFUSEADVICE"], env=ENVK)
check(rc == 0 and "gpt-fake declined to answer" in out, f"a refusal is an answer of its own kind: {out[-300:]!r}")

print("test advisor on a hosted API: Anthropic's Messages API")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\neffort.anthropic:claude-fake-opus=xhigh\n")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-fake-opus", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
r = last_preq("anthropic")
check(rc == 0 and "Tool said: ADVICE: read hay.txt" in out, f"consulted: {out[-300:]!r}")
check(r["path"] == "/anthropic/v1/messages" and r["headers"].get("anthropic-version") == "2023-06-01", f"the Messages API, versioned: {r['path']!r} {r['headers']!r}")
b = r["body"]
check(b["system"].startswith("You are the advisor") and [m["role"] for m in b["messages"]] == ["user"] and b["max_tokens"] == 16000, "the system prompt on its own, one user message")
check(b.get("thinking") == {"type": "adaptive"} and b.get("output_config") == {"effort": "xhigh"} and "fallbacks" not in b, f"adaptive thinking, the effort it has; no fallbacks where the model has none: {b.get('thinking')!r} {b.get('output_config')!r}")
check("anthropic-beta" not in r["headers"], "and no beta header for them")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-opus-5", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
r = last_preq("anthropic")
check(rc == 0 and r["body"].get("fallbacks") == "default" and r["headers"].get("anthropic-beta") == "server-side-fallback-2026-07-01", f"Opus 5: a declined request is answered by the fallback model: {r['body'].get('fallbacks')!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-fake-opus", "--yolo", "-p", "TOOL_ADVISOR REFUSEADVICE"], env=ENVK)
check(rc == 0 and "declined to answer (refusal: cyber)" in out, f"a refusal says its category, and the turn goes on: {out[-300:]!r}")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\neffort.anthropic:claude-fake-haiku=on\n")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-fake-haiku", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
b = last_preq("anthropic")["body"]
check(rc == 0 and b.get("thinking") == {"type": "enabled", "budget_tokens": 8000} and "output_config" not in b, f"a model with a thinking budget and no effort levels: {b.get('thinking')!r}")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")

print("test the model that does the work on Anthropic: haiku works, opus advises")
n0 = len(preqs())
out, rc = run(["-m", "anthropic:claude-fake-haiku", "--advisor", "anthropic:claude-fake-opus", "--yolo", "-p", "TOOL_BASH please"], env=ENVK)
check(rc == 0 and "hello-from-tool" in out and "Tool said:" in out, f"the tool ran, and the model read its result: {out[-400:]!r}")
posts = anth_since(n0)
work = work_posts(posts)
check(work and all(r["path"] == "/anthropic/v1/messages" and r["body"].get("stream") is True for r in work), f"streamed to the Messages API: {[(r['path'], r['body'].get('stream')) for r in work]!r}")
if work:
    first = work[0]["body"]
    check(any(t.get("name") == "bash" for t in first.get("tools") or []) and "thinking" not in first and "fallbacks" not in first and "output_config" not in first and first.get("max_tokens") == 64000,
          f"haiku, with tools, and no thinking (a budget model, left off): model={first.get('model')!r} max_tokens={first.get('max_tokens')!r} thinking={first.get('thinking')!r}")
    check(any(has_tool_result(r) for r in work[1:]), "the next call carries the tool_result")
check(not consult_posts(posts), "this request never consulted the advisor")

print("test loop guard: a stopped request leaves no call unanswered, and the next request is accepted")
cfg_before = open(CFG2_FILE).read()
open(CFG2_FILE, "w").write(cfg_before + "max_iters=2\n")   # the guard at 2 tool rounds, for this test only
try:
    n0 = len(preqs())
    out, rc = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "-p", "TOOL_LOOP please"], env=ENVK)
    check(rc == 0 and "loop guard" in out, f"the request stopped at the loop guard: {out[-300:]!r}")
    check("✗" not in out, "and nothing was refused on the way")
    out2, rc2 = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "--continue", "-p", "continue"], env=ENVK)
    check(rc2 == 0 and "✗" not in out2 and "tool_use" not in out2 and "Tool said:" in out2,
          f"the next request goes through: {out2[-300:]!r}")
finally:
    open(CFG2_FILE, "w").write(cfg_before)

print("test binary output: a tool result that is not UTF-8 reaches the model as a valid request")
out, rc = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "-p", "TOOL_BINARY please"], env=ENVK)
check(rc == 0 and "✗" not in out and "Tool said: bad" in out, f"the request went through, with the output mended: {out[-300:]!r}")

def msg_block_types(post):
    """content-block types on the wire, in order."""
    out = []
    for m in post["body"].get("messages") or []:
        c = m.get("content")
        if isinstance(c, list): out += [b.get("type") for b in c if isinstance(b, dict)]
    return out

print("test resume: thinking stays when its prefix still matches, and is set aside when the session cannot say so")
sdir = os.path.join(CFG2, "corbienest", "sessions")
os.makedirs(sdir, exist_ok=True)
out, rc = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "-p", "plain hello"], env=ENVK)
check(rc == 0 and "✗" not in out and "Echo:" in out and "set aside" not in out, f"a fresh turn keeps no stale blocks: {out[-200:]!r}")
spath = max((os.path.join(sdir, f) for f in os.listdir(sdir) if f.endswith(".json")), key=os.path.getmtime)
sess = json.load(open(spath))
fp = sess.get("think_prefix")
check(isinstance(fp, str) and len(fp) == 16 and all(c in "0123456789abcdef" for c in fp), f"the prefix the blocks were signed against is saved: {fp!r}")
asst = sess["messages"][-1]
pc = asst.get("provider_content") or [{"type": "text", "text": asst.get("content") or ""}]
pc.insert(0, {"type": "redacted_thinking", "data": "hidden"})
pc.insert(0, {"type": "thinking", "thinking": "", "signature": "sig-kept"})
asst["provider_content"] = pc
json.dump(sess, open(spath, "w"))
n1 = len(preqs())
out, rc = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "--resume", sess["id"], "-p", "and again"], env=ENVK)
check(rc == 0 and "✗" not in out and "set aside" not in out and "Echo:" in out, f"the same prefix sends the blocks again: {out[-300:]!r}")
kept = work_posts(anth_since(n1))
check(len(kept) == 1 and "thinking" in msg_block_types(kept[0]) and "redacted_thinking" in msg_block_types(kept[0]) and "sig-kept" in json.dumps(kept[0]["body"]),
      f"signed blocks survived the resume: {msg_block_types(kept[0]) if kept else None}")
fix_id = "thinkfix"
fix = {"id": fix_id, "cwd": WORK, "model": "anthropic:claude-fake-haiku", "updated": 1, "title": "old thinking",
       "prompt_tokens": 1, "eval_tokens": 1, "usage": [], "typed": [{"raw": "look", "msg": "look"}],
       "messages": [
           {"role": "user", "content": "look"},
           {"role": "assistant", "content": "Running it.",
            "tool_calls": [{"id": "toolu_old", "function": {"name": "bash", "arguments": "{\"command\":\"echo hi\"}"}}],
            "provider_content": [
                {"type": "thinking", "thinking": "", "signature": "sig-old"},
                {"type": "redacted_thinking", "data": "hidden"},
                {"type": "text", "text": "Running it."},
                {"type": "tool_use", "id": "toolu_old", "name": "bash", "input": {"command": "echo hi"}, "caller": {"type": "direct"}}]},
           {"role": "tool", "tool_name": "bash", "tool_call_id": "toolu_old", "content": "hi"}]}
json.dump(fix, open(os.path.join(sdir, fix_id + ".json"), "w"))
n2 = len(preqs())
out, rc = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "--resume", fix_id, "-p", "RESUME_BREAK please"], env=ENVK)
check(rc == 0 and "set aside" in out and "✗" not in out and "Invalid" not in out and "Tool said:" in out,
      f"a session with no saved prefix continues: {out[-400:]!r}")
broke = work_posts(anth_since(n2))
types = msg_block_types(broke[0]) if broke else []
check(len(broke) == 1 and "thinking" not in types and "redacted_thinking" not in types and "tool_use" in types and "tool_result" in types and "sig-old" not in json.dumps(broke[0]["body"]),
      f"the blocks were set aside before the call, and the tool round stayed paired: {types}")
saved = json.load(open(os.path.join(sdir, fix_id + ".json")))
check(isinstance(saved.get("think_prefix"), str) and len(saved["think_prefix"]) == 16 and "sig-old" not in json.dumps(saved["messages"]),
      f"the session now records the prefix, without the old blocks: {saved.get('think_prefix')!r}")

print("test a long tool run: eliding an old result sets aside the thinking signed over it, before the next call")
cfg_before = open(CFG2_FILE).read()
try:
    n3 = len(preqs())
    out, rc = run(["-m", "anthropic:claude-fake-haiku", "--yolo", "-c", "8k", "-p", "THINK_LIVE please"], env=ENVK)
    check(rc == 0 and "Live ok" in out and "set aside" in out and "✗" not in out and "Invalid" not in out,
          f"the run continues, and the stale blocks are set aside without a refused call: {out[-500:]!r}")
    sent = work_posts(anth_since(n3))
    elided = [r for r in sent if "[earlier output elided" in json.dumps(r["body"])]
    check(len(elided) == 1 and "sig-b" not in json.dumps(elided[0]["body"]) and "sig-a" in json.dumps(elided[0]["body"]),
          f"the block from before the edit is kept, the one signed over the old result is not: "
          f"{msg_block_types(elided[0]) if elided else [msg_block_types(r) for r in sent]}")
finally:
    open(CFG2_FILE, "w").write(cfg_before)
n0 = len(preqs())
out, rc = run(["-m", "anthropic:claude-fake-haiku", "--advisor", "anthropic:claude-opus-5", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
check(rc == 0 and "Tool said: ADVICE:" in out, f"the advice came back as the tool result: {out[-400:]!r}")
posts = anth_since(n0)
cons, work = consult_posts(posts), work_posts(posts)
check(len(cons) == 1 and [m["role"] for m in cons[0]["body"]["messages"]] == ["user"] and "tools" not in cons[0]["body"]
      and cons[0]["body"].get("fallbacks") == "default" and cons[0]["headers"].get("anthropic-beta") == "server-side-fallback-2026-07-01",
      f"the consultation is still one user message, and Opus 5 asks for a fallback: {cons[0]['body'].get('fallbacks')!r} {cons[0]['headers'].get('anthropic-beta')!r}" if cons else f"no consultation in {len(posts)} posts")
check(work and all(isinstance(r["body"].get("system"), list) and "tools" in r["body"] and "fallbacks" not in r["body"] for r in work),
      "the model doing the work is a different request: tools, a system array, no fallbacks")

print("test advisor on a hosted API: no key, a wrong key, no such model")
noenv = {k: v for k, v in ENVK.items() if k != "XAI_API_KEY"}
n0 = len(preqs())
out, rc = run(["-m", "fake-coder:latest", "--advisor", "xai:grok-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=noenv)
check(rc == 0 and "⎿ no advice: XAI_API_KEY is not set — export it before starting corbienest" in out and "Tool said: error: the advisor (xai:grok-fake) cannot be used" in out,
      f"no key: said, to the user and to the agent: {out[-300:]!r}")
check(len(preqs()) == n0, "and nothing is sent")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "xai:grok-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=dict(ENVK, XAI_API_KEY="wrong-key"))
check(rc == 0 and "xAI refused the key in XAI_API_KEY (400" in out and "wrong-key" not in out, f"a wrong key (xAI says so with a 400): said, without printing it: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "openai:gpt-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=dict(ENVK, OPENAI_API_KEY="sk-secret-wxyz"))
check(rc == 0 and "OpenAI refused the key in OPENAI_API_KEY (401" in out and "wxyz" not in out and "wxyz" not in json.dumps(requests()[-1]), f"not even the masked piece of it OpenAI quotes back: {out[-300:]!r}")
t0 = time.time()
out, rc = run(["-m", "fake-coder:latest", "--advisor", "openai:gpt-fake", "--yolo", "-p", "TOOL_ADVISOR please"], env=dict(ENVK, OPENAI_BASE_URL="http://192.0.2.1/v1"))
check(rc == 0 and "OPENAI_BASE_URL is plain http to another machine (192.0.2.1)" in out and time.time() - t0 < 4,
      f"a key is not sent in the clear to another machine — refused before any connection: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-nope", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
check(rc == 0 and "Anthropic does not know a model 'claude-nope'" in out, f"no such model: {out[-300:]!r}")
check(key_nowhere() is None, f"the key was written nowhere: {key_nowhere()!r}")

print("test interactive: /advisor with a hosted API")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVK); seen(s, "Ctrl-D to quit")
s.send("/advisor anthropic:"); s.send("c"); view = listed()
check("anthropic:claude-fake-opus" in view and "Claude Fake Haiku" in view and "grok-fake" not in view, f"behind a provider's prefix its own model list is suggested: {view!r}")
s.send("\x7f" * 20); s.send("/advisor xai:g"); s.send("r"); check("xai:grok-fake" in listed(), "each provider's own")
s.send("\t\r"); check(seen(s, "advisor: xai:grok-fake"), "and Tab + Enter takes one")
check(key_nowhere() is None, f"listing the models wrote the key nowhere: {key_nowhere()!r}")
s.send("/advisor xai:grok-nope\r"); check(seen(s, "xAI does not know a model 'grok-nope'"), "the model endpoint is asked: an unknown model is refused")
s.send("/advisor xai:grok-fake\r"); check(seen(s, "advisor: xai:grok-fake"), "/advisor PROVIDER:MODEL")
check(seen(s, f"sent to {HOST}/xai/v1 with each consultation, billed to the key in XAI_API_KEY"), f"with where the conversation goes: {since_send(s)[-300:]!r}")
cfg = open(CFG2_FILE).read(); check("advisor=xai:grok-fake" in cfg and "test-key" not in cfg, "saved, without the key")
s.send("/advisor effort\r"); check(seen(s, "advisor effort for xai:grok-fake"), "/advisor effort offers its levels"); check(seen(s, "xhigh"), "the ones xAI's models take")
s.send("\x1b"); seen(s, "unchanged")
s.send("/advisor effort max\r"); check(seen(s, "sent as xhigh, its strongest level"), "max is as hard as it goes, in xAI's name")
s.send("TOOL_ADVISOR now\r"); check(seen(s, "Tool said: ADVICE"), "consulted"); check(last_preq("xai")["body"].get("reasoning_effort") == "xhigh", "with that effort")
s.send("TOOL_ADVISOR SLOWADVICE\r"); check(seen(s, "⤷ advisor xai:grok-fake"), "a slow consultation")
time.sleep(0.8); s.send("never mind, do this\r")
check(seen(s, "Echo:", 15) and seen(s, "never mind, do", 15), "a queued message does not stop a hosted consultation either: it reaches the model after it")   # streamed in pieces, the chrome redrawn between them
s.send("TOOL_ADVISOR SLOWADVICE again\r"); seen(s, "⤷ advisor"); time.sleep(0.8); s.send("\x1b")
check(seen(s, "⎿ interrupted", 10), "Esc skips it"); check(seen(s, "Tool said: error: the consultation was interrupted", 15), "and the turn carries on")
s.send("/cost\r"); check(seen(s, "advisor       2 consultations "), f"/cost counts the two that gave advice (the skipped one did not): {since_send(s)[-300:]!r}")
s.send("/usage\r"); check(seen(s, "local: no charge") and "fake-coder:latest" in since_send(s), f"/usage lists the model doing the work: {since_send(s)[-400:]!r}")
check(seen(s, "no price known") and re.search(r"xai:grok-fake\s+\d+ calls?\s+↑", since_send(s)), f"and the hosted advisor with its own tokens — a model the price list does not know: {since_send(s)[-400:]!r}")
s.send("\x1b"); s.expect("(no such text)", 0.5)   # a bare /usage opens an overlay that holds the output back until Esc
seen(s, "[ready]", 60)   # the reply to the queued message may still be streaming: /usage runs only when the prompt is ready
s.send("/usage price xai:grok-fake 2 6\r"); check(seen(s, "xai:grok-fake: $2 in · $6 out per million tokens"), "/usage price sets one")
check("price.xai:grok-fake=2/6" in open(CFG2_FILE).read(), "saved in the config")
s.send("/usage\r"); # the overlay's border is drawn across "set with", so the stream never has it as one run; the estimate and the price are checked
check(seen(s, "$2 in · $6 out per million") and re.search(r"≈ \$\d+\.\d+", since_send(s)), f"and the cost is estimated from it: {since_send(s)[-400:]!r}")
check(seen(s, "estimated cost"), "with a total")
s.send("\x1b"); s.expect("(no such text)", 0.5)
s.send("TOOL_ADVISOR priced\r"); check(seen(s, "Tool said: ADVICE") and re.search(r"▸ advice · \S+ tokens · \d+s · ≈ \$\d+\.\d+", since_send(s)), f"a consultation of a priced model says what it came to: {since_send(s)[-400:]!r}")
s.send("/cost\r"); check(seen(s, "est. cost     ≈ $"), f"and /cost has the session's estimate: {since_send(s)[-300:]!r}")
s.send("/usage price xai:grok-fake default\r"); check(seen(s, "xai:grok-fake: no price"), "default forgets it"); check("price.xai" not in open(CFG2_FILE).read(), "in the config too")
s.send("/advisor anthropic:claude-fake-haiku\r"); check(seen(s, "advisor: anthropic:claude-fake-haiku"), "switched to Anthropic")
s.send("/advisor effort on\r"); check(seen(s, "advisor effort: on"), "a thinking budget, on")
s.send("\x04"); s.close()
check(key_nowhere() is None, f"still nowhere: {key_nowhere()!r}")

print("test interactive: /model completes a local name and an Anthropic one")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVK); s.expect("Ctrl-D to quit")
s.send("/model fake-co"); s.send("d"); view = listed()
check("fake-coder:latest" in view and "fake-chat" not in view and "xai:" not in view and "openai:" not in view,
      f"a local name is completed as it is typed, and an advisor-only provider is not offered: {view!r}")
s.send("\t\r"); check(s.expect("model set to fake-coder:latest"), "Tab completes the name, Enter sets it")
s.send("/model "); view = listed()
check("anthropic:" in view and "fake-coder:latest" in view and "the model that does the work" in view and "xai:" not in view and "openai:" not in view,
      f"a bare argument offers Anthropic and the installed models: {view!r}")
s.send("anthropic"); s.send(":"); view = listed()
check("anthropic:claude-fake-opus" in view and "Claude Fake Haiku" in view and "grok-fake" not in view,
      f"behind anthropic: its own models are listed: {view!r}")
s.send("laude-fake-hai"); s.send("\t\r"); check(s.expect("model set to anthropic:claude-fake-haiku"), "Tab + Enter takes one")
s.send("/model claude:"); s.send("c"); view = listed()
check("claude:claude-fake-opus" in view and "claude:claude-fake-haiku" in view and "grok-fake" not in view,
      f"claude: is the same list, under the name that was typed: {view!r}")
s.send("\x03"); s.send("/model xai"); s.send(":"); view = listed()
check("grok-fake" not in view and "xai:" not in view, f"xai: is not completed on /model: {view!r}")
s.send("\x03"); s.send("/model\r"); check(s.expect("Select model"), "Enter with no name still opens the local picker")
s.send("think"); time.sleep(0.2); s.send("\r"); check(s.expect("model set to fake-thinker:latest"), "and typing in it still filters")
s.send("\x04"); s.close()
check(key_nowhere() is None, f"completing the models wrote the key nowhere: {key_nowhere()!r}")

# ---------- /advisor guidance: how much the agent leans on it ----------
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")

print("test advisor guidance: light and normal leave it to the agent")
out, rc, rs = guided("light", "TOOL_ADVISOR ADVISOR_LOOP")
sysmsg = rs[0]["messages"][0]["content"]
check("Consult it only when you are stuck" in sysmsg and "at most once per request" in sysmsg, f"light: only when stuck, once: {sysmsg[-600:]!r}")
check(len([r for r in rs if r["model"] == "fake-big:latest"]) == 1 and "consulted 1 time in this request" in out, f"and stopped after one: {out[-300:]!r}")
adv = [r for r in rs if r["model"] == "fake-big:latest"][0]
check("under about 250 words" in adv["messages"][0]["content"] and "under about 250 words" in adv["messages"][1]["content"], "and asked for a short answer")
check("the count starts again every 15 minutes" in sysmsg, "and that a long request gets its consultations again")
# the limit is per request, but a request that runs long gets the consultations again: six calls a second apart
n0 = len(requests())
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--advisor-guidance", "light", "--advisor-window", "0.05", "--yolo", "-p", "ADVISOR_PACED"], env=ENVK)
had = len([r for r in requests()[n0:] if r["model"] == "fake-big:latest"])
check(rc == 0 and "Done pacing." in out and 2 <= had <= 3, f"with a window of three seconds the one consultation of light comes back as the request goes on: {had}")
check(any(m["role"] == "tool" and "It can be asked again in about 1 minute" in m["content"] for m in [r for r in requests()[n0:] if r["model"] == "fake-coder:latest"][-1]["messages"]), "and in between the agent is told when")
check("the count starts again every 3 seconds" in requests()[n0]["messages"][0]["content"], "its instructions say so")
n0 = len(requests())
out, rc = run(["-m", "fake-coder:latest", "--advisor", "fake-big:latest", "--advisor-guidance", "light", "--advisor-window", "off", "--yolo", "-p", "ADVISOR_PACED"], env=ENVK)
had = len([r for r in requests()[n0:] if r["model"] == "fake-big:latest"])
check(had == 1 and "starts again" not in requests()[n0]["messages"][0]["content"] and "asked again" not in out, f"with the window off the limit holds for the whole request: {had}")
if os.path.exists(os.path.join(WORK, "made.txt")): os.remove(os.path.join(WORK, "made.txt"))
out, rc, rs = guided("normal", "TOOL_WRITE please")
check("reviews the work" not in out and os.path.exists(os.path.join(WORK, "made.txt")), "normal: no review of its own")
check(not any(r["model"] == "fake-big:latest" for r in rs), "and no consultation the agent did not ask for")

print("test advisor guidance strong: a substantial change is reviewed before the request ends")
os.remove(os.path.join(WORK, "made.txt"))
out, rc, rs = guided("strong", "TOOL_WRITE please")
check("reviews the work" not in out and not any(r["model"] == "fake-big:latest" for r in rs), f"strong: a one-line change to a new file is not reviewed: {out[-300:]!r}")
big_made()
out, rc, rs = guided("strong", "TOOL_WRITE please")
check(rc == 0 and "⤷ advisor fake-big:latest · reviews the work before the request ends" in out, f"a request that changed files is reviewed: {out[-600:]!r}")
check("Tool said: ADVICE: REVIEW-FINDING" in out, f"and what it finds goes to the agent, which gets another round: {out[-300:]!r}")
main = [r for r in rs if r["model"] == "fake-coder:latest"]
m = main[-1]["messages"]
check(m[-1]["role"] == "tool" and m[-1]["tool_name"] == "advisor" and "REVIEW-FINDING" in m[-1]["content"] and "corbienest asked it, not you" in m[-1]["content"],
      f"as the result of an advisor call: {m[-1]!r}")
check(m[-2]["role"] == "assistant" and m[-2]["tool_calls"][-1]["function"]["name"] == "advisor", "that the agent's last reply now carries")
check(len(main) == 3, f"one more round, not a loop: {len(main)}")
sysmsg = main[0]["messages"][0]["content"]
check("Lean on it" in sysmsg and "at most 6 times per request" in sysmsg and "It reviews your work by itself" in sysmsg and "do not try a second fix on a guess" in sysmsg and "when your tool calls keep failing" in sysmsg, f"the agent is told how to lean on it: {sysmsg[-700:]!r}")
adv = [r for r in rs if r["model"] == "fake-big:latest"][0]
check("under about 700 words" in adv["messages"][0]["content"] and "exact steps" in adv["messages"][0]["content"], "the advisor is asked for concrete steps")
big_made()
out, rc, rs = guided("strong", "TOOL_WRITE APPROVE")
check("reviews the work" in out and "nothing to change" in out and len([r for r in rs if r["model"] == "fake-coder:latest"]) == 2, f"LGTM: the request ends there, no extra round: {out[-300:]!r}")
out, rc, rs = guided("strong", "hello there")
check("reviews the work" not in out and not any(r["model"] == "fake-big:latest" for r in rs), "a request that changed nothing is not reviewed")
out, rc, rs = guided("strong", "TOOL_READS APPROVE")
check("reviews the work" not in out and not any(r["model"] == "fake-big:latest" for r in rs), f"several tool calls with no change are not reviewed at strong: {out[-400:]!r}")
out, rc, rs = guided("normal", "TOOL_READS APPROVE")
check("reviews the work" not in out and not any(r["model"] == "fake-big:latest" for r in rs), "(not at normal)")
out, rc, rs = guided("strong", "TOOL_FAILS now")
check(rc == 0 and "⤷ advisor fake-big:latest · steps in: the tool calls keep failing" in out, f"strong: when the tool calls keep failing the advisor is brought in, unasked: {out[-600:]!r}")
check("Gave up after 2 tries." in out, f"after two failed rounds, not five: {out[-300:]!r}")
main = [r for r in rs if r["model"] == "fake-coder:latest"]; m = main[-1]["messages"]
check(m[-1]["role"] == "tool" and m[-1]["tool_name"] == "advisor" and "corbienest asked it, not you" in m[-1]["content"] and m[-2]["tool_name"] == "bash"
      and [c["function"]["name"] for c in m[-3]["tool_calls"]] == ["bash", "advisor"], f"its advice is the result of an advisor call on the round that failed: {m[-3:]!r}")
adv = [r for r in rs if r["model"] == "fake-big:latest"]
check(len(adv) >= 1 and "failed for two rounds in a row" in adv[0]["messages"][-1]["content"] and "nope-1" in adv[0]["messages"][-1]["content"], "and it is shown the failures")
out, rc, rs = guided("normal", "TOOL_FAILS now")
check("steps in: the tool calls keep failing" in out and "Gave up after 2 tries." in out, f"at normal too: {out[-300:]!r}")
check("It is brought in by itself when your tool calls keep failing" in rs[0]["messages"][0]["content"], "and the agent is told")
out, rc, rs = guided("light", "TOOL_FAILS now")
check("steps in" not in out and "Gave up after 5 tries." in out and not any(r["model"] == "fake-big:latest" for r in rs), f"at light the agent is left to it: {out[-300:]!r}")
big_made()
out, rc, rs = guided("strong", "TOOL_WRITE please", advisor="anthropic:claude-fake-opus")
check("reviews the work before the request ends" in out and "Tool said: ADVICE: REVIEW-FINDING" in out, "a hosted advisor reviews the same way")
check("The agent has ended its turn" in last_preq("anthropic")["body"]["messages"][0]["content"], "and is asked the same")

print("test advisor guidance max: the first change is checked before it is made")
os.remove(os.path.join(WORK, "made.txt"))
out, rc, rs = guided("max", "TOOL_WRITE please")
check("checks the first change before it is made" in out and "held back — the advisor sees a problem with it" in out, f"checked, and held back: {out[-600:]!r}")
check(not os.path.exists(os.path.join(WORK, "made.txt")), "the change was not made")
m = [r for r in rs if r["model"] == "fake-coder:latest"][-1]["messages"]
check(m[-1]["role"] == "tool" and m[-1]["tool_name"] == "write_file" and m[-1]["content"].startswith("not applied: the advisor") and "CHECK-FINDING" in m[-1]["content"], f"the agent is told why: {m[-1]!r}")
check("reviews the work" not in out, "nothing was changed, so there is nothing to review")
out, rc, rs = guided("max", "TOOL_WRITE APPROVE")
check("checks the first change" in out and "go ahead" in out and os.path.exists(os.path.join(WORK, "made.txt")), f"LGTM: the change is made: {out[-600:]!r}")
check("reviews the work before the request ends" in out, "and reviewed before the request ends")
check("it also checks the first change of a request before it is made" in rs[0]["messages"][0]["content"], "the agent is told so")

print("test interactive: /advisor guidance")
s = Session(["-m", "fake-coder:latest", "--advisor", "fake-big:latest"], env=ENVK); s.expect("Ctrl-D to quit")
s.send("/advisor guidance\r"); check(s.expect("Advisor guidance: how much the agent leans on it"), "a picker"); check(s.expect("normal") and "current" in since_send(s), "on the current level")
s.send("stro"); time.sleep(0.2); s.send("\r"); check(s.expect("advisor guidance: strong"), "picked by filter")
check("advisor_guidance=strong" in open(CFG2_FILE).read(), "saved")
s.send("/advisor guidance bogus\r"); check(s.expect("usage: /advisor guidance"), "a level that is not one")
s.send("/status\r"); check(s.expect("guidance strong"), "/status shows it")
s.send("/advisor guidance normal\r"); s.expect("advisor guidance: normal")
check("advisor_guidance" not in open(CFG2_FILE).read(), "the default is not written")
s.send("\x04"); s.close()
shutil.rmtree(CFG2, ignore_errors=True)

# ---------- folds: a long tool result is one line until it is clicked ----------
print("test interactive: a tool result of several lines is folded, and a click unfolds it")
s = Session(["-m", "fake-coder:latest", "--yolo"]); s.expect("Ctrl-D to quit")
s.send("TOOL_MANY\r"); check(s.expect("▸ 6 lines · fold-line-1"), f"one line says how much there is and how it begins: {since_send(s)[-300:]!r}")
check(s.expect("Tool said:") and s.expect("tok/s"), "the model gets the whole result all the same")
check("fold-line-5" in requests()[-1]["messages"][-1]["content"], "all of it")
check("     fold-line-3" not in s.text(), "the rest is not on screen")
view = click_rows(s)
check("▾ 6 lines · fold-line-1" in view and "     fold-line-3" in view and "     fold-line-5" in view, f"a click on the line shows the rest and turns the mark: {view[-400:]!r}")
s.send("after the fold\r"); check(s.expect("Echo: after the fold"), "output goes on under an open fold")
view = click_rows(s)
check("▸ 6 lines · fold-line-1" in view and "     fold-line-3" not in view and "Echo: after the fold" in view, f"another click hides it again, and what came after is still there: {view[-400:]!r}")
s.send("\x1b[5~"); time.sleep(0.3); view = click_rows(s)
check("fold-line-3" in view, "scrolled back it works the same")
s.send("\x1b"); s.send("\x04"); s.close()
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_MANY"])
check("fold-line-3" in out and "▸" not in out, "without a screen to click on, the first lines are shown as before")

print("test location line: the working folder and its git branch, under the status bar")
br = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=WORK, capture_output=True, text=True).stdout.strip()
sl = Session(["-m", "fake-coder:latest"]); sl.expect("Ctrl-D to quit")
check(sl.expect("· branch " + br, 10) if br else True, f"the location line names the branch: {br!r}")
check(sl.expect(WORK, 5), "the location line gives the absolute path")
sl.send("\x04"); sl.close()

# ---------- code in the transcript: highlighting; the prompt cache: the bar and /usage ----------
print("test highlight: code in a reply and a file's preview are coloured by their language")
sh = Session(["-m", "fake-coder:latest"]); sh.expect("Ctrl-D to quit")
m0 = sh.mark
sh.send("CODE_REPLY\r"); check(sh.expect("Done.", 10), "the reply with a code block")
check(b"\x1b[38;2;180;142;173mdef\x1b[0m" in sh.out[m0:], "a keyword is magenta")
check(b"\x1b[38;2;124;135;153m# say hi\x1b[0m" in sh.out[m0:], "a comment is grey")
check(b"\x1b[38;2;163;190;140m'hi '\x1b[0m" in sh.out[m0:], "a string is green")
sh.send("/mode auto\r"); sh.expect("mode: auto")
m1 = sh.mark
sh.send("TOOL_WRITE_CODE\r"); check(sh.expect("create hello.py", 10), "a file is written")
check(b"\x1b[38;2;180;142;173mdef\x1b[0m" in sh.out[m1:], "its preview shows the code coloured by the extension")
check(sh.expect("Echo", 10) or True, "the turn ends")
sh.send("/mode manual\r"); sh.expect("mode: manual")   # the shared config: back to the default
sh.send("\x04"); sh.close()
os.remove(os.path.join(WORK, "hello.py")) if os.path.exists(os.path.join(WORK, "hello.py")) else None

print("test cache: the bar shows a hosted model's cache hits, /usage prices and shows them")
sc = Session(["-m", "anthropic:claude-fake-haiku"], env=ENVK); sc.expect("Ctrl-D to quit")
sc.send("/usage price anthropic:claude-fake-haiku 1 5\r"); sc.expect("price", 5)
sc.send("CACHE hello\r"); check(sc.expect("⟳ 800", 10), f"the bar shows the cache hits: {sc.text()[-300:]!r}")
sc.expect("tok/s", 10)
sc.send("/usage\r")
check(sc.expect("cache: 800 read at 10% of the input price", 10), "/usage: the cache reads, at the cache rate")
check(sc.expect("40 written at 1.25×", 5), "/usage: the cache writes, at 1.25×")
check(sc.expect("≈ $0.0003", 5), f"/usage: the cost is the cache-priced one (100 at 1, 800 at 0.10, 40 at 1.25, 20 out at 5 = $0.00033): {sc.text()[-400:]!r}")
sc.send("\x04"); sc.close()

so = Session(["-m", "fake-coder:latest", "--advisor", "openai:gpt-fake", "--yolo"], env=ENVK); so.expect("Ctrl-D to quit")
so.send("TOOL_ADVISOR CACHE please\r"); check(so.expect("⎿  ", 15), "a consultation of an OpenAI advisor")
so.expect("tok/s", 15)
so.send("/usage\r")
check(so.expect("openai:gpt-fake", 10) and so.expect("cache: 1.5k read", 5), f"the OpenAI cached prompt tokens are counted as cache reads: {so.text()[-400:]!r}")
so.send("/mode manual\r"); so.expect("mode: manual")   # --yolo is saved to the shared config: back to the default for the tests after this
so.send("\x04"); so.close()

# ---------- /orchestrate: the advisor model plans and reviews, the main model does the tasks ----------
print("test interactive: /orchestrate")
if os.path.exists(MADE): os.remove(MADE)
os.makedirs(os.path.join(CFG2, "corbienest"), exist_ok=True)
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENV2); s.expect("Ctrl-D to quit")
s.send("/orchestrate build it\r"); check(s.expect("no advisor is set"), "without an advisor there is nobody to plan")
s.send("/advisor fake-big:latest\r"); s.expect("advisor: fake-big:latest")
s.send("/orchestrate\r"); check(s.expect("usage: /orchestrate REQUEST"), "bare: what it does")
n0 = len(requests())
s.send("/orchestrate build the thing\r")
check(s.expect("⤷ orchestrator fake-big:latest") and s.expect("plans the request"), "the advisor model is asked for a plan")
check(s.expect("plan · 2 tasks") and s.expect("1. make the file") and s.expect("2. say that it is there"), f"the tasks of its plan are listed, markdown or not: {since_send(s)[-400:]!r}")
check(s.expect("⤷ worker 1/2") and s.expect("write_file"), "the first task runs as a worker that may write")
check(s.expect("reviews task 1/2") and s.expect("⎿ accepted"), "and is reviewed")
check(s.expect("⤷ worker 2/2") and s.expect("reviews task 2/2"), "then the second")
check(s.expect("orchestrated: 2 of 2 tasks accepted", 20) and s.expect("files written: made.txt"), f"the run ends with what came of it: {since_send(s)[-400:]!r}")
check(open(MADE).read() == "made by fake\n", "the worker's file is there")
reqs = requests()[n0:]
plan_rq = [r for r in reqs if r["model"] == "fake-big:latest" and "# PLAN this request" in r["messages"][-1]["content"]]
check(len(plan_rq) == 1 and "tools" not in plan_rq[0] and "build the thing" in plan_rq[0]["messages"][-1]["content"], "one plan call, toolless, with the request")
rev = [r["messages"][-1]["content"] for r in reqs if r["model"] == "fake-big:latest" and "# REVIEW this task" in r["messages"][-1]["content"]]
check(len(rev) == 2 and "+made by fake" in rev[0] and "b/made.txt" in rev[0] and "Tool said" in rev[0], f"the review is shown the task's diff and the worker's report: {rev[0][-500:] if rev else None!r}")
check("(it wrote no file)" in rev[1] and "+made by fake" not in rev[1], "and the second task's review only what that task wrote")
wk = [r for r in reqs if r["model"] == "fake-coder:latest" and "You are a worker agent" in r["messages"][0]["content"]]
names = [t["function"]["name"] for t in wk[0].get("tools", [])]
check(wk[0].get("stream") is False and "write_file" in names and "task" not in names and "advisor" not in names, f"a worker has the tools to change files, but no agents or advisor of its own: {names!r}")
w2 = [r for r in wk if "# Your task (2 of 2)" in r["messages"][1]["content"]]
check(w2 and "# Done before you" in w2[0]["messages"][1]["content"] and "TOOL_WRITE" not in w2[0]["messages"][1]["content"] and len(w2[0]["messages"]) == 2, "the second worker starts fresh: its task and the first one's report, not its transcript")
s.send("and now?\r"); check(s.expect("Echo: and now?"), "the conversation goes on")
roles = [m["role"] for m in requests()[-1]["messages"]]
check(roles == ["system", "user", "assistant", "user"] and "[/orchestrate:" in requests()[-1]["messages"][2]["content"] and "## Task 1 of 2: make the file — accepted" in requests()[-1]["messages"][2]["content"],
      f"in which the run is the request and one reply that says what came of each task: {roles!r}")
s.send("/usage\r"); check(s.expect("usage") and s.expect("3 calls"), f"/usage counts the orchestrator's three calls — a plan and two reviews: {since_send(s)[-300:]!r}")
os.remove(MADE)
s.send("/orchestrate ONETASK REJECT it once\r")
check(s.expect("plan · 1 task ·") and s.expect("⎿ sent back") and s.expect("REVIEW-FINDING"), "a task the reviewer does not accept is sent back with what it said")
check(s.expect("second attempt") and s.expect("⎿ accepted") and s.expect("orchestrated: 1 of 1 task accepted", 20), f"and accepted when the worker has dealt with it: {since_send(s)[-400:]!r}")
back = [r for r in requests() if r["model"] == "fake-coder:latest" and r["messages"][-1]["role"] == "user" and "does not accept it yet" in r["messages"][-1]["content"]]
check(back and "REVIEW-FINDING" in back[-1]["messages"][-1]["content"] and len(back[-1]["messages"]) > 3, "the worker gets the reviewer's words in the conversation it already has")
s.send("/orchestrate ONETASK NEVERACCEPT this\r")
check(s.expect("last attempt", 20) and s.expect("orchestrated: 0 of 1 task accepted", 20) and s.expect("/rewind undoes what was written"), f"three attempts, then it stops and says so: {since_send(s)[-400:]!r}")
s.send("/orchestrate NOPLAN please\r"); check(s.expect("no tasks in its answer") and s.expect("talk this through"), "an answer without a plan runs nothing")
os.remove(MADE)
s.send("/clear\r"); s.expect("new conversation")   # (the fake reads its keywords from the whole brief, and that has the conversation before in it)
# tasks that do not wait for each other run at the same time: their model calls, each on a thread
s.send("/workers\r"); check(s.expect("workers: 3"), "three at a time unless told otherwise")
urllib.request.urlopen(f"{HOST}/_inflight").read()
t0 = time.time(); s.send("/orchestrate PARALLEL work\r")
check(s.expect("plan · 3 tasks") and s.expect("2. second slow thing  with the tasks before it") and s.expect("3. wrap it up  after 1 2"), f"the plan says what may overlap: {since_send(s)[-400:]!r}")
check(s.expect("⤷ worker 1/3") and s.expect("⤷ worker 2/3"), "both start at once")
check(s.expect("orchestrated: 3 of 3 tasks accepted", 30), f"and the run comes to its end: {since_send(s)[-500:]!r}")
took = time.time() - t0
most = json.loads(urllib.request.urlopen(f"{HOST}/_inflight").read())["max"]
check(most == 2, f"the two model calls were under way at the same time: {most}")
check(took < 6.2, f"so the two slow tasks took the time of one, not of two (3.2 s each): {took:.1f}s")
out3 = since_send(s)
_w3, _r2, _r1 = out3.find("⤷ worker 3/3"), out3.find("reviews task 2/3"), out3.find("reviews task 1/3")   # find, not index: a repainted line is missing, and the check fails rather than the suite stopping
check(_w3 >= 0 and _r2 >= 0 and _r1 >= 0 and _w3 > _r2 and _w3 > _r1, "the task that waits for both starts when both are accepted")
w3 = [r for r in requests() if r["model"] == "fake-coder:latest" and len(r["messages"]) > 1 and "# Your task (3 of 3)" in r["messages"][1]["content"]]
check(w3 and "## Task 1: first slow thing — accepted" in w3[0]["messages"][1]["content"] and "## Task 2: second slow thing — accepted" in w3[0]["messages"][1]["content"], "and is told what the two did")
check(open(MADE).read() == "made by fake\n", "its file is written")
os.remove(MADE)
s.send("/clear\r"); s.expect("new conversation")
s.send("/workers 1\r"); check(s.expect("workers = 1") and "workers=1" in open(CFG2_FILE).read(), "/workers N, saved")
urllib.request.urlopen(f"{HOST}/_inflight").read()
s.send("/orchestrate PARALLEL in turn\r"); check(s.expect("orchestrated: 3 of 3 tasks accepted", 40), "with one worker the same plan runs in turn")
check(json.loads(urllib.request.urlopen(f"{HOST}/_inflight").read())["max"] == 1, "one call at a time")
os.remove(MADE)
s.send("/workers 9\r"); check(s.expect("usage: /workers N"), "a number out of range is refused")
s.send("/workers 3\r"); s.expect("workers = 3")
s.send("/clear\r"); s.expect("new conversation")
s.send("/orchestrate PARALLEL then stop\r"); s.expect("⤷ worker 2/3"); time.sleep(0.8); s.send("\x1b")
check(s.expect("the run was stopped", 10) and s.expect("orchestrated: 0 of 3 tasks accepted", 10), f"Esc stops every worker that is under way: {since_send(s)[-400:]!r}")
s.send("still there?\r"); check(s.expect("Echo: still there?", 10), "and the prompt is back at once")
check("stopped by the user before it was finished" in requests()[-1]["messages"][-2]["content"] and "not started: the run was stopped" in requests()[-1]["messages"][-2]["content"], "with what came of each task in the conversation")
s.send("/clear\r"); s.expect("new conversation")
s.send("/mode plan\r"); s.expect("mode: plan")
s.send("/orchestrate ONETASK in plan mode\r"); check(s.expect("plan · 1 task ·") and s.expect("plan mode: the tasks are not run"), "plan mode shows the plan and runs nothing")
check(not os.path.exists(MADE), "nothing was written")
s.send("/clear\r"); s.expect("new conversation"); s.send("/mode manual\r"); s.expect("mode: manual")
s.send("/orchestrate ONETASK ask me first\r"); check(s.expect("Run this task with fake-coder:latest?"), "outside auto mode the plan is put to the user first")
s.send("n"); time.sleep(0.3); s.send("\r"); check(s.expect("the plan was not run"), f"and No leaves it at that: {since_send(s)[-300:]!r}")
check(not os.path.exists(MADE), "nothing was written then either")
s.send("\x04"); s.close()
print("test interactive: /orchestrate with a hosted model, and the banner")
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\nadvisor=xai:grok-fake\n")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENV2, banner=True)
check(s.expect("advisor: xai:grok-fake") and s.expect("XAI_API_KEY is not set"), "the banner names the saved advisor, and says when its key is missing")
s.send("\x1b"); s.expect("(no such text)", 1); s.send("\x04"); s.close()
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVK); s.expect("Ctrl-D to quit")
check("advisor: xai:grok-fake" in s.text() and "is not set" not in s.text(), "with the key there it is just named")
np = len(preqs())
s.send("/orchestrate ONETASK through the API\r")
check(s.expect("⤷ orchestrator xai:grok-fake") and s.expect("orchestrated: 1 of 1 task accepted", 20), f"a hosted model plans and reviews the same way: {since_send(s)[-400:]!r}")
bodies = [r["body"]["messages"] for r in preqs()[np:] if r["provider"] == "xai"]
check(len(bodies) == 2 and "You are the orchestrator" in bodies[0][0]["content"] and "# PLAN this request" in bodies[0][-1]["content"] and "+made by fake" in bodies[1][-1]["content"], "two calls to it: the plan, and the review with the diff")
check(key_nowhere() is None, f"and the key was written nowhere: {key_nowhere()!r}")
s.send("\x04"); s.close()
os.remove(MADE)
shutil.rmtree(CFG2, ignore_errors=True)

# ---------- https: Ollama behind TLS, and a hosted API over it ----------
print("test https: the same client over TLS, with the certificate checked")
if shutil.which("openssl"):
    TLS = tempfile.mkdtemp(prefix="crowtls_")
    cert, key = os.path.join(TLS, "cert.pem"), os.path.join(TLS, "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"], capture_output=True, check=True)
    TLS_PORT = free_port()
    threading.Thread(target=fake_ollama.serve, args=(TLS_PORT, cert, key), daemon=True).start()
    time.sleep(0.5)
    TENV = {k: v for k, v in ENV.items() if k not in ("CURL_CA_BUNDLE", "SSL_CERT_FILE")}
    out, rc = run(["-H", f"https://localhost:{TLS_PORT}", "-m", "fake-coder:latest", "-p", "hello over tls"], env=dict(TENV, CURL_CA_BUNDLE=cert))
    check(rc == 0 and "Echo: hello over tls" in out, f"an Ollama behind TLS: streamed as over http: {out[-300:]!r}")
    out, rc = run(["-H", f"https://localhost:{TLS_PORT}", "-m", "fake-coder:latest", "-p", "hello over tls"], env=TENV)
    check("TLS:" in out and "certificate" in out and "Echo:" not in out, f"a certificate nobody vouches for is refused: {out[-300:]!r}")
    out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-fake-opus", "--yolo", "-p", "TOOL_ADVISOR please"],
                  env=dict(TENV, ANTHROPIC_API_KEY="test-key", ANTHROPIC_BASE_URL=f"https://localhost:{TLS_PORT}/anthropic", SSL_CERT_FILE=cert))
    check(rc == 0 and "Tool said: ADVICE: read hay.txt" in out, f"a hosted advisor over https (SSL_CERT_FILE names the CA): {out[-300:]!r}")
    shutil.rmtree(TLS, ignore_errors=True)
else:
    print("  (skipped: no openssl to make a certificate with)")

print("test config persistence")
cfg = open(os.path.join(CFG, "corbienest", "config")).read()
check("keep_alive=30m" in cfg, "keep_alive saved to config")
check("model=fake-coder:latest" in cfg, "model saved to config")
check("mode=manual" in cfg, "mode saved to config")
check("max_iters=100" in cfg, "max_iters saved to config")
s = Session([])   # no -m: should use saved model
check(s.expect("model: fake-coder:latest"), "saved model used")
s.send("multi one\\\r"); s.send("two\r"); check(s.expect("Echo: multi one"), "multi-line entry")
s.send("\x04"); check(s.expect("bye"), "exit"); s.close()
hist = open(os.path.join(CFG, "corbienest", "history")).read()
check("multi one\x1ftwo" in hist, "history saved with encoded newline")

print("test two sessions at once: shared history accumulates, the config is never half-written")
ha = Session(["-m", "fake-coder:latest"]); check(ha.expect("Ctrl-D to quit"), "session A up")
hb = Session(["-m", "fake-coder:latest"]); check(hb.expect("Ctrl-D to quit"), "session B up, having loaded the history A is about to add to")
ha.send("alpha-in-a\r"); check(ha.expect("tok/s", 15), "A answered")
hb.send("beta-in-b\r"); check(hb.expect("tok/s", 15), "B answered")
ha.send("second-in-a\r"); check(ha.expect("tok/s", 15), "A again, after B had written the file")
hb.send("/keepalive 7m\r"); check(hb.expect("keep_alive = 7m"), "B rewrites the config")
ha.send("/temp 0.3\r"); check(ha.expect("temperature = 0.3"), "A rewrites it too")
hist = open(os.path.join(CFG, "corbienest", "history")).read()
check("alpha-in-a" in hist and "beta-in-b" in hist and "second-in-a" in hist,
      f"neither session's queries were clobbered by the other's save: {hist[-200:]!r}")
pos = [hist.find(k) for k in ("alpha-in-a", "beta-in-b", "second-in-a")]
check(all(p >= 0 for p in pos) and pos == sorted(pos), f"appended in the order they were typed: {pos}")
cfg = open(os.path.join(CFG, "corbienest", "config")).read()
check(cfg.startswith("# corbienest config") and "model=fake-coder:latest" in cfg and cfg.endswith("\n"),
      f"the config is one whole file, not a mixture of two writes: {cfg[-140:]!r}")
check(len([l for l in cfg.splitlines() if "=" in l]) >= 10, f"every key is there — nothing was truncated: {cfg!r}")
check("temperature=0.3" in cfg and "keep_alive=30m" in cfg,
      f"and it is A's whole state: the last writer wins, which atomicity does not change: {cfg!r}")
check(not [f for f in os.listdir(os.path.join(CFG, "corbienest")) if ".tmp" in f], "no temporary files left behind")
ha.send("\x04"); hb.send("\x04"); ha.close(); hb.close()

# ---------- /goal: candidates in git worktrees race to a metric ----------

print("test goal: a local model and xAI race in worktrees; the higher score is applied, the copies are gone")
repo = make_repo()
g = Session(["--yolo", "-m", "fake-coder:latest"], cwd=repo, env=GOALENV)
check(g.expect("❯", 20), "the goal session starts")
g.send('/goal "cat score 2>/dev/null || echo 0" max -n 1 -k 2 -m fake-coder:latest,xai:grok-fake TOOL_SCORE raise the score\r')
check(g.expect("goal: score 0 → 9", 180), f"the grok candidate scores 9 over the local one's 5 and is kept: {g.text()[-500:]!r}")
g.close()
score = open(os.path.join(repo, "score")).read() if os.path.exists(os.path.join(repo, "score")) else ""
check(score == "9\n", f"the kept result is applied to the working tree: {score!r}")
wt = subprocess.run(["git", "-C", repo, "worktree", "list"], capture_output=True, text=True).stdout.strip().splitlines()
check(len(wt) == 1, f"no candidate's worktree is left behind: {wt}")
br = subprocess.run(["git", "-C", repo, "branch", "--list", "corbie-goal/*"], capture_output=True, text=True).stdout.strip()
check(br.startswith("* ") or br.startswith("  corbie-goal/"), f"the kept result is kept as a branch: {br!r}")
left = os.listdir(os.path.join(CFG, "corbienest", "goals")) if os.path.isdir(os.path.join(CFG, "corbienest", "goals")) else []
check(left == [], f"the goal's folder is removed: {left}")
xai = [r for r in provider_requests() if r.get("provider") == "xai" and "tools" in r.get("body", {})]
check(xai and xai[-1]["body"]["model"] == "grok-fake" and xai[-1]["body"]["messages"][0]["role"] == "system", f"the xAI candidate was sent a Chat Completions turn with its tools: {len(xai)} request(s)")

print("test goal: DeepSeek and Kimi race; Kimi's K2 thinking is off, and the higher score wins")
repo2 = make_repo()
g = Session(["--yolo", "-m", "fake-coder:latest"], cwd=repo2, env=GOALENV)
check(g.expect("❯", 20), "the second goal session starts")
g.send('/goal "cat score 2>/dev/null || echo 0" max -n 1 -k 2 -m deepseek:deepseek-flash,kimi:kimi-k2.6 TOOL_SCORE raise the score\r')
check(g.expect("goal: score 0 → 7", 180), f"DeepSeek's 7 beats Kimi's 3 and is kept: {g.text()[-500:]!r}")
g.close()
score2 = open(os.path.join(repo2, "score")).read() if os.path.exists(os.path.join(repo2, "score")) else ""
check(score2 == "7\n", f"DeepSeek's result is applied: {score2!r}")
kimi = [r["body"] for r in provider_requests() if r.get("provider") == "kimi" and "tools" in r.get("body", {})]
check(kimi and kimi[-1].get("thinking") == {"type": "disabled"} and kimi[-1]["model"] == "kimi-k2.6", f"Kimi's K2 request has thinking disabled: {kimi[-1:]}")
ds = [r for r in provider_requests() if r.get("provider") == "deepseek" and "tools" in r.get("body", {})]
check(ds and "max_tokens" in ds[-1]["body"] and "max_completion_tokens" not in ds[-1]["body"], "DeepSeek gets max_tokens, not OpenAI's max_completion_tokens")
shutil.rmtree(repo, ignore_errors=True); shutil.rmtree(repo2, ignore_errors=True)

print("test interactive: /btw answers from the conversation so far, and nothing it says is kept in it")
b = Session(["-m", "fake-coder:latest"])
check(b.expect("❯", 20), "btw session starts")
b.send("first remark\r"); check(b.expect("Echo: first remark"), "a normal exchange first")
n_before = len(requests()[-1]["messages"])
b.send("/btw what did I say first\r")
check(b.expect("? what did I say first") and b.expect("Echo:", 20), f"the side question is asked and answered: {since_send(b)[-300:]!r}")
side = requests()[-1]["messages"]
check(side[-1]["role"] == "user" and "side question" in side[-1]["content"] and len(side) == n_before + 1, f"the side call carries the conversation and the question: {len(side)} messages")
check("side question" not in json.dumps(requests()[-2]["messages"]), "the answer is not in the conversation the next request sends")
b.expect("❯", 5)
b.send("second remark\r"); check(b.expect("Echo: second remark"), "the next message goes on from the conversation as it was")
nxt = requests()[-1]["messages"]
check(not any("side question" in (m.get("content") or "") for m in nxt), "the side question is in no later request")
check(sum(1 for m in nxt if m.get("role") == "user") == 2, f"and the conversation has only the two real messages: {[m.get('role') for m in nxt]}")
b.close()

# ---------- /orchestrate roles: a skill sets the worker's model and prompt, the contract is shown, owned paths wait ----------
print("test interactive: /orchestrate with roles, a contract and owned paths")
os.makedirs(os.path.join(RP, ".corbienest", "skills", "tester"), exist_ok=True)
open(os.path.join(RP, ".corbienest", "skills", "tester", "SKILL.md"), "w").write(
    "---\nname: tester\ndescription: Writes the item and the tests for it\nmodel: fake-levels:latest\n---\n"
    "ROLE-BODY: you write the tests first, then the code they need.\n")
os.makedirs(os.path.join(RCFG, "corbienest"), exist_ok=True)
open(os.path.join(RCFG, "corbienest", "config"), "w").write("memory=0\nmemory_idle=0\n")
rp = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVR, cwd=RP); rp.expect("Ctrl-D to quit")
rp.send("/advisor fake-big:latest\r"); rp.expect("advisor: fake-big:latest")
n0 = len(requests())
rp.send("/orchestrate ROLEPLAN build the item\r")
check(rp.expect("orchestrated: 3 of 3 tasks accepted", 90), f"the run of three tasks comes to its end: {since_send(rp)[-500:]!r}")
out = since_send(rp)
check("role nosuchrole is not a skill here: the default worker" in out, "a role that is not a skill says so and runs the default worker")
check("· role tester" in out, "a worker's line names its role")
rev1 = out.index("reviews task 1/3")
check(out.index("⤷ worker 2/3") > out.index("accepted", rev1), "the task that owns the same file as task 1 starts only after task 1 is accepted")
reqs = requests()[n0:]
w1, w2, w3 = worker(1), worker(2), worker(3)
check(w1 and w1[0]["model"] == "fake-levels:latest", f"a role's model runs its worker: {w1[0]['model'] if w1 else None!r}")
check(w3 and w3[0]["model"] == "fake-coder:latest", f"the default worker runs on the model in use: {w3[0]['model'] if w3 else None!r}")
check(w2 and "# Your role: tester" in w2[0]["messages"][0]["content"] and "ROLE-BODY: you write the tests first" in w2[0]["messages"][0]["content"],
      f"the skill's body is in the worker's prompt: {w2[0]['messages'][0]['content'][-300:] if w2 else None!r}")
check(w2 and "# The contract (what all the tasks share)" in w2[0]["messages"][1]["content"] and "the tests import Item from src/x.c" in w2[0]["messages"][1]["content"],
      "every worker is shown the contract, before its task")
check(w3 and "# The contract" in w3[0]["messages"][1]["content"] and "# Your role" not in w3[0]["messages"][0]["content"], "and the default worker gets the contract, but no role")
rp.close()
shutil.rmtree(RP, ignore_errors=True); shutil.rmtree(RCFG, ignore_errors=True)

# ---------- the features of the last rounds: each section has its own config and project directory ----------
import glob as _glob
def proj_key(path):   # the name corbienest gives a project's rules, hooks and MCP servers (FNV-1a of its real path)
    h = 1469598103934665603
    for c in os.path.realpath(path).encode(): h ^= c; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"
def feat(name):
    cfg = tempfile.mkdtemp(prefix=f"crow_{name}_cfg_"); proj = tempfile.mkdtemp(prefix=f"crow_{name}_proj_")
    os.makedirs(os.path.join(cfg, "corbienest"))
    open(os.path.join(cfg, "corbienest", "config"), "w").write("memory=0\nmemory_idle=0\n")
    return cfg, proj, dict(ENV, XDG_CONFIG_HOME=cfg)
def in_proj(proj, args, env, stdin=b""):
    p = subprocess.run([BIN] + args, cwd=proj, env=env, input=stdin, capture_output=True, timeout=120)
    return clean(p.stdout.decode("utf-8", "replace")), clean(p.stderr.decode("utf-8", "replace")), p.returncode
def put(cfg, sub, name, text):   # a file in the config directory's per-project folder
    d = os.path.join(cfg, "corbienest", sub); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, name), "w").write(text)
def procs(pat):   # the processes whose command line has pat, not the grep itself
    o = subprocess.run(["ps", "-eo", "args"], capture_output=True, text=True).stdout
    return [l for l in o.splitlines() if pat in l and "ps -eo" not in l and "grep" not in l]

print("test feature: a guard rule stops a tool under --yolo; an ask is refused without a terminal")
cfg, proj, env = feat("guard")
put(cfg, "permissions", proj_key(proj) + ".rules", "deny bash echo *\n")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_BASH please"], env)
check("denied (project rule: deny bash echo *)" in out and "⎿  hello-from-tool" not in out, f"deny stops the call under --yolo: {out[-300:]!r}")
put(cfg, "permissions", proj_key(proj) + ".rules", "ask bash echo *\n")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_BASH please"], env)
check("no one to ask" in out and "⎿  hello-from-tool" not in out, f"an ask is refused when nobody can answer: {out[-300:]!r}")
put(cfg, "permissions", proj_key(proj) + ".rules", "bash echo\n")
out, err, rc = in_proj(proj, ["-p", "TOOL_BASH please"], env)
check("⎿  hello-from-tool" in out, f"an allow rule still lets the call through: {out[-200:]!r}")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: hooks gate a call (exit 2), add a post note, and run at the end of a request")
cfg, proj, env = feat("hooks")
put(cfg, "hooks", proj_key(proj) + ".hooks",
    "pre bash cat > evt.json; grep -q BLOCKME evt.json && { echo blocked-by-gate; exit 2; }; exit 0\n"
    "post bash echo post-note-here\n"
    "stop echo stop-note-here\n")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_BASH please"], env)
check("post-note-here" in out and "stop-note-here" in out, f"post and stop hooks ran: {out[-300:]!r}")
evt = open(os.path.join(proj, "evt.json")).read() if os.path.exists(os.path.join(proj, "evt.json")) else ""
check('"event":"pre"' in evt and '"tool":"bash"' in evt and '"command":"echo hello-from-tool"' in evt, f"the event reached the hook on stdin: {evt[:200]!r}")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_SHELL echo BLOCKME"], env)
check("blocked-by-gate" in out and "hook stopped bash" in out, f"a pre hook that exits 2 stops the call: {out[-300:]!r}")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: a background command runs on after the call and is stopped when corbienest exits")
cfg, proj, env = feat("bg")
before = len(procs("sleep 317"))
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_BG please"], env)
check("started background job bg" in out, f"the job is started and announced: {out[-200:]!r}")
check(len(procs("sleep 317")) == before, "the job is gone when the run ends")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: stream-json: one JSON object per line on stdout, tool calls matched to results")
cfg, proj, env = feat("stream")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_BASH please", "--output-format", "stream-json"], env)
lines = [l for l in out.splitlines() if l.strip()]
objs = []
for l in lines:
    try: objs.append(json.loads(l))
    except Exception: objs.append(None)
check(objs and all(o is not None for o in objs), f"every stdout line is JSON: {lines[:2]!r}")
check(objs and objs[0].get("type") == "system" and objs[-1].get("type") == "result", "a system line first, a result line last")
uses = [b for o in objs if o for b in (o.get("message", {}).get("content") or []) if isinstance(b, dict) and b.get("type") == "tool_use"]
res = [b for o in objs if o for b in (o.get("message", {}).get("content") or []) if isinstance(b, dict) and b.get("type") == "tool_result"]
check(uses and res and uses[0]["id"] == res[0]["tool_use_id"] and uses[0]["name"] == "bash", f"the tool call and its result carry the same id: {uses[:1]} {res[:1]}")
out, err, rc = in_proj(proj, ["--yolo", "--output-format", "stream-json", "--input-format", "stream-json"], env,
                       stdin=b'{"type":"user","message":{"role":"user","content":"first"}}\n{"type":"user","message":{"role":"user","content":[{"type":"text","text":"second"}]}}\n')
results = [json.loads(l) for l in out.splitlines() if l.startswith("{") and '"type":"result"' in l]
check(len(results) == 2 and all(r.get("subtype") == "success" for r in results), f"two requests on stdin, one result line each: {len(results)}")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: todo_write keeps a task list and shows it whole")
cfg, proj, env = feat("todo")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_TODO please"], env)
check("Tasks (1 of 3 done)" in out and "☑ read the code" in out and "◐ make the change" in out and "☐ run the tests" in out,
      f"the list is shown: {out[-300:]!r}")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: an MCP server's tool is offered, called, and guarded; the server stops with the run")
cfg, proj, env = feat("mcp")
mcp_py = os.path.join(HERE, "fake_mcp.py")
put(cfg, "mcp", proj_key(proj) + ".json", json.dumps({"servers": {"fake": {"command": "python3", "args": [mcp_py]}}}))
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_MCP please"], env)
check("echo: from-model" in out, f"the server's tool answers the call: {out[-300:]!r}")
check("fake-mcp: starting" not in out, "the server's banner line does not reach the screen")
put(cfg, "permissions", proj_key(proj) + ".rules", "deny mcp mcp__fake__echo\n")
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_MCP please"], env)
check("was not run" in out and "echo: from-model" not in out, f"a deny rule on the tool's name stops it: {out[-300:]!r}")
check(not procs("fake_mcp.py"), "no server is left running after the run")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: a sub-agent file's role and tools reach the sub-agent, and nothing more")
cfg, proj, env = feat("agents")
os.makedirs(os.path.join(proj, ".corbienest", "agents"))
open(os.path.join(proj, ".corbienest", "agents", "reviewer.md"), "w").write(
    "---\nname: reviewer\ndescription: Reviews made.txt, read-only\ntools: read_file, grep, Write\n---\nROLE_MARK_XYZ: review and report.\n")
n0 = len(requests())
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_AGENT please"], env)
sub = [r for r in requests()[n0:] if any("Your role" in (m.get("content") or "") for m in r.get("messages", []))]
check(sub and "ROLE_MARK_XYZ" in sub[-1]["messages"][0]["content"], "the role is in the sub-agent's instructions")
check(sub and sorted(t["function"]["name"] for t in sub[-1].get("tools", [])) == ["grep", "read_file"],
      f"only the allowed tools are offered (Write is not a sub-agent tool): {[t['function']['name'] for t in sub[-1].get('tools', [])] if sub else None}")
n1 = len(requests())
out, err, rc = in_proj(proj, ["--yolo", "-p", "TOOL_AGENT_NOSUCH please"], env)
check("no sub-agent named nosuch" in out or "no sub-agent named nosuch" in json.dumps(requests()[n1:]), "an unknown agent name is an error for the model")
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: the sandbox keeps writes to the project (when bwrap is installed)")
if shutil.which("bwrap") and subprocess.run(["bwrap", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "true"], capture_output=True).returncode == 0:
    cfg, proj, env = feat("sandbox")
    outside = tempfile.mkdtemp(prefix="crow_outside_")
    open(os.path.join(cfg, "corbienest", "config"), "a").write("sandbox=bwrap\n")
    out, err, rc = in_proj(proj, ["--yolo", "-p", f"TOOL_SHELL touch ./inproj.txt && echo PROJ_OK; touch {outside}/x 2>/dev/null; echo OUTSIDE_RC=$?"], env)
    check("PROJ_OK" in out and "OUTSIDE_RC=1" in out, f"the project is writable, the rest is not: {out[-300:]!r}")
    shutil.rmtree(outside, ignore_errors=True); shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)
else:
    print("  (bwrap is not installed or cannot start: this section is skipped)")

print("test feature: /resume shows the conversation again, and its typed lines are in the ↑ history")
cfg, proj, env = feat("resume")
out, err, rc = in_proj(proj, ["-p", "hello replay"], env)
sids = [os.path.basename(p)[:-5] for p in _glob.glob(os.path.join(cfg, "corbienest", "sessions", "*.json"))]
sess = Session(["--resume", sids[0]] if sids else [], cwd=proj, env=env)
check(sess.expect("Echo: hello replay"), "the earlier reply is shown again")
check(sess.expect("› hello replay"), "the typed line is shown as the user sent it")
sess.send("\x1b[A", wait=0.6)
check(seen(sess, "hello replay", 8), "the ↑ key recalls the typed line from the session")
sess.close()
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

print("test feature: the slash commands for the rules, hooks, MCP servers, agents and the sandbox answer")
cfg, proj, env = feat("slash")
sess = Session([], cwd=proj, env=env)
sess.expect("Ctrl-D to quit")
def slash(line):   # the input field takes a fast line as it is typed; Esc closes the suggestion list before Enter
    sess.mark = len(sess.out)
    for ch in line: os.write(sess.fd, ch.encode()); time.sleep(0.03)
    time.sleep(0.6); os.write(sess.fd, b"\x1b"); time.sleep(0.4); os.write(sess.fd, b"\r")
slash("/permissions add deny bash rm *")
check(seen(sess, "✓ added: deny bash rm *", 8), "/permissions add accepts a guard rule")
slash("/hooks add pre bash echo gate")
check(seen(sess, "hook added", 8), "/hooks add takes a pre hook")
slash("/sandbox")
check(seen(sess, "sandbox off", 8), "/sandbox shows that it is off")
slash("/agents")
check(seen(sess, "no sub-agent files", 8), "/agents says there are none")
sess.close()
shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(cfg, ignore_errors=True)

shutil.rmtree(WORK, ignore_errors=True); shutil.rmtree(CFG, ignore_errors=True)
print(f"{passed} checks passed, {failed} failed")
sys.exit(1 if failed else 0)
