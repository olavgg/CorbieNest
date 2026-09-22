#!/usr/bin/env python3
"""Integration tests: run the real corbienest binary against tests/fake_ollama.py.

Covers the non-interactive (-p) agent loop, tool-call recovery, error handling,
config/env handling, and — via a pseudo-terminal — the interactive editor,
confirmations, Ctrl-C interruption, type-ahead, slash commands and the /model
picker. Requires only python3 (stdlib). No real Ollama needed.
"""
import json, os, pty, re, select, shutil, socket, struct, subprocess, sys, tempfile, termios, fcntl, time, threading, urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "corbienest")
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

WORK = tempfile.mkdtemp(prefix="crowtest_")
CFG = tempfile.mkdtemp(prefix="crowcfg_")
ENV = dict(os.environ, XDG_CONFIG_HOME=CFG, OLLAMA_HOST=HOST, TERM="xterm-256color", HOME=WORK)
ENV.pop("CORBIENEST_MODEL", None)
# memory extraction adds a background model call after every request; keep it off for the
# general tests (they assert on requests()[-1]) and exercise it explicitly at the end.
# memory_idle=0 too, so a pending extraction never fires from a slow expect() mid-test.
os.makedirs(os.path.join(CFG, "corbienest")); open(os.path.join(CFG, "corbienest", "config"), "w").write(
    f"memory=0\nmemory_idle=0\nsearch_url=http://127.0.0.1:{DOC_PORT}/search?q=%s\n")

passed = failed = 0
def check(cond, msg):
    global passed, failed
    if cond: passed += 1
    else: failed += 1; print(f"  FAIL: {msg}")

def run(args, stdin=b"", env=None):
    p = subprocess.run([BIN] + args, cwd=WORK, env=env or ENV, input=stdin, capture_output=True, timeout=60)
    run.err = clean(p.stderr.decode())
    return clean(p.stdout.decode()), p.returncode

def requests(): return json.loads(urllib.request.urlopen(f"{HOST}/_requests").read())

# ---------- non-interactive ----------
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
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_FETCH file:///etc/passwd"])
check("will not open" in requests()[-1]["messages"][-1]["content"], "non-http scheme refused")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_FETCH http://169.254.169.254/latest/meta-data/"])
check("will not open" in requests()[-1]["messages"][-1]["content"], "cloud metadata refused")
out, rc = run(["-m", "fake-coder:latest", "-p", f"TOOL_FETCH {DOC_URL}"])
check("denied" in out and "non-interactively" in requests()[-1]["messages"][-1]["content"], "denied when non-interactive without --yolo")

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
out = clean(subprocess.run([BIN, "-H", "http://127.0.0.1:1", "-p", "hi"], cwd=WORK, env=ENV, capture_output=True, timeout=30).stdout.decode())
check("connect" in out or "no models" in out, "unreachable host reported")

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
SESS = os.path.join(CFG, "corbienest", "sessions")
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
class Session:
    def __init__(self, args=(), cols=100, rows=40, env=None):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(WORK)
            for k, v in (env or ENV).items(): os.environ[k] = v
            os.execv(BIN, ["corbienest"] + list(args))
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.out = b""; self.mark = 0
    def expect(self, pat, t=15):
        """Wait until `pat` appears in output produced since the last send()."""
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try: d = os.read(self.fd, 65536)
                except OSError: return False
                if not d: return False
                self.out += d
            if pat in clean(self.out[self.mark:].decode("utf-8", "replace")): return True
        return False
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

