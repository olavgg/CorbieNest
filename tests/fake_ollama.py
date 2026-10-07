"""A tiny fake Ollama server for integration tests.

Serves /api/version, /api/tags, /api/show and a scripted /api/chat. The chat handler is
deterministic: it looks at the last message and replies with canned streaming
NDJSON, including tool calls, so the whole agent loop can be exercised without
a real model. It also stands in for the hosted APIs the advisor can consult, under
/xai/v1, /openai/v1 (Chat Completions) and /anthropic/v1 (Messages), with the key
"test-key". Run standalone: python3 fake_ollama.py PORT
"""
import json, re, ssl, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer

MODELS = [
    {"name": "fake-coder:latest", "model": "fake-coder:latest", "size": 1, "digest": "x",
     "details": {"parameter_size": "7B"}, "capabilities": ["completion", "tools"]},
    {"name": "fake-chat:latest", "model": "fake-chat:latest", "size": 1, "digest": "y",
     "details": {"parameter_size": "3B"}, "capabilities": ["completion"]},
    {"name": "fake-thinker:latest", "model": "fake-thinker:latest", "size": 1, "digest": "z",
     "details": {"parameter_size": "9B"}, "capabilities": ["completion", "tools", "thinking"]},
    {"name": "fake-slow:latest", "model": "fake-slow:latest", "size": 1, "digest": "w",
     "details": {"parameter_size": "30B"}, "capabilities": ["completion", "tools"]},   # /api/ps says: half in GPU memory
    {"name": "fake-levels:latest", "model": "fake-levels:latest", "size": 1, "digest": "v",
     "details": {"parameter_size": "20B", "family": "gptoss"}, "capabilities": ["completion", "tools", "thinking"]},   # thinks in levels, like gpt-oss
    {"name": "fake-big:latest", "model": "fake-big:latest", "size": 1, "digest": "u",
     "details": {"parameter_size": "120B", "family": "fakebig"}, "capabilities": ["completion", "tools", "thinking"]},   # the expensive one: /advisor
    {"name": "fake-stale:latest", "model": "fake-stale:latest", "size": 1, "digest": "t",
     "details": {"parameter_size": "32B", "family": "fakestale"}, "capabilities": ["completion"]},   # /api/tags is out of date: /api/show knows better
    {"name": "fake-declared:latest", "model": "fake-declared:latest", "size": 1, "digest": "s",
     "details": {"parameter_size": "27B", "family": "fakedeclared"}, "capabilities": ["completion", "tools", "thinking"]},   # a newer server: /api/show lists its levels
]
# what /api/show reports when it differs from the (stale) manifest that /api/tags serves
SHOW_CAPS = {"fake-stale:latest": ["completion", "tools", "thinking"]}
# thinking.values/default in /api/show (Ollama >= 0.34.3): the levels a model has, in its own names
SHOW_THINKING = {"fake-declared:latest": {"values": [False, "low", "xhigh"], "default": "low"}}
LEGACY_LEVELS = ("low", "medium", "high", "max")
# a cloud model: /api/show knows it, /api/tags does not list it, and /api/chat wants `ollama signin`
CLOUD = {"fake-remote:cloud": {"details": {"parameter_size": "671B", "family": "fakecloud"}, "capabilities": ["completion", "tools", "thinking"]}}
EVICTED = set()   # models an advisor call pushed out of memory: gone from /api/ps until their next chat

REQUEST_LOG = []
PROVIDER_LOG = []   # requests to the hosted APIs: path, headers (keys left out) and body

# the hosted APIs: the models each one knows, and what Anthropic's model endpoint says of them
PROVIDER_KEY = "test-key"
ADAPTIVE = {"thinking": {"supported": True, "types": {"enabled": {"supported": False}, "adaptive": {"supported": True}}},
            "effort": dict({"supported": True}, **{l: {"supported": True} for l in ("low", "medium", "high", "xhigh", "max")})}
