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
# general tests (they assert on requests()[-1]) and exercise it explicitly at the end.
# memory_idle=0 too, so a pending extraction never fires from a slow expect() mid-test.
os.makedirs(os.path.join(CFG, "corbienest")); open(os.path.join(CFG, "corbienest", "config"), "w").write("memory=0\nmemory_idle=0\n")

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
check(req["keep_alive"] == "30m" and "num_predict" not in req["options"] and "think" not in req, "default keep_alive 30m; no num_predict/think on a normal call")
out, rc = run(["-m", "fake-coder:latest", "--keep-alive", "-1", "-p", "hello"])
check(requests()[-1]["keep_alive"] == "-1", "--keep-alive -1 sent")
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
check(sub and all(t["function"]["name"] in ("read_file", "list_dir", "grep", "bash") for t in sub[-1]["tools"]), "sub-agent got read-only tools only")
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
s.send("/think high\r"); check(s.expect("level high"), "/think high")
s.send("/model fake-thinker:latest\r"); s.expect("model set to fake-thinker")
s.send("level test\r"); check(s.expect("Echo: level test"), "reply"); check(requests()[-1]["think"] == "high", "thinking level sent as a string")
s.send("/think auto\r"); s.expect("think: auto"); s.send("level test 2\r"); s.expect("Echo: level test 2"); check("think" not in requests()[-1], "/think auto clears the level")
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

shutil.rmtree(WORK, ignore_errors=True); shutil.rmtree(CFG, ignore_errors=True)
print(f"{passed} checks passed, {failed} failed")
sys.exit(1 if failed else 0)