print("test interactive: banner, echo, editor keys, history")
s = Session(["-m", "fake-coder:latest"])
check(s.expect("Ctrl-D to quit"), "banner"); check("● connected" in s.text(), "connected marker")
raw = s.out.decode("utf-8", "replace")
check("\x1b[?1049h" in raw, "alternate screen entered (full screen)")
check(re.search(r"\x1b\[1;36r", raw) is not None, "scroll region reserves the input field and the status bar")
check(re.search(r"\x1b\[40;1H[^\n]*manual mode[^\n]*fake-coder:latest[^\n]*0 tokens", raw) is not None, f"status bar on the last row: {raw[-300:]!r}")
check("manual mode (shift+tab to cycle)" in clean(raw), "mode-cycling hint sits next to the mode in the bar")
check(re.search(r"\x1b\[37;1H[^\n]*─{20}", raw) is not None, "the input field is framed by a rule above the prompt")
check(re.search(r"\x1b\[38;1H[^\n]*❯", raw) is not None, "the prompt sits between the rules, above the bar")
check(re.search(r"\x1b\[39;1H[^\n]*─{20}", raw) is not None, "and a rule below it")
s.send("hello wrld"); s.send("\x1b[D\x1b[D\x1b[D"); s.send("o"); s.send("\r")
check(s.expect("Echo: hello world"), "cursor-left insert then send"); check(s.expect("tok/s"), "stats")
check(s.expect("168 tokens (↑123 ↓45)"), f"status bar counts session tokens: {s.text()[-200:]!r}")
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
check(re.search(r"\x1b\[1;16r", sf.out.decode("utf-8", "replace")) is not None, "region is rows minus field and bar")
sf.send("\x1b\rtwo\x1b\rthree")   # alt+enter twice: three input rows
check(sf.expect("three"), "multi-line input")
raw = sf.out.decode("utf-8", "replace")
check(re.search(r"\x1b\[1;14r", raw) is not None, f"the region shrank by the two extra input rows: {raw[-200:]!r}")
check(re.search(r"\x1b\[16;1H[^\n]*three", raw) is not None, "the last input row sits just above the lower rule")
sf.send("\x15")   # ctrl-u back to one row
check(sf.expect("❯ "), "cleared")
raw = sf.out.decode("utf-8", "replace")[-4000:]
check(re.search(r"\x1b\[1;16r", raw) is not None, "the region grew back")
check(re.search(r"\x1b\[15;1H\x1b\[K", raw) and re.search(r"\x1b\[16;1H\x1b\[K", raw),
      f"the rows the field gave back are wiped, not left showing its old text: {raw[-300:]!r}")
