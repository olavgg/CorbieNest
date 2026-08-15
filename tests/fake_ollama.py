"""A tiny fake Ollama server for integration tests.

Serves /api/version, /api/tags, /api/show and a scripted /api/chat. The chat handler is
deterministic: it looks at the last message and replies with canned streaming
NDJSON, including tool calls, so the whole agent loop can be exercised without
a real model. Run standalone: python3 fake_ollama.py PORT
"""
import json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer

MODELS = [
    {"name": "fake-coder:latest", "model": "fake-coder:latest", "size": 1, "digest": "x",
     "details": {"parameter_size": "7B"}, "capabilities": ["completion", "tools"]},
    {"name": "fake-chat:latest", "model": "fake-chat:latest", "size": 1, "digest": "y",
     "details": {"parameter_size": "3B"}, "capabilities": ["completion"]},
    {"name": "fake-thinker:latest", "model": "fake-thinker:latest", "size": 1, "digest": "z",
     "details": {"parameter_size": "9B"}, "capabilities": ["completion", "tools", "thinking"]},
]

REQUEST_LOG = []

def chunk(model, content=None, tool_calls=None, thinking=None, done=False, prompt_tokens=123):
    msg = {"role": "assistant", "content": content or ""}
    if thinking: msg["thinking"] = thinking
    if tool_calls: msg["tool_calls"] = tool_calls
    d = {"model": model, "created_at": "now", "message": msg, "done": done}
    if done:
        d.update({"done_reason": "stop", "total_duration": 1_500_000_000, "eval_duration": 1_000_000_000,
                  "prompt_eval_count": prompt_tokens, "eval_count": 45})
    return json.dumps(d) + "\n"

def script(model, messages, req):
    """Yield NDJSON chunks for the given conversation."""
    last = messages[-1]
    user_msgs = [m for m in messages if m["role"] == "user"]
    text = user_msgs[-1]["content"] if user_msgs else ""
    if messages and messages[0]["role"] == "system" and "You maintain the persistent memory file" in messages[0]["content"]:
        # end-of-request memory extraction: only "REMEMBER_ME <fact>" is worth saving
        facts = [m["content"].split("REMEMBER_ME", 1)[1].strip() for m in messages if m["role"] == "user" and "REMEMBER_ME" in m["content"]]
        if facts:
            yield chunk(model, "```markdown\n# Project memory\n\n## User\n- " + facts[-1] + "\n\n## Feedback\n\n## Project\n\n## Reference\n```")
        else:
            yield chunk(model, "NO_CHANGE")
        yield chunk(model, done=True)
        return
    if messages[0]["role"] == "system" and "You are a sub-agent" in messages[0]["content"]:
        # sub-agent: one grep round, then a report
        if last["role"] == "tool":
            yield chunk(model, "REPORT: found it in " + last["content"].strip().splitlines()[0][:40])
            yield chunk(model, done=True)
            return
        yield chunk(model, tool_calls=[{"function": {"name": "grep", "arguments": {"pattern": "needle", "path": "."}}}])
        yield chunk(model, done=True)
        return
    if last["role"] == "tool":
        # after a tool result: summarise it
        yield chunk(model, "Tool said: ")
        yield chunk(model, last["content"].strip().splitlines()[0][:60])
        yield chunk(model, done=True)
        return
    if "TOOL_TASK" in text:
        yield chunk(model, tool_calls=[{"function": {"name": "task", "arguments": {"description": "find the needle", "prompt": "Search for needle and report where it is."}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_BASH" in text:
        yield chunk(model, "Running it.")
        yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "echo hello-from-tool"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_SLEEP" in text:
        # a slow tool: lets tests type while a command runs
        yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "sleep 1.5; echo slept"}}}])
        yield chunk(model, done=True)
        return
    if "create a CORBIENEST.md file" in text:
        # /init: pretend to have explored, then write the file
        yield chunk(model, tool_calls=[{"function": {"name": "write_file", "arguments": {"path": "CORBIENEST.md", "content": "# Project\nBuild with make.\n"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_WRITE" in text:
        yield chunk(model, tool_calls=[{"function": {"name": "write_file", "arguments": {"path": "made.txt", "content": "made by fake\n"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_XML" in text:
        # leaked native syntax; corbienest must recover it
        yield chunk(model, "<tool_call>\n<function=list_dir>\n<parameter=path>\n.\n</parameter>\n</function>\n</tool_call>")
        yield chunk(model, done=True)
        return
    if "SLOW" in text:
        for w in ["one ", "two ", "three ", "four ", "five ", "six ", "seven ", "eight "]:
            yield chunk(model, w); time.sleep(0.4)
        yield chunk(model, done=True)
        return
    if "ERROR" in text:
        yield "ERR"
        return
    if "HUGE_CTX" in text:
        # report a prompt that (nearly) fills the context window: triggers auto-compact
        yield chunk(model, "Echo: HUGE_CTX")
        yield chunk(model, done=True, prompt_tokens=999_999)
        return
    if req.get("think"):
        yield chunk(model, thinking="pondering... ")
    yield chunk(model, "Echo: ")
    yield chunk(model, text.split("\n")[0])
    if "**" in text: yield chunk(model, " **bold**")
    yield chunk(model, done=True)

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def _json(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == "/api/version": return self._json(200, {"version": "0.0.0-fake"})
        if self.path == "/api/tags": return self._json(200, {"models": MODELS})
        if self.path == "/_requests": return self._json(200, REQUEST_LOG)
        self._json(404, {"error": "not found"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        model = req.get("model", "")
        if self.path == "/api/show":
            if not any(m["name"] == model for m in MODELS): return self._json(404, {"error": f"model '{model}' not found"})
            return self._json(200, {"model_info": {"general.architecture": "fake", "fake.context_length": 65536, "fake.embedding_length": 8}})
        if self.path != "/api/chat": return self._json(404, {"error": "not found"})
        REQUEST_LOG.append(req)   # only chat requests: tests read the last one
        if not any(m["name"] == model for m in MODELS):
            return self._json(404, {"error": f"model '{model}' not found"})
        msgs = req.get("messages", [])
        gen = script(model, msgs, req)
        self.send_response(200); self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
        try:
            for c in gen:
                if c == "ERR":
                    # simulate server error mid-stream (chunked body carrying an error object)
                    c = json.dumps({"error": "boom from fake"}) + "\n"
                b = c.encode(); self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n"); self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n"); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass   # client interrupted (Ctrl-C) — expected

def serve(port):
    HTTPServer(("127.0.0.1", port), H).serve_forever()

if __name__ == "__main__":
    serve(int(sys.argv[1]) if len(sys.argv) > 1 else 11435)
