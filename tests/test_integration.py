#!/usr/bin/env python3
"""Integration tests: run the real corbienest binary against tests/fake_ollama.py.

Covers the non-interactive (-p) agent loop, tool-call recovery, error handling,
config/env handling, and — via a pseudo-terminal — the interactive editor,
confirmations, Ctrl-C interruption, type-ahead, slash commands and the /model
picker. Requires only python3 (stdlib). No real Ollama needed.
"""
import json, os, pty, re, select, shutil, socket, struct, subprocess, sys, tempfile, termios, fcntl, time, threading, urllib.request

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

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")   # CSI sequences, DECSC/DECRC
def clean(s): return ANSI.sub("", s).replace("\r", "")

WORK = tempfile.mkdtemp(prefix="crowtest_")
CFG = tempfile.mkdtemp(prefix="crowcfg_")
ENV = dict(os.environ, XDG_CONFIG_HOME=CFG, OLLAMA_HOST=HOST, TERM="xterm-256color", HOME=WORK)
ENV.pop("CORBIENEST_MODEL", None)
# memory extraction adds a background model call after every request; keep it off for the
# general tests (they assert on requests()[-1]) and exercise it explicitly at the end
os.makedirs(os.path.join(CFG, "corbienest")); open(os.path.join(CFG, "corbienest", "config"), "w").write("memory=0\n")

passed = failed = 0
def check(cond, msg):
    global passed, failed
    if cond: passed += 1
    else: failed += 1; print(f"  FAIL: {msg}")

def run(args, stdin=b""):
    p = subprocess.run([BIN] + args, cwd=WORK, env=ENV, input=stdin, capture_output=True, timeout=60)
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
check(any(t["function"]["name"] == "bash" for t in req.get("tools", [])), "tools sent")
check(req["options"]["num_ctx"] == 32768, "default num_ctx")
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

print("test tool denied when non-interactive without --yolo")
out, rc = run(["-m", "fake-coder:latest", "-p", "TOOL_WRITE please"])
check("denied" in out, "denied shown"); check(not os.path.exists(os.path.join(WORK, "made.txt")), "file not written")
check("non-interactively" in requests()[-1]["messages"][-1]["content"], "denial reason sent to model")

print("test write_file with --yolo")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_WRITE please"])
check(open(os.path.join(WORK, "made.txt")).read() == "made by fake\n", "file written")

print("test recovery of leaked XML tool call")
out, rc = run(["-m", "fake-coder:latest", "--yolo", "-p", "TOOL_XML"])
check("recovered 1 tool call" in out, "recovery notice"); check("● list_dir(.)" in out and "made.txt" in out, "list_dir executed")

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

# ---------- interactive via pty ----------
class Session:
    def __init__(self, args=(), cols=100, rows=40):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(WORK)
            for k, v in ENV.items(): os.environ[k] = v
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
        try: os.kill(self.pid, 9)
        except Exception: pass

print("test interactive: banner, echo, editor keys, history")
s = Session(["-m", "fake-coder:latest"])
check(s.expect("Ctrl-D to quit"), "banner"); check("● connected" in s.text(), "connected marker")
raw = s.out.decode("utf-8", "replace")
check("\x1b[?1049h" in raw, "alternate screen entered (full screen)")
check(re.search(r"\x1b\[1;39r", raw) is not None, "scroll region reserves the bottom row")
check(re.search(r"\x1b\[40;1H[^\n]*manual mode[^\n]*fake-coder:latest[^\n]*0 tokens", raw) is not None, f"status bar on the last row: {raw[-300:]!r}")
check("manual mode (shift+tab to cycle)" in clean(raw), "mode-cycling hint sits next to the mode in the bar")
s.send("hello wrld"); s.send("\x1b[D\x1b[D\x1b[D"); s.send("o"); s.send("\r")
check(s.expect("Echo: hello world"), "cursor-left insert then send"); check(s.expect("tok/s"), "stats")
check(s.expect("168 tokens (↑123 ↓45)"), f"status bar counts session tokens: {s.text()[-200:]!r}")
s.send("\x1b[A")   # history up
check(s.expect("› hello world"), f"history recall: {s.text()[-80:]!r}")
s.send("\x15")   # ctrl-u clears
s.send("first line\\\r"); s.send("second\r")
check(s.expect("Echo: first line"), "backslash-newline multi-line submit")
check(requests()[-1]["messages"][-1]["content"] == "first line\nsecond", "newline preserved in message")
s.send("\x1b[200~pasted\nlines\x1b[201~"); s.send("\r")
check(s.expect("Echo: pasted"), "bracketed paste")
check(requests()[-1]["messages"][-1]["content"] == "pasted\nlines", "paste newlines kept")

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
check(s.expect("› typed ahead"), f"type-ahead preserved: {s.text()[-60:]!r}")
s.send("\x15")