BUDGET = {"thinking": {"supported": True, "types": {"enabled": {"supported": True}, "adaptive": {"supported": False}}}, "effort": {"supported": False}}
PROVIDER_MODELS = {
    "xai": {"grok-fake": {}},
    "openai": {"gpt-fake": {}},   # takes reasoning_effort up to high, like the gpt-5 family
    "anthropic": {"claude-fake-opus": {"max_input_tokens": 1_000_000, "max_tokens": 128_000, "capabilities": ADAPTIVE},
                  "claude-opus-5": {"max_input_tokens": 1_000_000, "max_tokens": 128_000, "capabilities": ADAPTIVE},   # has refusal fallbacks
                  "claude-fake-haiku": {"max_input_tokens": 200_000, "max_tokens": 64_000, "capabilities": BUDGET}},
}
EFFORTS = {"xai": ("none", "low", "medium", "high", "xhigh"), "openai": ("none", "minimal", "low", "medium", "high")}

def advisor_text(brief):
    """What the fake advisor says, whichever way it is reached. A review (before the request
    ends) and a check (of the first change) find something unless the request says APPROVE."""
    if "The agent has ended its turn" in brief:
        return "LGTM" if "APPROVE" in brief else "ADVICE: REVIEW-FINDING the test for the empty field is missing; add it."
    if "about to make the first change" in brief:
        return "LGTM" if "APPROVE" in brief else "ADVICE: CHECK-FINDING write to the other file instead."
    asked = brief.split("# What the agent asks you\n", 1)[-1].strip().splitlines()[0][:80]
    return "ADVICE: read hay.txt before you edit anything.\nYou asked: " + asked

def chunk(model, content=None, tool_calls=None, thinking=None, done=False, prompt_tokens=123, done_reason=None):
    msg = {"role": "assistant", "content": content or ""}
    if thinking: msg["thinking"] = thinking
    if tool_calls: msg["tool_calls"] = tool_calls
    d = {"model": model, "created_at": "now", "message": msg, "done": done}
    if done:
        d.update({"done_reason": done_reason or "stop", "total_duration": 1_500_000_000, "eval_duration": 1_000_000_000,
                  "prompt_eval_duration": 200_000_000, "prompt_eval_count": prompt_tokens, "eval_count": 45})
    return json.dumps(d) + "\n"