fcntl.ioctl(sf.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
os.kill(sf.pid, __import__("signal").SIGWINCH)
sf.send("after resize")
check(sf.expect("❯ after resize"), "the field follows a resize")
raw = sf.out.decode("utf-8", "replace")
check(re.search(r"\x1b\[1;26r", raw) is not None, f"the region was re-cut for the new height: {raw[-300:]!r}")
sf.send("\r"); check(sf.expect("Echo: after resize"), "and the line still sends")
check("› after resize" in sf.text(), "the submitted line goes into the transcript, not the field")
sf.close()

print("test interactive: a multi-line paste leaves nothing behind when it is sent")
sp = Session(["-m", "fake-coder:latest"], cols=60, rows=20); sp.expect("Ctrl-D to quit")
sp.send("\x1b[200~" + "\n".join("pasted line %d" % i for i in range(1, 9)) + "\x1b[201~")
check(sp.expect("pasted line 8"), "the paste lands in the field, newlines and all")
raw = sp.out.decode("utf-8", "replace")
check(re.search(r"\x1b\[1;9r", raw) is not None, f"the field grew to 8 rows and the region shrank: {raw[-300:]!r}")
sp.send("\r"); check(sp.expect("Echo: pasted line 1"), "the whole paste is sent as one message")
check(requests()[-1]["messages"][-1]["content"].count("pasted line 8") == 1, "sent once, not twice")
raw = sp.out.decode("utf-8", "replace")[-6000:]
missing = [r for r in range(10, 17) if not re.search(r"\x1b\[%d;1H\x1b\[K" % r, raw)]
check(not missing, f"every row the field gave back is wiped, so the paste is not repainted below the reply: rows {missing} left dirty")
sp.close()


print("test interactive: confirmation menu — deny with reason, arrow keys, then always")
s.send("TOOL_BASH\r")
check(s.expect("Run this command?"), "confirmation prompt")
check(s.expect("1. Yes") and "3. Yes, and always allow `echo …` in this project" in s.text() and "4. No, and tell the model" in s.text(), f"menu options shown: {s.text()[-400:]!r}")
s.send("n"); check(s.expect("tell the model why"), "reason prompt"); s.send("because\r")
check(s.expect("denied"), "denied shown"); check(s.expect("Tool said: User denied"), "model saw denial")
check("because" in requests()[-1]["messages"][-1]["content"], "reason forwarded")
s.send("TOOL_BASH\r"); check(s.expect("Run this command?"), "prompt again")
s.send("\x1b[B"); s.send("\x1b[B"); s.send("\x1b[A"); s.send("\r")   # down, down, up -> option 2 (always)
check(s.expect("hello-from-tool"), "command ran after selecting 'always' with arrows+enter")
check("yes, always" in s.text(), "collapsed answer line")
s.send("TOOL_BASH\r"); check(s.expect("Tool said: hello-from-tool", 15), "no prompt after 'always'")
check(s.text().count("Run this command?  yes") == 1, "always suppresses further prompts")

print("test interactive: project permission rules (.corbienest/permissions)")
s3 = Session(["-m", "fake-coder:latest"]); s3.expect("Ctrl-D to quit")
s3.send("TOOL_BASH\r"); check(s3.expect("Run this command?"), "prompt")
s3.send("p"); check(s3.expect("yes, always in this project"), "answered with p")
check(s3.expect("saved to .corbienest/permissions: bash echo"), "rule saved")
check(open(os.path.join(WORK, ".corbienest", "permissions")).read().strip().splitlines()[-1] == "bash echo", "file content")
s3.expect("tok/s")
s3.send("TOOL_BASH\r"); check(s3.expect("auto-approved (project rule: bash echo)"), "rule auto-approves next time"); s3.expect("tok/s")
s3.send("/permissions\r"); check(s3.expect("shell commands starting with echo"), "/permissions lists it")
s3.send("/permissions add bash git status\r"); check(s3.expect("added: bash git status"), "/permissions add")
s3.send("/permissions remove 2\r"); check(s3.expect("removed rule 2"), "/permissions remove")
s3.send("\x04"); s3.close()
s3 = Session(["-m", "fake-coder:latest"]); s3.expect("Ctrl-D to quit")   # rules persist across sessions
s3.send("TOOL_BASH\r"); check(s3.expect("auto-approved (project rule: bash echo)"), "rule survives a restart"); s3.expect("tok/s")
s3.send("/permissions clear\r"); check(s3.expect("permissions cleared"), "/permissions clear")
check(not os.path.exists(os.path.join(WORK, ".corbienest", "permissions")), "file removed")
s3.send("TOOL_BASH\r"); check(s3.expect("Run this command?"), "asks again after clear"); s3.send("y"); s3.expect("tok/s")
s3.send("\x04"); s3.close()

print("test interactive: web_fetch asks per page, and 'p' remembers the host")
s3 = Session(["-m", "fake-coder:latest"]); s3.expect("Ctrl-D to quit")
s3.send(f"TOOL_FETCH {DOC_URL}\r")
check(s3.expect("Fetch this page?"), f"confirmation shown: {s3.text()[-300:]!r}")
check(DOC_URL in s3.text(), "the whole url is shown before it is fetched")
check(s3.expect(f"3. Yes, and always allow fetching from 127.0.0.1:{DOC_PORT} in this project"), f"per-host rule offered: {s3.text()[-400:]!r}")
s3.send("p"); check(s3.expect(f"saved to .corbienest/permissions: fetch 127.0.0.1:{DOC_PORT}"), "host rule saved")
check(s3.expect("Fake Docs", 15), "the page text is previewed"); s3.expect("tok/s")
s3.send(f"TOOL_FETCH {DOC_URL}\r")
check(s3.expect(f"auto-approved (project rule: fetch 127.0.0.1:{DOC_PORT})"), "the host is not asked about again"); s3.expect("tok/s")
s3.send("/permissions\r"); check(s3.expect(f"web pages from 127.0.0.1:{DOC_PORT}"), "/permissions lists the host")
s3.send("/permissions clear\r"); s3.expect("permissions cleared")   # else that rule approves the search below too
s3.send("TOOL_SEARCH keycloak\r")
check(s3.expect("Search the web?"), f"a search is confirmed too: {s3.text()[-300:]!r}")
check(s3.expect(f"⌕ keycloak") and f"127.0.0.1:{DOC_PORT}" in s3.text(), "the query and the engine are shown")
s3.send("y"); check(s3.expect("Keycloak Admin REST API", 15), "results previewed"); s3.expect("tok/s")
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
check(len(frames) >= 2, f"status bar shows an animated spinner while the model streams: {frames}")
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
check(s.expect("stopped: your message goes to the model first", 8), f"the running command was stopped for it: {s.text()[-300:]!r}")
check(s.expect("› mid-task note", 5), "queued message injected at once, not after the command")
check("slept" not in clean(s.out[s.mark:].decode("utf-8", "replace")), "the command never ran to completion")
check(s.expect("Echo: mid-task note", 15), "model saw the mid-task message on its next call")
msgs = requests()[-1]["messages"]
check(msgs[-1]["role"] == "user" and msgs[-1]["content"] == "mid-task note" and msgs[-2]["role"] == "tool", f"injected right after the tool result: {[m['role'] for m in msgs[-4:]]}")
check("did not finish" in msgs[-2]["content"], f"the tool result says the command was cut short: {msgs[-2]['content'][:100]!r}")

print("test interactive: a queued message stops the tool calls of the round that have not started")
s.send("TOOL_SLEEP2\r"); check(s.expect("running…", 10), "the first of two calls is running")
s.send("second thoughts\r")
check(s.expect("○ bash(echo second-tool)", 8), f"the call that had not started is shown as skipped: {s.text()[-400:]!r}")
check(s.expect("⎿  not run — your message goes to the model first", 5), "with the reason under it")
check(s.expect("› second thoughts", 8), "and the message goes in instead")
check(s.expect("Echo: second thoughts", 15), "the model was called with it right away")
msgs = requests()[-1]["messages"]
check(len([m for m in msgs if m["role"] == "tool"]) >= 2 and msgs[-2]["role"] == "tool" and "not run" in msgs[-2]["content"],
      f"the skipped call still got a result of its own: {[m['role'] for m in msgs[-4:]]}")

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
check(s.expect("session two") and s.expect("session one"), "earlier sessions listed, this directory")
s.send("\x1b"); check(s.expect("cancelled"), "esc cancels")
s.send("/resume " + sid + "\r"); check(s.expect("resumed session " + sid), "/resume ID loads it")
check(s.expect("last reply:") and s.expect("Echo: three"), "recap shows the last reply")
s.send("four\r"); check(s.expect("Echo: four"), "continues")
msgs = requests()[-1]["messages"]
check([m["content"] for m in msgs if m["role"] == "user"] == ["session one", "and two", "three", "four"], "resumed history sent")
s.send("/status\r"); check(s.expect("session    " + sid), "/status shows the session id")
s.send("/clear\r"); s.expect("new conversation")
s.send("fresh start\r"); check(s.expect("Echo: fresh start"), "reply")
check(any(json.load(open(os.path.join(SESS, f)))["title"] == "fresh start" for f in os.listdir(SESS)), "/clear starts a new session file")

print("test interactive: /init writes CORBIENEST.md and loads it")
s.send("/mode auto\r"); s.expect("mode: auto")
s.send("/init\r"); check(s.expect("analysing the project"), "/init starts")
check(s.expect("project instructions loaded", 15), f"instructions loaded: {s.text()[-300:]!r}")
check(open(os.path.join(WORK, "CORBIENEST.md")).read().startswith("# Project"), "file written by the model")
s.send("after init\r"); check(s.expect("Echo: after init"), "reply")
check("Build with make." in requests()[-1]["messages"][0]["content"], "new instructions in the system prompt")
os.remove(os.path.join(WORK, "CORBIENEST.md")); s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: /cost")
s.send("/cost\r"); check(s.expect("session cost"), "/cost header")
check(s.expect("model calls") and re.search(r"model calls   \d+  \(\d+ requests", s.text()) is not None, f"calls and requests counted: {s.text()[-400:]!r}")
check(re.search(r"tokens        [\d.]+k?  \(↑", s.text()) is not None, "token totals shown")
check(re.search(r"wall time     \d", s.text()) is not None and "tok/s" in s.text(), "wall time and tok/s shown")

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
os.remove(os.path.join(WORK, "hay.txt"))
s.send("/mode auto\r"); s.expect("mode: auto")
s.send("TOOL_TASK_SLEEP\r"); check(s.expect("⤷ sub-agent slow research", 15), "sub-agent started")
check(s.expect("running…", 10), "its shell command is running")
s.send("never mind, do this instead\r")
check(s.expect("stopped after 1 tool round", 10), f"a queued message stops the sub-agent between its rounds: {s.text()[-400:]!r}")
check("sub-slept" not in clean(s.out[s.mark:].decode("utf-8", "replace")), "its command was stopped as well")
check(s.expect("› never mind, do this instead", 8), "and the message reaches the model")
check(s.expect("Echo: never mind, do this instead", 15), "which is called with it right away")
msgs = requests()[-1]["messages"]
check(any(m["role"] == "tool" and "stopped after 1 tool round" in m["content"] for m in msgs),
      f"the parent gets what the sub-agent had gathered, not an error: {[m['role'] for m in msgs[-4:]]}")
s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: /ctx picker and sizes")
s.send("/ctx 64k\r"); check(s.expect("context window: 64k (num_ctx 65536)"), "/ctx 64k")
s.send("hi ctx\r"); check(s.expect("Echo: hi ctx"), "reply"); check(requests()[-1]["options"]["num_ctx"] == 65536, "num_ctx sent")
s.send("/ctx max\r"); check(s.expect("context window: 64k"), "/ctx max uses the model's trained length (from /api/show)")
s.send("/ctx 128k\r"); check(s.expect("larger than the model's trained length"), "warning beyond model max")
s.send("/ctx bogus\r"); check(s.expect("bad size"), "bad size rejected")
s.send("/ctx\r"); check(s.expect("Context window (fake-coder:latest supports up to 64k)"), "picker title shows model max")
check("32k" in s.text() and "model maximum" in s.text() and "server default" in s.text(), f"picker entries: {s.text()[-400:]!r}")
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
check(len(set(re.findall(r"([⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]) running command", raw2))) >= 2, "bar spinner animates during a shell command")
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
check(s.expect("bye"), "exit"); s.close()
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
def since(): return clean(s.out[s.mark:].decode("utf-8", "replace"))
for pfx in "abcd":
    s.send("!seq -f '%s-%%02g' 1 30\r" % pfx); check(s.expect(pfx + "-30", 10), "30 rows printed")
s.send("hello before\r"); check(s.expect("Echo: hello before"), "reply")
def viewrange():   # the viewer's own "rows X-Y of Z" header (the window depends on the region height)
    m = re.findall(r"rows (\d+)-(\d+) of (\d+)", since())
    return tuple(int(x) for x in m[-1]) if m else None
s.send("\x1b[5~"); check(s.expect("scrollback · rows"), f"PgUp opens the viewer: {s.text()[-200:]!r}")
view = since(); first = viewrange()
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
check("❯" in s.text().split("scrollback")[-1], "PgDn at the bottom leaves the viewer")
s.send("typed\r"); check(s.expect("Echo: typed"), "prompt usable again")
s.send("\x04"); s.close()

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
CFG2 = tempfile.mkdtemp(prefix="crowcfg2_")
ENV2 = dict(ENV, XDG_CONFIG_HOME=CFG2)
CFG2_FILE = os.path.join(CFG2, "corbienest", "config")
os.makedirs(os.path.join(CFG2, "corbienest")); open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")
def thinks(rs): return [r.get("think", "absent") for r in rs]
def run_rounds(model, args):
    """run() in ENV2; returns (out, rc, the requests this run made to `model`) — not the log's last n, which may be an earlier run's"""
    n0 = len(requests())
    out, rc = run(args, env=ENV2)
    return out, rc, [r for r in requests()[n0:] if r["model"] == model]

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
s = Session(["-m", "fake-levels:latest"], env=ENV2)
check(s.expect("effort 'ultra' is saved for fake-levels:latest, which does not offer it"), "a saved effort the model does not offer is said at start-up, and left to the model")
check(s.expect("advisor: fake-big:latest"), "the banner names the advisor"); s.expect("Ctrl-D to quit")
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
check(s.expect("fake-levels:latest · high"), f"the status bar shows the effort with the model: {s.text()[-300:]!r}")
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
s.send("/effort low\r"); check(s.expect("effort: low", 3), "/effort LEVEL runs while the model is working")
check("queued" not in since_send(s) and "eight" not in since_send(s), "there and then: not queued for after the turn")
s.send("/effort\r"); check(s.expect("fake-levels:latest offers: low · medium · high", 3), "bare /effort prints the levels while busy")
check("Effort for" not in since_send(s) and "eight" not in since_send(s), "instead of opening a picker inside a live request")
check(s.expect("eight", 10), "generation carried on"); s.expect("tok/s")
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

print("test interactive: /advisor")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENV2); s.expect("Ctrl-D to quit")
s.send("/advisor\r"); check(s.expect("Select advisor"), "/advisor opens a picker"); check(s.expect("No advisor"), "with 'No advisor' first")
s.send("\x1b"); check(s.expect("advisor unchanged: none"), "Esc leaves it")
s.send("/advisor nope:latest\r"); check(s.expect("does not know a model 'nope:latest'"), "an unknown model is refused")
s.send("/advisor fake-big:latest\r"); check(s.expect("advisor: fake-big:latest"), "/advisor MODEL"); check(s.expect("at most 3 times per request"), "with what it costs")
check("advisor=fake-big:latest" in open(CFG2_FILE).read(), "saved")
s.send("/advisor effort high\r"); check(s.expect("advisor effort: high"), "/advisor effort LEVEL")
check("effort.fake-big:latest=high" in open(CFG2_FILE).read(), "kept with the advisor's model, like any model's")
s.send("/advisor ctx 8k\r"); check(s.expect("advisor context window: 8k"), "/advisor ctx")
s.send("TOOL_ADVISOR now\r"); check(s.expect("⤷ advisor fake-big:latest"), "consulted"); check(s.expect("Tool said: ADVICE"), "advice used")
adv = [r for r in requests() if r["model"] == "fake-big:latest"][-1]
check(adv.get("think") == "high" and adv["options"]["num_ctx"] == 8192, f"its effort and its window: {adv.get('think')!r} {adv['options']!r}")
s.send("/status\r"); check(s.expect("advisor    fake-big:latest"), "/status shows it")
s.send("/cost\r"); check(s.expect("advisor       1 consultation"), f"/cost counts it: {since_send(s)[-300:]!r}")
s.send("TOOL_ADVISOR SLOWADVICE\r"); check(s.expect("⤷ advisor"), "a slow consultation")
time.sleep(0.6); s.send("never mind, do this\r")
check(s.expect("stopped — your message goes first", 10), f"a queued message stops the consultation: {s.text()[-400:]!r}")
check(s.expect("Echo: never mind, do this", 10), "and reaches the model on its next call")
msgs = requests()[-1]["messages"]
check(msgs[-2]["role"] == "tool" and "not heard out" in msgs[-2]["content"] and msgs[-1]["content"] == "never mind, do this", f"the conversation stays well-formed: {[m['role'] for m in msgs[-3:]]}")
check("What it had written so far:\nADVICE:" in msgs[-2]["content"], "with what the advisor had written by then")
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
KEYS = dict(XAI_API_KEY="test-key", OPENAI_API_KEY="test-key", ANTHROPIC_API_KEY="test-key",
            XAI_BASE_URL=f"{HOST}/xai/v1", OPENAI_BASE_URL=f"{HOST}/openai/v1", ANTHROPIC_BASE_URL=f"{HOST}/anthropic")