print("test interactive: messages queued while the model works (Enter during generation)")
s.send("SLOW\r"); check(s.expect("two"), "streaming started")
s.send("queued que"); check(s.expect("› queued que▏", 3), f"text typed while busy is echoed live in the bar: {s.text()[-200:]!r}")
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
check(s.expect("› keep me", 5), f"after Ctrl-C the queued text is back in the editor, not sent: {s.text()[-200:]!r}")
time.sleep(0.5); check("Echo: keep me" not in s.text(), "queued message not auto-sent after an interrupt")
s.send("\x15")
check("queued" not in clean(s.out[-400:].decode("utf-8", "replace")), "queue counter gone from the bar")
s.send("/mode auto\r"); s.expect("mode: auto")
s.send("TOOL_SLEEP\r"); check(s.expect("running…", 10), "slow tool running")
s.send("mid-task note\r"); check(s.expect("1 queued", 3), "queued while the tool runs")
check(s.expect("slept", 10), "tool finished")
check(s.expect("› mid-task note", 5), "queued message injected between tool rounds")
check(s.expect("Echo: mid-task note", 15), "model saw the mid-task message on its next call")
msgs = requests()[-1]["messages"]
check(msgs[-1]["role"] == "user" and msgs[-1]["content"] == "mid-task note" and msgs[-2]["role"] == "tool", f"injected right after the tool result: {[m['role'] for m in msgs[-4:]]}")
s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: shift+tab while the model works switches the mode immediately")
s.send("SLOW\r"); check(s.expect("two"), "streaming")
s.send("\x1b[Z"); check(s.expect("accept edits on", 3), "mode switched while busy")
s.send("half typed"); check(s.expect("› half typed▏", 3), "typing still echoed after the switch")
s.send("\x1b[Z"); check(s.expect("plan mode on", 3), "second switch while busy, with pending text")
check(s.expect("eight"), "generation finished")
check(s.expect("› half typed", 5), f"pending text back in the editor without the CSI Z bytes: {s.text()[-200:]!r}")
s.send("\x15"); s.send("/mode manual\r"); s.expect("mode: manual")

print("test interactive: auto-compact when the context is 95%+ full")
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
s = Session(["-m", "fake-coder:latest"]); s.expect("›")
s.send("!sleep 1.2; echo bang-done\r"); check(s.expect("bang-done", 10), "bang command ran")
raw2 = clean(s.out[s.mark:].decode("utf-8", "replace"))
check(len(set(re.findall(r"([⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]) running command", raw2))) >= 2, "bar spinner animates during a shell command")
check(re.search(r"running… \(\ds\)  ctrl-c to interrupt", raw2) is not None, f"inline spinner during a shell command: {raw2[-200:]!r}")
check("auto mode" not in raw2, "bang command does not flash a mode change in the bar")
s.close()

print("test interactive: project memory (.corbienest/memory.md), curated by the model after each request")
s = Session(["-m", "fake-coder:latest"]); s.expect("›")
s.send("/memory\r"); check(s.expect("auto-update is off"), "/memory shows state"); check(s.expect("no memory yet"), "no file yet")
s.send("/memory on\r"); check(s.expect("memory on"), "/memory on")
s.send("nothing special\r"); check(s.expect("Echo: nothing special"), "reply")
check(requests()[-1]["messages"][0]["content"].startswith("You maintain the persistent memory file"), "extraction call made after the request")
check(requests()[-1]["messages"][-1]["role"] == "user" and "NO_CHANGE" in requests()[-1]["messages"][-1]["content"], "asked for the updated file or NO_CHANGE")
time.sleep(0.5); check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "NO_CHANGE: no file written")
s.send("REMEMBER_ME the user prefers tabs\r"); check(s.expect("memory updated"), f"memory written: {s.text()[-200:]!r}")
mem = open(os.path.join(WORK, ".corbienest", "memory.md")).read()
check(mem.startswith("# Project memory") and "- the user prefers tabs" in mem and "```" not in mem, f"file content (fence stripped): {mem!r}")
s.send("what now\r"); check(s.expect("Echo: what now"), "next request")
sysmsg = [r for r in requests() if r["messages"][-1]["content"] == "what now"][-1]["messages"][0]["content"]
check("# Project memory (from .corbienest/memory.md)" in sysmsg and "the user prefers tabs" in sysmsg, "memory loaded into the system prompt")
s.send("/memory\r"); check(s.expect("the user prefers tabs"), "/memory prints it")
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
s.send("/memory clear\r"); check(s.expect("removed .corbienest/memory.md"), "/memory clear")
check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "file removed")
s.send("\x04"); s.close()
out, rc = run(["-m", "fake-coder:latest", "-p", "REMEMBER_ME oneshot"])
check(not os.path.exists(os.path.join(WORK, ".corbienest", "memory.md")), "one-shot run does not create the memory file (memory off in config)")

print("test config persistence")
cfg = open(os.path.join(CFG, "corbienest", "config")).read()
check("model=fake-coder:latest" in cfg, "model saved to config")
check("mode=manual" in cfg, "mode saved to config")
hist = open(os.path.join(CFG, "corbienest", "history")).read()
check("hello world" in hist and "first line\x1fsecond" in hist, "history saved with encoded newline")
s = Session([])   # no -m: should use saved model
check(s.expect("model: fake-coder:latest"), "saved model used"); s.send("\x04"); s.close()

shutil.rmtree(WORK, ignore_errors=True); shutil.rmtree(CFG, ignore_errors=True)
print(f"{passed} checks passed, {failed} failed")
sys.exit(1 if failed else 0)