def script(model, messages, req):
    """Yield NDJSON chunks for the given conversation."""
    last = messages[-1]
    user_msgs = [m for m in messages if m["role"] == "user"]
    text = user_msgs[-1]["content"] if user_msgs else ""
    if messages and messages[0]["role"] == "system" and "You maintain the persistent memory file" in messages[0]["content"]:
        # end-of-request memory extraction: only "REMEMBER_ME <fact>" is worth saving
        facts = [m["content"].split("REMEMBER_ME", 1)[1].strip() for m in messages if m["role"] == "user" and "REMEMBER_ME" in m["content"]]
        if any("CUT_MEMORY" in m["content"] for m in messages if m["role"] == "user"):
            # generation hit num_predict: a truncated file must never be written
            yield chunk(model, "# Project memory\n\n## User\n- half a fac")
            yield chunk(model, done=True, done_reason="length")
            return
        if any("MEMWAIT" in m["content"] for m in messages if m["role"] == "user"):
            # a slow extraction: lets a test type (and so queue) a message while memory is written
            for w in ["# Project memory\n\n", "## User\n", "- slow fact\n\n", "## Feedback\n\n", "## Project\n\n", "## Reference\n"]:
                yield chunk(model, w); time.sleep(0.5)
            yield chunk(model, done=True)
            return
        if facts:
            yield chunk(model, "```markdown\n# Project memory\n\n## User\n- " + facts[-1] + "\n\n## Feedback\n\n## Project\n\n## Reference\n```")
        else:
            yield chunk(model, "NO_CHANGE")
        yield chunk(model, done=True)
        return
    if messages[0]["role"] == "system" and "You are the advisor" in messages[0]["content"]:
        # the advisor: one toolless call that is shown the conversation. Keywords sit in the
        # transcript it is handed (the user's request is quoted in it).
        brief = messages[-1]["content"]
        if req.get("think"): yield chunk(model, thinking="weighing it up... ")
        if "EVICTMAIN" in brief: EVICTED.add("fake-coder:latest")   # loading this one took the main model's place
        if "THINKTAG" in brief:
            # a model whose thinking the server does not split off: it arrives inside the answer
            yield chunk(model, "<think>SECRET-REASONING about the needle</think>\n\nADVICE: strip me")
            yield chunk(model, done=True)
            return
        if "CUTADVICE" in brief:
            yield chunk(model, done=True, done_reason="length")   # still thinking when num_predict ran out
            return
        if "SLOWADVICE" in brief:
            for w in ["ADVICE: ", "take ", "it ", "slowly, ", "one ", "step ", "at ", "a ", "time."]:
                yield chunk(model, w); time.sleep(0.5)
            yield chunk(model, done=True)
            return
        yield chunk(model, advisor_text(brief))
        # a prompt of 30k tokens: were it taken for the conversation's, a 32k window would look 91% full
        yield chunk(model, done=True, prompt_tokens=30_000)
        return
    if messages[0]["role"] == "system" and "You are a sub-agent" in messages[0]["content"]:
        # sub-agent: one grep round, then a report — or, for a SLEEP task, one slow command,
        # so a test can type a message while the sub-agent is in the middle of a round
        if "SLEEP" in messages[1]["content"] and last["role"] != "tool":
            yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "sleep 2; echo sub-slept"}}}])
            yield chunk(model, done=True)
            return
        if last["role"] == "tool":
            yield chunk(model, "REPORT: found it in " + last["content"].strip().splitlines()[0][:40])
            yield chunk(model, done=True)
            return
        yield chunk(model, tool_calls=[{"function": {"name": "grep", "arguments": {"pattern": "needle", "path": "."}}}])
        yield chunk(model, done=True)
        return
    if last["role"] == "tool" and "ADVISOR_LOOP" in text and not last["content"].startswith("error: the advisor has been consulted"):
        # a model that cannot stop asking: the client has to be the one to say "enough"
        yield chunk(model, tool_calls=[{"function": {"name": "advisor", "arguments": {"question": "and now?"}}}])
        yield chunk(model, done=True)
        return
    if last["role"] == "tool":
        # after a tool result: summarise it
        yield chunk(model, "Tool said: ")
        yield chunk(model, last["content"].strip().splitlines()[0][:60])
        yield chunk(model, done=True)
        return
    if "EMPTY_ONCE" in text or "EMPTY_ALWAYS" in text:
        # tokens generated, but the server delivers neither text nor a tool call (Ollama's
        # parser dropping a malformed call looks like this). EMPTY_ONCE answers on the retry.
        seen = sum(1 for r in REQUEST_LOG
                   if any("EMPTY_" in (m.get("content") or "") for m in r.get("messages", []) if m["role"] == "user"))
        if "EMPTY_ALWAYS" in text or seen == 1:
            yield chunk(model, done=True)
            return
        yield chunk(model, "Echo: " + text.split("\n")[0])
        yield chunk(model, done=True)
        return
    if "TOOL_ADVISOR_NOQ" in text:
        yield chunk(model, tool_calls=[{"function": {"name": "advisor", "arguments": {}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_ADVISOR" in text:
        yield chunk(model, tool_calls=[{"function": {"name": "advisor", "arguments": {"question": "Why does the needle test fail, and where should I look first?"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_TASK_SLEEP" in text:
        yield chunk(model, tool_calls=[{"function": {"name": "task", "arguments": {"description": "slow research", "prompt": "SLEEP, then report."}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_TASK" in text:
        yield chunk(model, tool_calls=[{"function": {"name": "task", "arguments": {"description": "find the needle", "prompt": "Search for needle and report where it is."}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_SEARCH" in text:
        q = text.split("TOOL_SEARCH", 1)[1].strip() or "keycloak admin rest api"
        yield chunk(model, tool_calls=[{"function": {"name": "web_search", "arguments": {"query": q, "max_results": 3}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_FETCH" in text:
        # the test puts the URL of its own little docs server in the prompt
        m = re.search(r"https?://\S+", text)
        yield chunk(model, tool_calls=[{"function": {"name": "web_fetch", "arguments": {"url": m.group(0) if m else "no-url"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_BIG" in text:
        # a tool round with a big result (300 numbered lines), for the elision test
        yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "seq -f 'line-%04g' 1 300"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_BASH" in text:
        yield chunk(model, "Running it.")
        yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "echo hello-from-tool"}}}])
        yield chunk(model, done=True)
        return
    if "TOOL_SLEEP2" in text:
        # two calls in one round, the first slow: a message can arrive before the second starts
        yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "sleep 2; echo slept-one"}}},
                                       {"function": {"name": "bash", "arguments": {"command": "echo second-tool"}}}])
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
    if "HALF_CTX" in text:
        # report a prompt using ~60% of the default 32k window: triggers tool-result elision, not compaction
        yield chunk(model, "Echo: " + text.split("\n")[0])
        yield chunk(model, done=True, prompt_tokens=20_000)
        return
    if "HUGE_TOOL" in text:
        # a tool round that also reports a (nearly) full context window: auto-compact fires
        # in the middle of the turn, between the tool result and the next model call
        yield chunk(model, tool_calls=[{"function": {"name": "bash", "arguments": {"command": "echo hello-from-tool"}}}])
        yield chunk(model, done=True, prompt_tokens=999_999)
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
        try:
            self.send_response(code); self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        except (BrokenPipeError, ConnectionResetError):
            pass   # the client gave up waiting (Esc, a queued message)
    def _provider(self):
        m = re.match(r"^/(xai|openai|anthropic)/v1(/.*)$", self.path)
        return (m.group(1), m.group(2)) if m else (None, None)
    def _provider_auth(self, prov):
        """None if the request may go on, else the error already sent"""
        if prov == "anthropic":
            if self.headers.get("x-api-key") != PROVIDER_KEY:
                return self._json(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}}) or "sent"
            if self.headers.get("anthropic-version") != "2023-06-01":
                return self._json(400, {"type": "error", "error": {"type": "invalid_request_error", "message": "anthropic-version header is required"}}) or "sent"
        elif self.headers.get("Authorization") != "Bearer " + PROVIDER_KEY:
            # what the real ones answer: xAI a 400, OpenAI a 401 that quotes a masked piece of the key
            if prov == "xai": return self._json(400, {"code": "Client specified an invalid argument", "error": "Incorrect API key provided. You can obtain an API key from https://console.x.ai."}) or "sent"
            k = self.headers.get("Authorization", "")[7:]
            return self._json(401, {"error": {"message": f"Incorrect API key provided: {k[:3]}********{k[-4:]}. You can find your API key at https://platform.openai.com/account/api-keys.",
                                              "type": "invalid_request_error", "code": "invalid_api_key"}}) or "sent"
        return None
    def _provider_get(self, prov, rest):
        if self._provider_auth(prov): return
        mid = rest[len("/models/"):] if rest.startswith("/models/") else None
        entry = PROVIDER_MODELS[prov].get(mid) if mid else None
        if entry is None:
            if prov == "anthropic": return self._json(404, {"type": "error", "error": {"type": "not_found_error", "message": f"model: {mid}"}})
            if prov == "xai": return self._json(404, {"code": "Some resource has not been found", "error": f"The model {mid} does not exist or your team does not have access to it."})
            return self._json(404, {"error": {"message": f"The model '{mid}' does not exist", "type": "invalid_request_error", "code": "model_not_found"}})
        return self._json(200, dict({"id": mid, "object": "model", "type": "model", "owned_by": prov}, **entry))
    def _provider_post(self, prov, rest, req):
        hdrs = {k.lower(): v for k, v in self.headers.items() if k.lower() not in ("authorization", "x-api-key")}
        PROVIDER_LOG.append({"provider": prov, "path": self.path, "headers": hdrs, "body": req})
        if self._provider_auth(prov): return
        model = req.get("model", "")
        if model not in PROVIDER_MODELS[prov]:
            return self._json(404, {"error": {"message": f"The model '{model}' does not exist"}})
        if prov == "anthropic":
            if rest != "/messages": return self._json(404, {"type": "error", "error": {"type": "not_found_error", "message": "Not found"}})
            return self._messages(model, req)
        if rest != "/chat/completions": return self._json(404, {"error": {"message": "Unknown request URL"}})
        if "max_tokens" in req:
            return self._json(400, {"error": {"message": "Unsupported parameter: 'max_tokens' is not supported with this model. Use 'max_completion_tokens' instead."}})
        eff = req.get("reasoning_effort")
        if eff is not None and eff not in EFFORTS[prov]:
            return self._json(400, {"error": {"message": f"Unsupported value: '{eff}' is not supported with the '{model}' model."}})
        brief = req["messages"][-1]["content"]
        if "SLOWADVICE" in brief: time.sleep(3)
        msg, why = {"role": "assistant", "content": advisor_text(brief)}, "stop"
        if "CUTADVICE" in brief: msg["content"], why = "", "length"
        if "REFUSEADVICE" in brief: msg["content"], msg["refusal"] = None, "I can't help with that."
        return self._json(200, {"id": "chatcmpl-fake", "object": "chat.completion", "model": model,
                                "choices": [{"index": 0, "message": msg, "finish_reason": why}],
                                "usage": {"prompt_tokens": 2000, "completion_tokens": 150, "total_tokens": 2150}})
    def _messages(self, model, req):
        caps = PROVIDER_MODELS["anthropic"][model]["capabilities"]
        bad = lambda m: self._json(400, {"type": "error", "error": {"type": "invalid_request_error", "message": m}})
        if "fallbacks" in req and self.headers.get("anthropic-beta") != "server-side-fallback-2026-07-01": return bad("fallbacks: requires the beta header")
        if "fallbacks" in req and not model.startswith("claude-opus-5"): return bad("fallbacks: not available for this model")
        th = req.get("thinking", {}).get("type")
        if th and not caps["thinking"]["types"].get(th, {}).get("supported"): return bad(f"thinking.type: {th} is not supported on this model")
        if "output_config" in req and not caps["effort"]["supported"]: return bad("output_config.effort: not supported on this model")
        if not isinstance(req.get("max_tokens"), int) or [m["role"] for m in req.get("messages", [])] != ["user"]: return bad("messages: malformed")
        brief = req["messages"][0]["content"]
        if "SLOWADVICE" in brief: time.sleep(3)
        usage = {"input_tokens": 1800, "cache_read_input_tokens": 200, "output_tokens": 120}
        if "REFUSEADVICE" in brief:
            return self._json(200, {"type": "message", "role": "assistant", "model": model, "content": [], "stop_reason": "refusal",
                                    "stop_details": {"type": "refusal", "category": "cyber", "explanation": "declined"}, "usage": usage})
        blocks = [{"type": "thinking", "thinking": "", "signature": "sig"}]
        if "CUTADVICE" not in brief: blocks.append({"type": "text", "text": advisor_text(brief)})
        return self._json(200, {"type": "message", "role": "assistant", "model": model, "content": blocks,
                                "stop_reason": "max_tokens" if "CUTADVICE" in brief else "end_turn", "usage": usage})
    def do_GET(self):
        prov, rest = self._provider()
        if prov: return self._provider_get(prov, rest)
        if self.path == "/_provider_requests": return self._json(200, PROVIDER_LOG)
        if self.path == "/api/version": return self._json(200, {"version": "0.0.0-fake"})
        if self.path == "/api/tags": return self._json(200, {"models": MODELS})
        if self.path == "/_requests": return self._json(200, REQUEST_LOG)
        if self.path == "/api/ps":   # the loaded model: fake-slow is only half in GPU memory
            loaded = [{"name": "fake-coder:latest", "size": 8_000_000_000, "size_vram": 8_000_000_000},
                      {"name": "fake-slow:latest", "size": 8_000_000_000, "size_vram": 4_000_000_000}]
            return self._json(200, {"models": [m for m in loaded if m["name"] not in EVICTED]})
        self._json(404, {"error": "not found"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n) or b"{}")
        model = req.get("model", "")
        prov, rest = self._provider()
        if prov: return self._provider_post(prov, rest, req)
        if self.path == "/api/show":
            entry = next((m for m in MODELS if m["name"] == model), None) or CLOUD.get(model)
            if not entry: return self._json(404, {"error": f"model '{model}' not found"})
            info = {"model_info": {"general.architecture": "fake", "fake.context_length": 65536, "fake.embedding_length": 8},
                    "details": entry["details"], "capabilities": SHOW_CAPS.get(model, entry["capabilities"])}
            if model in SHOW_THINKING: info["thinking"] = SHOW_THINKING[model]
            if model == "fake-thinker:latest": info["parameters"] = "top_k 20\ndraft_num_predict 4\ntemperature 1"   # ships an MTP draft head
            return self._json(200, info)
        if self.path != "/api/chat": return self._json(404, {"error": "not found"})
        REQUEST_LOG.append(req)   # only chat requests: tests read the last one
        if model in CLOUD: return self._json(401, {"error": "Unauthorized"})   # relayed to ollama.com, which wants an account
        EVICTED.discard(model)   # a chat loads the model (again)
        entry = next((m for m in MODELS if m["name"] == model), None)
        if not entry:
            return self._json(404, {"error": f"model '{model}' not found"})
        # "think" is checked the way the real server (0.33) checks it: a closed set of values,
        # and anything but false is refused for a model that cannot think
        think = req.get("think")
        declared = [v for v in SHOW_THINKING.get(model, {}).get("values", []) if isinstance(v, str)]
        if isinstance(think, str) and think not in (declared or LEGACY_LEVELS):
            return self._json(400, {"error": f'invalid think value: "{think}" (must be "high", "medium", "low", "max", true, or false)'})
        if think and "thinking" not in SHOW_CAPS.get(model, entry["capabilities"]):
            return self._json(400, {"error": f'"{model}" does not support thinking'})
        msgs = req.get("messages", [])
        # Ollama trims an over-long prompt by dropping whole messages from the front, and its
        # qwen3.8 renderer then refuses a prompt the user's request has fallen out of. Play that
        # back for CTX_OVERFLOW once there are tool results, until the client has shrunk them.
        tool_msgs = [m for m in msgs if m["role"] == "tool"]
        if tool_msgs and any("CTX_OVERFLOW" in (m.get("content") or "") for m in msgs if m["role"] == "user") \
           and not any("elided to save context" in (m.get("content") or "") for m in tool_msgs):
            return self._json(500, {"error": "no user query found in messages"})
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

def serve(port, certfile=None, keyfile=None):
    srv = HTTPServer(("127.0.0.1", port), H)
    if certfile:   # the same server behind TLS: https:// for Ollama and for the hosted APIs
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(certfile, keyfile)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()

if __name__ == "__main__":
    serve(int(sys.argv[1]) if len(sys.argv) > 1 else 11435)