ENVK = dict(ENV2, **KEYS)
def preqs(): return json.loads(urllib.request.urlopen(f"{HOST}/_provider_requests").read())
def last_preq(prov): return [r for r in preqs() if r["provider"] == prov][-1]
def key_nowhere():
    """the key is never written anywhere: not the config, not a session, not the history"""
    for root, _, files in os.walk(CFG2):
        for f in files:
            if "test-key" in open(os.path.join(root, f), errors="replace").read(): return os.path.join(root, f)
    return None

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
check(rc == 0 and "OPENAI_BASE_URL is plain http:// to another machine (192.0.2.1)" in out and time.time() - t0 < 4,
      f"a key is not sent in the clear to another machine — refused before any connection: {out[-300:]!r}")
out, rc = run(["-m", "fake-coder:latest", "--advisor", "anthropic:claude-nope", "--yolo", "-p", "TOOL_ADVISOR please"], env=ENVK)
check(rc == 0 and "Anthropic does not know a model 'claude-nope'" in out, f"no such model: {out[-300:]!r}")
check(key_nowhere() is None, f"the key was written nowhere: {key_nowhere()!r}")

print("test interactive: /advisor with a hosted API")
s = Session(["-m", "fake-coder:latest", "--yolo"], env=ENVK); s.expect("Ctrl-D to quit")
s.send("/advisor xai:grok-nope\r"); check(s.expect("xAI does not know a model 'grok-nope'"), "the model endpoint is asked: an unknown model is refused")
s.send("/advisor xai:grok-fake\r"); check(s.expect("advisor: xai:grok-fake"), "/advisor PROVIDER:MODEL")
check(s.expect(f"sent to {HOST}/xai/v1 with each consultation, billed to the key in XAI_API_KEY"), f"with where the conversation goes: {since_send(s)[-300:]!r}")
cfg = open(CFG2_FILE).read(); check("advisor=xai:grok-fake" in cfg and "test-key" not in cfg, "saved, without the key")
s.send("/advisor effort\r"); check(s.expect("advisor effort for xai:grok-fake"), "/advisor effort offers its levels"); check(s.expect("xhigh"), "the ones xAI's models take")
s.send("\x1b"); s.expect("unchanged")
s.send("/advisor effort max\r"); check(s.expect("sent as xhigh, its strongest level"), "max is as hard as it goes, in xAI's name")
s.send("TOOL_ADVISOR now\r"); check(s.expect("Tool said: ADVICE"), "consulted"); check(last_preq("xai")["body"].get("reasoning_effort") == "xhigh", "with that effort")
s.send("TOOL_ADVISOR SLOWADVICE\r"); check(s.expect("⤷ advisor xai:grok-fake"), "a slow consultation")
time.sleep(0.8); s.send("never mind, do this\r")
check(s.expect("stopped — your message goes first", 10), f"a queued message stops a hosted consultation too: {s.text()[-400:]!r}")
check(s.expect("Echo: never mind, do this", 15), "and reaches the model")
s.send("TOOL_ADVISOR SLOWADVICE again\r"); s.expect("⤷ advisor"); time.sleep(0.8); s.send("\x1b")
check(s.expect("⎿ interrupted", 10), "Esc skips it"); check(s.expect("Tool said: error: the consultation was interrupted", 15), "and the turn carries on")
s.send("/cost\r"); check(s.expect("advisor       1 consultation "), f"/cost counts the one that gave advice (the stopped and the skipped one did not): {since_send(s)[-300:]!r}")
s.send("/advisor anthropic:claude-fake-haiku\r"); check(s.expect("advisor: anthropic:claude-fake-haiku"), "switched to Anthropic")
s.send("/advisor effort on\r"); check(s.expect("advisor effort: on"), "a thinking budget, on")
s.send("\x04"); s.close()
check(key_nowhere() is None, f"still nowhere: {key_nowhere()!r}")

# ---------- /advisor guidance: how much the agent leans on it ----------
open(CFG2_FILE, "w").write("memory=0\nmemory_idle=0\n")
def guided(level, prompt, advisor="fake-big:latest"):
    n0, p0 = len(requests()), len(preqs())
    out, rc = run(["-m", "fake-coder:latest", "--advisor", advisor, "--advisor-guidance", level, "--yolo", "-p", prompt], env=ENVK)
    return out, rc, requests()[n0:]

print("test advisor guidance: light and normal leave it to the agent")
out, rc, rs = guided("light", "TOOL_ADVISOR ADVISOR_LOOP")
sysmsg = rs[0]["messages"][0]["content"]
check("Consult it only when you are stuck" in sysmsg and "at most once per request" in sysmsg, f"light: only when stuck, once: {sysmsg[-600:]!r}")
check(len([r for r in rs if r["model"] == "fake-big:latest"]) == 1 and "consulted 1 time in this request" in out, f"and stopped after one: {out[-300:]!r}")
adv = [r for r in rs if r["model"] == "fake-big:latest"][0]
check("under about 250 words" in adv["messages"][0]["content"] and "under about 250 words" in adv["messages"][1]["content"], "and asked for a short answer")
if os.path.exists(os.path.join(WORK, "made.txt")): os.remove(os.path.join(WORK, "made.txt"))
out, rc, rs = guided("normal", "TOOL_WRITE please")
check("reviews the work" not in out and os.path.exists(os.path.join(WORK, "made.txt")), "normal: no review of its own")
check(not any(r["model"] == "fake-big:latest" for r in rs), "and no consultation the agent did not ask for")

print("test advisor guidance strong: the work is reviewed before the request ends")
os.remove(os.path.join(WORK, "made.txt"))
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
check("Lean on it" in sysmsg and "at most 6 times per request" in sysmsg and "It reviews your work by itself" in sysmsg, f"the agent is told how to lean on it: {sysmsg[-700:]!r}")
adv = [r for r in rs if r["model"] == "fake-big:latest"][0]
check("under about 700 words" in adv["messages"][0]["content"] and "exact steps" in adv["messages"][0]["content"], "the advisor is asked for concrete steps")
os.remove(os.path.join(WORK, "made.txt"))
out, rc, rs = guided("strong", "TOOL_WRITE APPROVE")
check("reviews the work" in out and "nothing to change" in out and len([r for r in rs if r["model"] == "fake-coder:latest"]) == 2, f"LGTM: the request ends there, no extra round: {out[-300:]!r}")
out, rc, rs = guided("strong", "hello there")
check("reviews the work" not in out and not any(r["model"] == "fake-big:latest" for r in rs), "a request that changed nothing is not reviewed")
os.remove(os.path.join(WORK, "made.txt"))
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
check("and checks the first change of a request before it is made" in rs[0]["messages"][0]["content"], "the agent is told so")

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

shutil.rmtree(WORK, ignore_errors=True); shutil.rmtree(CFG, ignore_errors=True)
print(f"{passed} checks passed, {failed} failed")
sys.exit(1 if failed else 0)
