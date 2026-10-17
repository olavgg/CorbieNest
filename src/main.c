/* corbienest — a Claude-Code-style terminal coding agent for local and hosted models. */
#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static cJSON *g_messages = NULL;    /* conversation, without system prompt */
static cJSON *g_tools = NULL;       /* tool definitions */
static bool   g_model_tools = true; /* current model supports tools */
static bool   g_model_bad = false;  /* the working model cannot be used (a hosted one with no key, or an advisor-only provider) */
static char   g_model_err[512];
static bool   g_placement_checked = false; /* /api/ps warning about a partly-CPU model shown once per model */
static bool   g_while_busy = false;        /* a slash command is running inside the turn (see run_slash_while_busy) */
static int    g_model_max_ctx = 0;  /* context length the model was trained for (0 = unknown) */
static char   g_cwd[PATH_MAX];
static char  *g_project_instructions = NULL;
static char  *g_memory = NULL;                    /* contents of MEMORY_PATH (see memory_*) */

static char   g_session_id[64];                  /* current session (file stem under config_dir()/sessions) */
static int    g_advisor_uses = 0;                /* consultations of the advisor in the current request (see run_advisor) */
static time_t g_advisor_since = 0;               /* ... counted from here: a request that runs long gets them again (advisor_window_roll) */
static bool   g_advisor_said = false;            /* run_advisor() printed what came of the tool call in hand (see run_turn) */
static bool   g_advisor_reviewed = false;        /* this request's review before it ends has been had (guidance strong and up) */
static bool   g_advisor_checked = false;         /* and the check of its first change (max) */
static bool   g_advisor_stepped_in = false;      /* and its stepping in when the tool calls kept failing (normal and up) */
static int    g_fail_rounds = 0;                 /* rounds of tool calls in a row, in this request, with a failure in them */

/* The commands, each with the line the suggestions under the input field show for it. */
static const struct { const char *name, *desc; } SLASH_CMDS[] = {
    { "/help", "commands, keys and input tricks" },
    { "/model", "pick a model from a menu, or anthropic:NAME" },
    { "/models", "list the models ollama has" },
    { "/clear", "start a new conversation" },
    { "/compact", "summarise the conversation to free context" },
    { "/status", "model, context usage, settings" },
    { "/system", "extra system instructions: TEXT, or clear" },
    { "/think", "when the model thinks: on, off, auto · show or hide it" },
    { "/effort", "how hard this model thinks" },
    { "/advisor", "a stronger model the agent may consult: a local one, or Claude, ChatGPT, Grok by API key" },
    { "/subagent", "the model sub-agents (task) run on, and how hard it thinks" },
    { "/orchestrate", "the advisor model plans the request as tasks, this model does them, the advisor reviews each" },
    { "/plan", "the advisor plans the request in layers, you approve, and the project is built after each layer" },
    { "/goal", "candidates in git worktrees, on one or more models, race to a metric: the best score is kept" },
    { "/btw", "a side question: answered from the conversation, and not kept in it" },
    { "/workers", "how many tasks of /orchestrate may run at the same time" },
    { "/mode", "permission mode: manual, accept-edits, plan, auto" },
    { "/yolo", "auto mode on or off: every tool call approved" },
    { "/tools", "tool calling on or off" },
    { "/web", "web_search and web_fetch: on, off, engine URL" },
    { "/max_iters", "tool rounds one request may run" },
    { "/ui", "the screen: ansi, or notcurses (restarts into this session); theme: the colours" },
    { "/ctx", "context window: a size, max, default — or a picker" },
    { "/temp", "temperature (-1 = the server's default)" },
    { "/host", "the ollama host" },
    { "/keepalive", "how long ollama keeps the model loaded" },
    { "/save", "save the transcript as markdown" },
    { "/history", "the last queries" },
    { "/cd", "change the working directory" },
    { "/pwd", "show the working directory" },
    { "/skills", "list the skills · reload · new NAME" },
    { "/memory", "the project memory: on, off, clear, update, every N, idle N" },
    { "/resume", "continue an earlier session" },
    { "/hooks", "shell commands for this project: before or after a tool call, at start and at the end of a request" },
    { "/permissions", "the project's saved rules: always-allow ones, and deny/ask guards on a tool and a pattern: add, remove, clear" },
    { "/init", "have the model write AGENTS.md for this project" },
    { "/cost", "tokens, model calls and time of this session" },
    { "/usage", "tokens per model, the advisor's too, and the estimated cost of the hosted ones" },
    { "/banner", "the Corbie Nest box again, as a window: close it with its X or Esc" },
    { "/diff", "the working-tree diff, without sending it to the model" },
    { "/rewind", "go back to an earlier request: files, conversation or both" },
    { "/quit", "leave" },
    { "/exit", "leave" },
};

/* What a command takes as its next word, for the same list: "word:what it does", '|' between
 * them. The key is the command line up to the word being typed. (/effort and /cd are worked
 * out in suggest(): the levels are the model's, the directories the disk's.) */
static const struct { const char *after, *opts; } SLASH_ARGS[] = {
    { "/ui", "ansi:the ANSI screen (default)|notcurses:the notcurses screen: planes, true colour (needs notcurses installed at build time)|theme:the colour theme: a list to pick from, or a name" },
    { "/goal", "min:a lower score is better|max:a higher score is better (the default)|-n:rounds, 1 to 10|-k:candidates, 2 to 8|-m:models, a,b,c (round-robin)" },
    { "/mode", "manual:ask before every edit and command|accept-edits:file edits go ahead, commands still ask|plan:read-only: the model proposes a plan|auto:nothing asks (dangerous)" },
    { "/think", "on:think on every call|off:do not think|auto:think once per request, not after every tool result|show:show the thinking|hide:hide the thinking" },
    { "/yolo", "on:approve every tool call (dangerous)|off:back to manual confirmations" },
    { "/tools", "on:the model may call tools|off:chat only" },
    { "/web", "on:the model may search and read the web|off:no web access|engine:the search engine: a URL with %s for the query, or default" },
    { "/web engine", "default:back to the built-in engine" },
    { "/memory", "on:keep the project memory up to date|off:stop updating it|update:run the pending update now|clear:remove the memory file|every:update every N requests|idle:update after N seconds idle at the prompt" },
    { "/memory idle", "off:wait for the next update or the exit instead" },
    { "/hooks", "add:a hook line: pre|post TOOL COMMAND, start|stop COMMAND|remove:remove hook N|clear:remove them all" },
    { "/permissions", "add:save a rule: edit, bash WORDS, fetch HOST, or deny|ask TOOL PATTERN|remove:remove rule N|clear:remove them all" },
    { "/permissions add", "edit:file edits|bash:a command, by its leading words|fetch:a host|deny:stop a tool on a pattern (* any text)|ask:ask before a tool on a pattern" },
    { "/advisor", "off:no advisor|guidance:how much the agent leans on it|effort:how hard it thinks|window:minutes after which a long request gets its consultations again|ctx:its context window" },
    { "/subagent", "default:the model doing the work|effort:how hard the sub-agents' model thinks" },
    { "/advisor guidance", "light:consulted rarely|normal:the default — and brought in when tool calls keep failing|strong:also reviews a request that changed a lot before it ends|max:also reviews any change, and checks the first change before it is made" },
    { "/advisor ctx", "auto:the main window, within the advisor's bounds" },
    { "/advisor window", "15:the default, a quarter of an hour|off:never: the limit holds for the whole request" },
    { "/usage", "price:what a model costs per million tokens — MODEL IN OUT, or MODEL default" },
    { "/skills", "reload:read the SKILL.md files again|new:scaffold a skill: /skills new NAME" },
    { "/system", "clear:remove the extra instructions" },
    { "/keepalive", "default:the server's own|30m:half an hour|-1:forever|0:unload right away" },
    { "/ctx", "max:the largest the model takes|default:back to the default size" },
    { "/resume", "all:sessions of every directory, not only this one" },
};

/* what each model was sent and gave in this session: /usage, and what it estimates the cost from */
typedef struct { char *model; long in, out; int calls; long cr, cw; } usage_row;   /* in = all input; cr/cw = its cache reads and writes */
static usage_row *g_usage = NULL;
static int g_n_usage = 0;
static void usage_add(const char *model, long in, long out, long cr, long cw, int calls) {
    if (!model || !*model) model = "(no model)";
    int i = 0;
    while (i < g_n_usage && strcmp(g_usage[i].model, model)) i++;
    if (i == g_n_usage) {
        g_usage = xrealloc(g_usage, sizeof *g_usage * (size_t)(g_n_usage + 1));
        g_usage[g_n_usage++] = (usage_row){ xstrdup(model), 0, 0, 0, 0, 0 };
    }
    g_usage[i].in += in; g_usage[i].out += out; g_usage[i].calls += calls; g_usage[i].cr += cr; g_usage[i].cw += cw;
}
static void usage_clear(void) {
    for (int i = 0; i < g_n_usage; i++) free(g_usage[i].model);
    free(g_usage); g_usage = NULL; g_n_usage = 0;
}

/* the advisor's window as it is said: "15 minutes", "90 seconds" */
static const char *fmt_window(void) {
    static char buf[32];
    if (g_cfg.advisor_window % 60) snprintf(buf, sizeof buf, "%d second%s", g_cfg.advisor_window, g_cfg.advisor_window == 1 ? "" : "s");
    else snprintf(buf, sizeof buf, "%d minute%s", g_cfg.advisor_window / 60, g_cfg.advisor_window == 60 ? "" : "s");
    return buf;
}

/* one model call finished: fold its stats into the session totals — account() for the model
 * doing the work, account_as() for a call to another one (the advisor) */
static void account_as(const char *model, const chat_stats *st) {
    usage_add(model, st->prompt_tokens, st->eval_tokens, st->cache_read, st->cache_write, 1);
    g_session.prompt_tokens += st->prompt_tokens;
    g_session.cache_read += st->cache_read;
    g_session.eval_tokens += st->eval_tokens;
    g_session.model_seconds += st->total_seconds;
    g_session.eval_seconds += st->eval_seconds;
    g_session.think_seconds += st->think_seconds; g_session.think_chunks += st->think_chunks;
    g_session.calls++;
}
static void account(const chat_stats *st) { account_as(g_cfg.model, st); }

/* ---------- how full the context is ----------
 * Ollama reports prompt_eval_count, but only for calls that have already happened: everything
 * appended since — an assistant reply, a 64 KB tool result — is unmeasured, and that is exactly
 * when the window overflows. So the unmeasured part is estimated from its byte count, with
 * bytes-per-token calibrated on the last measured call (code and prose differ far too much for
 * a fixed 4). Overflowing matters because Ollama does not refuse an over-long prompt: it drops
 * whole messages from the front until the rest fits, and once the user's own request has fallen
 * off, the model is answering a conversation that no longer contains the question (qwen3.8 is
 * the one that says so out loud: "no user query found in messages"). */
static double g_bytes_per_token = 3.5;   /* calibrated below; only the starting guess */
static size_t g_sys_bytes = 0;           /* system prompt as last built (messages_with_system) */
static size_t g_tools_bytes = 0;         /* tool definitions as JSON (measured once) */
static size_t g_measured_bytes = 0;      /* prompt bytes of the last measured call */

/* bytes of a conversation as sent (content plus the tool-call JSON; the template's own tags and
 * the tool definitions are not counted, which the calibrated ratio absorbs) */
static size_t conv_bytes(cJSON *msgs) {
    size_t n = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, msgs) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(c)) n += strlen(c->valuestring);
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        if (tc) { char *t = cJSON_PrintUnformatted(tc); if (t) { n += strlen(t); free(t); } }
    }
    return n;
}

/* the tool definitions go into every prompt too, and they are far from free (a few KB) */
static size_t tools_bytes(void) {
    if (g_cfg.no_tools || !g_model_tools) return 0;
    if (!g_tools_bytes && g_tools) { char *t = cJSON_PrintUnformatted(g_tools); if (t) { g_tools_bytes = strlen(t); free(t); } }
    return g_tools_bytes;
}

/* everything the next call would send */
static size_t prompt_bytes(void) { return g_sys_bytes + tools_bytes() + conv_bytes(g_messages); }

/* prompt tokens the next call would use: the last measured figure plus everything added since */
static int ctx_estimate(void) {
    size_t now = prompt_bytes();
    if (g_session.last_prompt_tokens > 0 && now >= g_measured_bytes)
        return g_session.last_prompt_tokens + (int)((double)(now - g_measured_bytes) / g_bytes_per_token);
    return (int)((double)now / g_bytes_per_token);   /* nothing measured yet, or the conversation shrank */
}

/* usage as a percentage of the window (0 when num_ctx is left to the server) */
static int ctx_pct(void) { return g_cfg.num_ctx > 0 ? (int)(100.0 * ctx_estimate() / g_cfg.num_ctx) : 0; }

/* a call came back with a real prompt_eval_count: re-calibrate on it */
static void ctx_measured(int prompt_tokens, size_t sent_bytes) {
    if (prompt_tokens <= 0) return;
    double r = (double)sent_bytes / prompt_tokens;
    if (r > 1.0 && r < 12.0) g_bytes_per_token = r;
    g_measured_bytes = sent_bytes;
}

/* ---------- context hygiene: elide old tool results ----------
 * Tool results (file contents, command output, grep hits) are the bulk of a long
 * conversation and are re-sent with every model call, but they go stale fast: once the model
 * has acted on them, the assistant's own text carries what mattered. So when the context is
 * ELIDE_PCT% full, tool results from requests *before the previous one* are replaced in place
 * by a short stub (the tool name and the first line, so the model still sees what it looked
 * at). The model can always call the tool again; the previous request's results stay intact
 * because "now apply that" style follow-ups still need them. This is far cheaper than
 * compaction (no model call) and usually postpones it for a long time. */
#define ELIDE_PCT  50      /* context usage (% of num_ctx) from which elision kicks in */
#define ELIDE_KEEP 160     /* bytes of the original kept in the stub */
#define ELIDE_MIN  400     /* smaller results are not worth eliding */
static const char ELIDE_MARK[] = "[earlier output elided to save context";
static int g_prev_request_first = -1;   /* where the most recent finished request started */

/* Replace every tool result before `upto` with its stub; returns how many were elided. */
static int elide_tool_results(cJSON *msgs, int upto) {
    int n = 0; size_t saved = 0;
    for (int i = 0; i < upto && i < cJSON_GetArraySize(msgs); i++) {
        cJSON *m = cJSON_GetArrayItem(msgs, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!cJSON_IsString(role) || strcmp(role->valuestring, "tool") || !cJSON_IsString(c)) continue;
        const char *txt = c->valuestring; size_t len = strlen(txt);
        if (len < ELIDE_MIN || !strncmp(txt, ELIDE_MARK, sizeof ELIDE_MARK - 1)) continue;
        cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name");
        size_t keep = ELIDE_KEEP; if (keep > len) keep = len;
        while (keep > 0 && ((unsigned char)txt[keep] & 0xC0) == 0x80) keep--;   /* UTF-8 boundary */
        sbuf b; sb_init(&b);
        sb_printf(&b, "%s; %s returned %zu bytes, call it again if you need the details]\n", ELIDE_MARK, cJSON_IsString(tn) ? tn->valuestring : "the tool", len);
        sb_append(&b, txt, keep); if (keep < len) sb_puts(&b, "…");
        cJSON_SetValuestring(c, b.data); sb_free(&b);
        n++; saved += len - keep;
    }
    if (n) printf(C_GRAY "⋯ elided %d old tool result%s (~%zu KB) to save context" C_RESET "\n", n, n == 1 ? "" : "s", saved / 1024);
    return n;
}

/* the conversation, when it is worth it */
static int elide_old_tool_results(int upto) {
    if (g_cfg.num_ctx <= 0 || ctx_pct() < ELIDE_PCT) return 0;
    return elide_tool_results(g_messages, upto);
}

/* /ctx given while a request is in flight: the running turn keeps the window it started with (Ollama
 * reloads the model for a new num_ctx, and the calls of this turn would each pay for it). begin_request()
 * applies it when the next request starts. */
static int g_pending_ctx = -1;
/* /advisor guidance given while a request is in flight: the guidance is in the system prompt, so the
 * change waits for the next request like the window does (see g_pending_ctx) */
static int g_pending_guidance = -1;
/* settings given while busy that change the prompt or the tool list (/think, /effort, /mode, /advisor window):
 * the lines are run, in the order typed, when the next request starts (run_slash_while_busy) */
static char **g_pending_lines = NULL;
static int g_pending_n = 0;
static int handle_slash(char *line);

/* the settings given while a request was in flight take effect now, between two requests: when the turn
 * ends (so they are not lost if the user quits first) and again when the next one starts (a no-op if none) */
static void apply_pending(void) {
    if (g_pending_ctx >= 0 || g_pending_guidance >= 0) {   /* /ctx and /advisor guidance given during the last request */
        if (g_pending_ctx >= 0) { g_cfg.num_ctx = g_pending_ctx; g_pending_ctx = -1; }
        if (g_pending_guidance >= 0) { g_cfg.advisor_guidance = g_pending_guidance; g_pending_guidance = -1; }
        config_save();
    }
    for (int i = 0; i < g_pending_n; i++) { handle_slash(g_pending_lines[i]); free(g_pending_lines[i]); }
    g_pending_n = 0;
}

/* a user request starts: remember where it begins in the conversation (for /rewind, memory) */
static int begin_request(void) {
    apply_pending();
    /* a conversation that came from a stopped request (or a session saved by one) may hold calls without
     * results and text the server refuses: mend it before the next request goes out */
    int mended = messages_repair(g_messages);
    if (mended) printf(C_GRAY "⋯ mended the conversation: %d fix%s (unanswered tool calls, invalid UTF-8)" C_RESET "\n", mended, mended == 1 ? "" : "es");
    int first = cJSON_GetArraySize(g_messages);
    if (g_prev_request_first >= 0 && g_prev_request_first <= first) elide_old_tool_results(g_prev_request_first);
    g_prev_request_first = first;
    g_advisor_uses = 0; g_advisor_since = time(NULL); g_advisor_reviewed = g_advisor_checked = g_advisor_stepped_in = false;
    g_fail_rounds = 0;
    tools_checkpoint_turn(first);
    return first;
}

/* ---------- project memory ----------
 * Like Claude Code's auto-memory: a per-project markdown file (MEMORY_PATH, in the working
 * directory) that the *model* curates. It is loaded into the system prompt of every request,
 * the model may edit it directly with edit_file, and at the end of every request a quiet
 * extraction call asks the model whether this exchange taught it anything durable — who the
 * user is, feedback on how to work, project goals/decisions/constraints, references — and,
 * if so, to return the updated file (or NO_CHANGE). Off with /memory off (saved in config). */
#define MEMORY_PATH   ".corbienest/memory.md"
#define MEMORY_MAX    (24 * 1024)     /* larger files are cut when loaded; the model is told to keep it short */
#define MEMORY_MAX_TOKENS 6144   /* num_predict cap for the memory-update call (a full MEMORY_MAX file fits) */

static void load_memory(void) {
    free(g_memory); g_memory = NULL;
    if (!is_file(MEMORY_PATH)) return;
    size_t n; char *d = read_whole_file(MEMORY_PATH, &n, MEMORY_MAX);
    if (!d) return;
    if (n >= MEMORY_MAX) { char *nl = strrchr(d, '\n'); if (nl) *nl = 0; }
    g_memory = d;
}

static const char *MEMORY_TEMPLATE =
    "# Project memory\n\n"
    "<!-- Kept by corbienest (" MEMORY_PATH "). Loaded into every request; updated by the model after each request\n"
    "     and editable by hand. One durable fact per bullet; keep it short. -->\n\n"
    "## User\n\n## Feedback\n\n## Project\n\n## Reference\n";

static const char *MEMORY_RULES =
    "The memory file has four sections. Save only what will still matter in a future session:\n"
    "- User: who the user is (role, expertise, preferences, tools they like).\n"
    "- Feedback: guidance the user gave on how to work — corrections and confirmed approaches — with the why.\n"
    "- Project: goals, decisions, constraints and ongoing work that cannot be derived from the code or git history; write dates as absolute dates.\n"
    "- Reference: pointers to external things (URLs, tickets, dashboards, hosts).\n"
    "Do NOT save what the repository already records (code structure, what a file contains, fixes just made), transient task state, "
    "or anything only relevant to the current conversation. Update or delete facts that turned out wrong; merge duplicates. "
    "Keep the whole file under ~80 lines, one bullet per fact.";

/* Strip an optional ```markdown fence around the model's reply. Returns a malloc'd string. */
static char *unfence(const char *t) {
    while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t') t++;
    size_t n = strlen(t);
    while (n && (t[n-1] == ' ' || t[n-1] == '\n' || t[n-1] == '\r' || t[n-1] == '\t')) n--;
    if (n >= 6 && !strncmp(t, "```", 3) && !strncmp(t + n - 3, "```", 3)) {
        const char *b = strchr(t, '\n'); if (!b || b >= t + n - 3) return xstrndup(t, n);
        b++; size_t e = n - 3; while (e > (size_t)(b - t) && (t[e-1] == '\n' || t[e-1] == ' ')) e--;
        return xstrndup(b, e - (size_t)(b - t));
    }
    return xstrndup(t, n);
}

/* Ask the model (quietly, without tools) to fold anything worth remembering from the
 * exchanges since g_messages[first] into the memory file. In one-shot (-p) runs the file is
 * only touched if the project directory already exists, so `corbienest -p` leaves no litter.
 *
 * This is a separate prompt, so it evicts Ollama's prompt cache for the conversation and the
 * next request re-evaluates the whole context: that is why it is not run after every request
 * but batched — memory_note() counts finished requests and memory_flush() runs the call once
 * g_cfg.memory_every of them are pending, or when the conversation is about to go away
 * (exit, /clear, /compact, /cd, /resume). */
static int g_mem_first = -1;      /* index of the first message not yet folded into memory (-1 = none pending) */
static int g_mem_pending = 0;     /* finished requests since the last flush */
static bool g_mem_exiting = false;/* the last flush, on the way out: say that Ctrl-C skips it */

/* The model that does the work is a local one, or Anthropic (anthropic:MODEL / claude:MODEL).
 * xAI, OpenAI, DeepSeek and Kimi stay advisors here: a turn never sends their prefix to Ollama, and the sub-agents
 * and /model stay Ollama or Anthropic. The workers of /goal may be any of them (worker_remote()). */
static bool model_messages(const char *name) {
    const provider_def *p = name ? provider_find(name, NULL) : NULL;
    return p && p->style == PROVIDER_MESSAGES;
}
static bool model_hosted(void) { return model_messages(g_cfg.model); }
/* "" is the working model when none is chosen yet (no Ollama model, no provider key): the screen starts and
 * says so, and nothing is sent until /model picks one. NULL is "not read yet" (refresh_model_caps picks). */
static bool model_set(void) { return g_cfg.model && *g_cfg.model; }

static cJSON *model_chat(cJSON *messages, cJSON *tools, chat_stats *stats, bool *aborted) {
    const char *model = ollama_call.model ? ollama_call.model : g_cfg.model;
    if (!model || !*model) {   /* no model yet: nothing is sent (g_model_err says how to get one) */
        *aborted = false;
        if (stats) memset(stats, 0, sizeof *stats);
        snprintf(ollama_error, sizeof ollama_error, "%s", g_model_err);
        printf(C_RED "✗ %s" C_RESET "\n", g_model_err);
        return NULL;
    }
    const provider_def *p = provider_find(model, NULL);
    if (p && p->style == PROVIDER_MESSAGES) return provider_turn(messages, tools, stats, aborted);
    if (p) {
        *aborted = false;
        if (stats) memset(stats, 0, sizeof *stats);
        snprintf(ollama_error, sizeof ollama_error, "%s is an advisor — use /advisor %s", p->label, model);
        printf(C_RED "✗ %s" C_RESET "\n", ollama_error);
        return NULL;
    }
    return ollama_chat(messages, tools, stats, aborted);
}

static void memory_extract(int first) {
    if (!g_cfg.interactive && !is_dir(".corbienest")) return;
    int total = cJSON_GetArraySize(g_messages);
    if (first < 0 || first >= total) first = 0;   /* e.g. auto-compact replaced the conversation */
    if (total - first < 2) return;                 /* no reply: nothing to learn */
    cJSON *msgs = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    sbuf sp; sb_init(&sp);
    sb_printf(&sp, "You maintain the persistent memory file %s of a coding assistant. You are shown the exchange that just finished. %s\n\n"
                   "Current memory file:\n\n%s", MEMORY_PATH, MEMORY_RULES, g_memory ? g_memory : MEMORY_TEMPLATE);
    cJSON_AddStringToObject(sys, "content", sp.data); sb_free(&sp);
    cJSON_AddItemToArray(msgs, sys);
    for (int i = first; i < total; i++) {
        cJSON *m = cJSON_GetArrayItem(g_messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        const char *r = cJSON_IsString(role) ? role->valuestring : "user";
        cJSON *n = cJSON_CreateObject();
        /* keep it plain: tool results become user text (no tools are offered on this call) */
        cJSON_AddStringToObject(n, "role", !strcmp(r, "assistant") ? "assistant" : "user");
        sbuf b; sb_init(&b);
        if (!strcmp(r, "tool")) { cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name"); sb_printf(&b, "[result of %s]\n", cJSON_IsString(tn) ? tn->valuestring : "tool"); }
        if (cJSON_IsString(c) && c->valuestring[0]) {
            size_t len = strlen(c->valuestring), cap = 4000, keep = utf8_cut(c->valuestring, len, cap);
            sb_append(&b, c->valuestring, keep);
            if (len > keep) sb_puts(&b, "\n[…truncated]");
        }
        if (cJSON_IsArray(tc)) {
            char *t = cJSON_PrintUnformatted(tc); size_t tl = strlen(t);
            sb_printf(&b, "%s[tool calls] %.*s", b.len ? "\n" : "", (int)utf8_cut(t, tl, 600), t); free(t);
        }
        if (!b.len) sb_puts(&b, "(empty)");
        cJSON_AddStringToObject(n, "content", b.data); sb_free(&b);
        cJSON_AddItemToArray(msgs, n);
    }
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content",
        "Update the memory file with anything durable this exchange revealed, following the rules. "
        "If nothing is worth saving, reply with exactly NO_CHANGE. Otherwise reply with the complete updated memory file "
        "in markdown, keeping the existing headings, and nothing else.");
    cJSON_AddItemToArray(msgs, u);

    if (g_cfg.interactive) { printf(C_DIM "✎ updating memory…%s" C_RESET, g_mem_exiting ? "  (Ctrl-C skips it)" : ""); fflush(stdout); }
    chat_stats st; bool ab = false;
    /* a background call: no thinking (it only adds latency here), a hard cap on the reply so a
     * runaway generation cannot hang the prompt, and an honest status-bar label */
    ollama_quiet = true;
    ollama_call.think = 0; ollama_call.num_predict = MEMORY_MAX_TOKENS; ollama_call.busy = "updating memory";
    cJSON *reply = model_chat(msgs, NULL, &st, &ab);
    ollama_call_reset();
    ollama_quiet = false;
    cJSON_Delete(msgs);
    account(&st);
    if (g_cfg.interactive) { fputs("\r\x1b[2K", stdout); term_status_refresh(); }
    if (!reply) return;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(reply, "content");
    char *text = unfence(cJSON_IsString(c) ? c->valuestring : "");
    cJSON_Delete(reply);
    bool cut = !strcmp(st.done_reason, "length");   /* hit num_predict: never write a truncated file */
    bool nochange = ab || cut || !*text || strstr(text, "NO_CHANGE") || *text != '#' || strlen(text) > 2 * MEMORY_MAX;
    bool written = false;
    if (!nochange && (!g_memory || strcmp(text, g_memory))) {
        size_t n = strlen(text);
        if (n && text[n-1] != '\n') { text = xrealloc(text, n + 2); text[n++] = '\n'; text[n] = 0; }
        if (mkdir_p(".corbienest") == 0 && write_whole_file_atomic(MEMORY_PATH, text, n) == 0) {
            load_memory(); written = true;
            printf(C_DIM "✎ memory updated (%s) · %.1fs" C_RESET "\n", MEMORY_PATH, st.total_seconds);
        }
    }
    /* show what this step cost, so a slow turn is never a mystery */
    if (!written && g_cfg.interactive && st.total_seconds > 0)
        printf(C_GRAY "✎ memory: %s · %.1fs%s" C_RESET "\n", cut ? "reply too long, ignored" : "no change", st.total_seconds,
               st.total_seconds >= 15 ? "  (slow? /memory off skips this step)" : "");
    free(text);
}

/* Run the pending extraction now (if anything is pending). */
static void memory_flush(void) {
    if (!g_cfg.memory || g_mem_first < 0) { g_mem_first = -1; g_mem_pending = 0; return; }
    int first = g_mem_first;
    g_mem_first = -1; g_mem_pending = 0;
    if (first >= cJSON_GetArraySize(g_messages)) return;   /* rewound past it: nothing left to learn */
    memory_extract(first);
}

/* Called by the editor once the prompt has been idle for memory_idle seconds (see
 * term_idle_hook): the extraction happens while the reply is being read instead of on the way
 * out, so quitting is not held up by a model call. Typing cancels it, and the batch simply
 * waits for the next chance — so a user who keeps working still gets the batched cadence and
 * a warm prompt cache. */
static void memory_idle_hook(void) { memory_flush(); }

/* How long the next prompt should wait before folding a finished request into memory
 * (0 = not at all: nothing pending, or the idle flush is switched off). */
static void memory_arm_idle(void) {
    term_idle_ms = (g_cfg.memory && g_cfg.memory_idle > 0 && g_mem_first >= 0) ? g_cfg.memory_idle * 1000 : 0;
}

/* A request finished (it started at g_messages[first]): remember it for the next flush,
 * and flush when enough have accumulated. Aborted requests are not counted but stay
 * covered by the range. One-shot runs flush at once (the process is about to exit). */
static void memory_note(int first, bool aborted) {
    if (!g_cfg.memory || !model_set()) return;   /* a refused turn (no model yet) is not a request */
    int total = cJSON_GetArraySize(g_messages);
    if (first < 0 || first > total) first = 0;   /* the conversation was replaced meanwhile (compact) */
    if (g_mem_first < 0 || first < g_mem_first) g_mem_first = first;
    if (aborted) return;
    g_mem_pending++;
    if (!g_cfg.interactive || g_mem_pending >= g_cfg.memory_every) memory_flush();
}

/* "# fact" typed at the prompt: append a bullet to a section of the memory file without a
 * model call (like Claude Code's # shortcut). Interactive: pick the section from a menu;
 * otherwise it goes under Project. */
static void memory_quick_add(const char *fact) {
    static const char *secs[] = { "Project", "User", "Feedback", "Reference" };
    static const char *descs[] = { "goals, decisions, constraints, ongoing work", "who you are, how you like to work", "how the assistant should work (corrections, confirmed approaches)", "URLs, tickets, dashboards, hosts" };
    int sec = 0;
    if (g_cfg.interactive) {
        char title[200]; snprintf(title, sizeof title, "Remember \"%.60s%s\" under which section?", fact, strlen(fact) > 60 ? "…" : "");
        sec = term_select(title, secs, descs, 4, 0);
        if (sec < 0) { printf(C_DIM "not saved" C_RESET "\n"); return; }
    }
    size_t n = 0; char *old = is_file(MEMORY_PATH) ? read_whole_file(MEMORY_PATH, &n, 4 * 1024 * 1024) : NULL;
    sbuf f; sb_init(&f);
    sb_puts(&f, old && *old ? old : MEMORY_TEMPLATE);
    free(old);
    char head[32]; snprintf(head, sizeof head, "## %s", secs[sec]);
    char *h = strstr(f.data, head);
    if (!h) { if (f.data[f.len-1] != '\n') sb_putc(&f, '\n'); sb_printf(&f, "\n%s\n", head); h = strstr(f.data, head); }
    /* insert after the last non-blank line of that section (before the next "## " or EOF) */
    char *next = strstr(h + 3, "\n## "); size_t sec_end = next ? (size_t)(next - f.data) + 1 : f.len;
    size_t ins = sec_end; while (ins > (size_t)(h - f.data) && f.data[ins-1] == '\n') ins--;
    sbuf line; sb_init(&line); sb_printf(&line, "\n- %s", fact);
    sbuf out; sb_init(&out); sb_append(&out, f.data, ins); sb_append(&out, line.data, line.len);
    if (ins < f.len) sb_append(&out, f.data + ins, f.len - ins); else sb_putc(&out, '\n');
    if (out.data[out.len-1] != '\n') sb_putc(&out, '\n');
    if (mkdir_p(".corbienest") == 0 && write_whole_file_atomic(MEMORY_PATH, out.data, out.len) == 0) {
        load_memory();
        printf(C_GREEN "✓ remembered under %s" C_RESET C_DIM " (%s)" C_RESET "\n", secs[sec], MEMORY_PATH);
    } else printf(C_RED "✗ cannot write %s: %s" C_RESET "\n", MEMORY_PATH, strerror(errno));
    sb_free(&f); sb_free(&line); sb_free(&out);
}

static const char *memory_cadence(void) {
    static char b[160];
    char idle[64] = "";
    if (g_cfg.memory_idle > 0) snprintf(idle, sizeof idle, ", after %ds idle at the prompt", g_cfg.memory_idle);
    if (g_cfg.memory_every <= 1) snprintf(b, sizeof b, "after each request");
    else snprintf(b, sizeof b, "every %d requests%s (and at exit, /clear, /compact)", g_cfg.memory_every, idle);
    return b;
}
static void cmd_memory(const char *arg) {
    if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
        g_cfg.memory = !strcmp(arg, "on"); config_save();
        if (g_cfg.memory) printf(C_GREEN "✓ memory on" C_RESET C_DIM " — " MEMORY_PATH " is updated %s" C_RESET "\n", memory_cadence());
        else printf(C_GREEN "✓ memory off" C_RESET "\n");
        return;
    }
    if (arg && !strncmp(arg, "every", 5)) {
        int n = atoi(arg + 5);
        if (n <= 0) { printf("usage: /memory every N   (N ≥ 1; larger keeps the model's prompt cache warm between requests)\n"); return; }
        g_cfg.memory_every = n; config_save();
        printf(C_GREEN "✓ memory updated %s" C_RESET "\n", memory_cadence());
        if (g_mem_pending >= n) memory_flush();
        return;
    }
    if (arg && !strncmp(arg, "idle", 4)) {
        const char *v = arg + 4; while (*v == ' ') v++;
        int n = !strcmp(v, "off") ? 0 : atoi(v);
        if (n < 0 || (!n && strcmp(v, "off") && strcmp(v, "0"))) { printf("usage: /memory idle N   (seconds idle at the prompt before a pending update runs; 'off' waits for exit)\n"); return; }
        g_cfg.memory_idle = n; config_save();
        if (n) printf(C_GREEN "✓ pending memory update runs after %ds idle at the prompt" C_RESET "\n", n);
        else printf(C_GREEN "✓ memory idle update off" C_RESET C_DIM " — pending updates wait for %d requests or exit" C_RESET "\n", g_cfg.memory_every);
        return;
    }
    if (arg && !strcmp(arg, "update")) { if (g_mem_first < 0) printf(C_DIM "nothing pending" C_RESET "\n"); else memory_flush(); return; }
    if (arg && !strcmp(arg, "clear")) {
        if (is_file(MEMORY_PATH) && unlink(MEMORY_PATH) == 0) { load_memory(); printf(C_GREEN "✓ removed %s" C_RESET "\n", MEMORY_PATH); }
        else printf(C_DIM "no memory file" C_RESET "\n");
        return;
    }
    if (arg && *arg) { printf("usage: /memory [on|off|clear|update|every N|idle N]\n"); return; }
    printf(C_DIM "%s — %s; loaded into the system prompt · /memory on|off|clear|update|every N|idle N" C_RESET "\n", MEMORY_PATH, g_cfg.memory ? memory_cadence() : "auto-update is off");
    if (g_cfg.memory && g_mem_pending > 0) printf(C_DIM "(%d request%s pending extraction — /memory update runs it now)" C_RESET "\n", g_mem_pending, g_mem_pending == 1 ? "" : "s");
    if (!g_memory) { printf(C_DIM "(no memory yet — nothing durable has been recorded)" C_RESET "\n"); return; }
    fputs(g_memory, stdout);
    if (g_memory[strlen(g_memory) - 1] != '\n') printf("\n");
}

/* ---------- sessions (--continue / --resume / /resume) ----------
 * Every conversation is persisted as JSON under config_dir()/sessions/<id>.json after each
 * request (id = start time + pid). --continue reloads the latest session started in this
 * working directory, --resume [ID] / /resume [ID] pick one from a menu (or by id). The
 * latest SESSIONS_KEEP files are kept. */
#define SESSIONS_KEEP 100
static void print_result_preview(const char *text, int lines);
static void print_result_folded(const char *text, int preview_lines, const char *lead);
static void print_task_lines(const char *text);
static void echo_queued(const char *text);
static void tool_arg_summary(const char *name, cJSON *args, char *out, size_t n);

static const char *sessions_dir(void) {
    static char d[1200];
    if (!d[0]) { snprintf(d, sizeof d, "%s/sessions", config_dir()); mkdir_p(d); }
    return d;
}

static void session_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/%s.json", sessions_dir(), id); }

/* first line of the first user message, for menus */
static char *session_title_of(cJSON *messages) {
    cJSON *m;
    cJSON_ArrayForEach(m, messages) {
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(r) && !strcmp(r->valuestring, "user") && cJSON_IsString(c) && c->valuestring[0]) {
            const char *t = c->valuestring; while (*t == ' ' || *t == '\n') t++;
            size_t n = strcspn(t, "\n"); if (n > 70) n = 70;
            while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
            char *out = xmalloc(n + 4); memcpy(out, t, n); out[n] = 0;
            if (t[n]) strcat(out, "…");
            return out;
        }
    }
    return xstrdup("(empty)");
}

static void sessions_prune(void) {
    DIR *d = opendir(sessions_dir()); if (!d) return;
    struct { char name[128]; time_t mt; } *ents = NULL; int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", sessions_dir(), e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; ents = xrealloc(ents, sizeof *ents * (size_t)cap); }
        snprintf(ents[n].name, sizeof ents[n].name, "%s", e->d_name); ents[n].mt = st.st_mtime; n++;
    }
    closedir(d);
    while (n > SESSIONS_KEEP) {   /* drop the oldest one at a time (n is small) */
        int oldest = 0; for (int i = 1; i < n; i++) if (ents[i].mt < ents[oldest].mt) oldest = i;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", sessions_dir(), ents[oldest].name); unlink(p);
        ents[oldest] = ents[--n];
    }
    free(ents);
}

static void session_new_id(void) {
    time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(g_session_id, sizeof g_session_id, "%s-%d", ts, (int)getpid());
}

/* What the user typed for each message they sent: the line as typed (its ↑ history entry) and the message as it was
 * sent (after @mentions). A resumed session's history and its replay come from this: the other user messages are the
 * app's own (a compacted summary, an advisor's note), and are neither. */
typedef struct { char *raw, *msg; } typed_t;
static typed_t *g_typed; static int g_typed_n, g_typed_cap;
static bool g_typed_known = true;   /* false for a session saved before this record existed: every user message is replayed */
static void typed_note(const char *raw, const char *msg) {
    if (g_typed_n == g_typed_cap) { g_typed_cap = g_typed_cap ? g_typed_cap * 2 : 16; g_typed = xrealloc(g_typed, sizeof *g_typed * (size_t)g_typed_cap); }
    g_typed[g_typed_n].raw = xstrdup(raw ? raw : ""); g_typed[g_typed_n].msg = xstrdup(msg ? msg : "");
    g_typed_n++;
}
static void typed_clear(void) {
    for (int i = 0; i < g_typed_n; i++) { free(g_typed[i].raw); free(g_typed[i].msg); }
    g_typed_n = 0; g_typed_known = true;
}

static void session_save(void) {
    if (!g_messages || cJSON_GetArraySize(g_messages) == 0) return;
    if (!g_session_id[0]) session_new_id();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", g_session_id);
    cJSON_AddStringToObject(o, "cwd", g_cwd);
    cJSON_AddStringToObject(o, "model", g_cfg.model ? g_cfg.model : "");
    cJSON_AddNumberToObject(o, "updated", (double)time(NULL));
    char *title = session_title_of(g_messages); cJSON_AddStringToObject(o, "title", title); free(title);
    cJSON_AddNumberToObject(o, "prompt_tokens", (double)g_session.prompt_tokens);
    if (g_session.cache_read) cJSON_AddNumberToObject(o, "cache_read", (double)g_session.cache_read);
    cJSON_AddNumberToObject(o, "eval_tokens", (double)g_session.eval_tokens);
    cJSON *us = cJSON_AddArrayToObject(o, "usage");   /* per model, for /usage after a --resume */
    for (int i = 0; i < g_n_usage; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "model", g_usage[i].model);
        cJSON_AddNumberToObject(u, "in", (double)g_usage[i].in); cJSON_AddNumberToObject(u, "out", (double)g_usage[i].out);
        cJSON_AddNumberToObject(u, "calls", g_usage[i].calls);
        if (g_usage[i].cr) cJSON_AddNumberToObject(u, "cr", (double)g_usage[i].cr);
        if (g_usage[i].cw) cJSON_AddNumberToObject(u, "cw", (double)g_usage[i].cw);
        cJSON_AddItemToArray(us, u);
    }
    cJSON *ty = cJSON_AddArrayToObject(o, "typed");   /* see typed_note() */
    for (int i = 0; i < g_typed_n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "raw", g_typed[i].raw); cJSON_AddStringToObject(t, "msg", g_typed[i].msg);
        cJSON_AddItemToArray(ty, t);
    }
    if (tools_todos_get()) cJSON_AddItemReferenceToObject(o, "todos", tools_todos_get());   /* the task list, if the model kept one */
    cJSON_AddItemReferenceToObject(o, "messages", g_messages);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char path[1400]; session_path(g_session_id, path, sizeof path);
    write_whole_file_atomic(path, txt, strlen(txt));
    free(txt);
    sessions_prune();
}

typedef struct { char id[128]; char *title; char *cwd; char *model; int nmsg; time_t updated; } session_info;

static int session_cmp(const void *a, const void *b) {
    const session_info *x = a, *y = b;
    return x->updated < y->updated ? 1 : x->updated > y->updated ? -1 : 0;
}

/* newest first; if only_cwd, just the sessions started in the current directory */
static session_info *sessions_list(bool only_cwd, int *count) {
    session_info *v = NULL; int n = 0, cap = 0;
    DIR *d = opendir(sessions_dir());
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l >= 128 || strcmp(e->d_name + l - 5, ".json")) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", sessions_dir(), e->d_name);
        size_t sz; char *txt = read_whole_file(p, &sz, 64 * 1024 * 1024);
        if (!txt) continue;
        cJSON *o = cJSON_Parse(txt); free(txt);
        if (!o) continue;
        cJSON *cwd = cJSON_GetObjectItemCaseSensitive(o, "cwd");
        if (only_cwd && !(cJSON_IsString(cwd) && !strcmp(cwd->valuestring, g_cwd))) { cJSON_Delete(o); continue; }
        if (n == cap) { cap = cap ? cap * 2 : 32; v = xrealloc(v, sizeof *v * (size_t)cap); }
        session_info *si = &v[n++]; memset(si, 0, sizeof *si);
        snprintf(si->id, sizeof si->id, "%.*s", (int)(l - 5), e->d_name);
        cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "title"), *m = cJSON_GetObjectItemCaseSensitive(o, "model"), *u = cJSON_GetObjectItemCaseSensitive(o, "updated");
        si->title = xstrdup(cJSON_IsString(t) ? t->valuestring : "?");
        si->cwd = xstrdup(cJSON_IsString(cwd) ? cwd->valuestring : "?");
        si->model = xstrdup(cJSON_IsString(m) ? m->valuestring : "?");
        si->updated = cJSON_IsNumber(u) ? (time_t)u->valuedouble : 0;
        si->nmsg = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "messages"));
        cJSON_Delete(o);
    }
    if (d) closedir(d);
    if (n) qsort(v, (size_t)n, sizeof *v, session_cmp);
    *count = n;
    return v;
}
static void sessions_free(session_info *v, int n) { for (int i = 0; i < n; i++) { free(v[i].title); free(v[i].cwd); free(v[i].model); } free(v); }

/* The conversation of a resumed session, printed as it was shown: what the user typed, the replies (as the markdown
 * printer shows them), each tool call's header and its folded result. Only the last REPLAY_MAX messages; the rest are
 * in the model's history all the same. A user message is shown only when it is one the user typed (typed_note()),
 * unless the session has no such record. */
#define REPLAY_MAX 200
static void replay_conversation(void) {
    int n = cJSON_GetArraySize(g_messages), from = n > REPLAY_MAX ? n - REPLAY_MAX : 0;
    if (from) printf(C_DIM "  … %d earlier message%s not shown" C_RESET "\n", from, from == 1 ? "" : "s");
    int tp = 0;   /* the typed entries are in the order they were sent: a user message is looked for from the last one found */
    for (int i = from; i < n; i++) {
        cJSON *m = cJSON_GetArrayItem(g_messages, i);
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        const char *role = cJSON_IsString(r) ? r->valuestring : "", *text = cJSON_IsString(c) ? c->valuestring : NULL;
        if (!strcmp(role, "user")) {
            if (!text) continue;
            if (!g_typed_known) { echo_queued(text); continue; }
            int k = tp; while (k < g_typed_n && strcmp(g_typed[k].msg, text)) k++;
            if (k == g_typed_n) continue;   /* the app's own note, not something the user typed */
            tp = k + 1;
            echo_queued(g_typed[k].raw);
        } else if (!strcmp(role, "assistant")) {
            if (text) while (*text == '\n' || *text == ' ') text++;
            if (text && *text) {
                fputs(C_ORANGE "●" C_RESET " ", stdout);
                md_state md; md_init(&md); md.col = 2;
                md_feed(&md, text, strlen(text)); md_finish(&md);
            }
            cJSON *call;
            cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(m, "tool_calls")) {
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
                cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL, *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                const char *name = cJSON_IsString(nm) ? nm->valuestring : "?";
                cJSON *parsed = NULL;
                if (cJSON_IsString(args)) { parsed = cJSON_Parse(args->valuestring); args = parsed; }
                char summ[160]; tool_arg_summary(name, args, summ, sizeof summ);
                printf(C_GREEN "●" C_RESET " " C_BOLD "%s" C_RESET, name);
                if (summ[0]) printf(C_DIM "(%s)" C_RESET, summ);
                printf("\n");
                if (parsed) cJSON_Delete(parsed);
            }
        } else if (!strcmp(role, "tool")) {
            cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name");
            const char *name = cJSON_IsString(tn) ? tn->valuestring : "";
            if (!strcmp(name, "todo_write")) print_task_lines(text);
            else print_result_folded(text ? text : "", (!strcmp(name, "read_file") || !strcmp(name, "web_fetch")) ? 3 : 8, NULL);
        }
    }
    if (n > from) printf(C_DIM "  — end of the earlier conversation —" C_RESET "\n");
}

/* Load a session into g_messages; returns false if not found / unreadable. */
static bool session_load(const char *id) {
    char path[1400]; session_path(id, path, sizeof path);
    size_t sz; char *txt = read_whole_file(path, &sz, 64 * 1024 * 1024);
    if (!txt) return false;
    cJSON *o = cJSON_Parse(txt); free(txt);
    if (!o) return false;
    cJSON *msgs = cJSON_DetachItemFromObjectCaseSensitive(o, "messages");
    if (!cJSON_IsArray(msgs)) { if (msgs) cJSON_Delete(msgs); cJSON_Delete(o); return false; }
    memory_flush();
    cJSON_Delete(g_messages); g_messages = msgs; g_prev_request_first = -1;
    typed_clear();
    cJSON *td = cJSON_DetachItemFromObjectCaseSensitive(o, "todos");
    tools_todos_set(cJSON_IsArray(td) ? td : NULL);
    if (td && !cJSON_IsArray(td)) cJSON_Delete(td);
    cJSON *ty = cJSON_GetObjectItemCaseSensitive(o, "typed");
    g_typed_known = cJSON_IsArray(ty);
    if (g_typed_known) { cJSON *t; cJSON_ArrayForEach(t, ty) { cJSON *r = cJSON_GetObjectItemCaseSensitive(t, "raw"), *mm = cJSON_GetObjectItemCaseSensitive(t, "msg"); if (cJSON_IsString(r) && cJSON_IsString(mm)) typed_note(r->valuestring, mm->valuestring); } }
    tools_reset_permissions(); tools_checkpoint_clear();
    snprintf(g_session_id, sizeof g_session_id, "%s", id);
    cJSON *pt = cJSON_GetObjectItemCaseSensitive(o, "prompt_tokens"), *et = cJSON_GetObjectItemCaseSensitive(o, "eval_tokens");
    g_session.prompt_tokens = cJSON_IsNumber(pt) ? (long)pt->valuedouble : 0;
    cJSON *cr = cJSON_GetObjectItemCaseSensitive(o, "cache_read");
    g_session.cache_read = cJSON_IsNumber(cr) ? (long)cr->valuedouble : 0;
    g_session.eval_tokens = cJSON_IsNumber(et) ? (long)et->valuedouble : 0;
    usage_clear();   /* the totals are the resumed session's from here on, and so is what /usage lists */
    cJSON *u; cJSON_ArrayForEach(u, cJSON_GetObjectItemCaseSensitive(o, "usage")) {
        cJSON *um = cJSON_GetObjectItemCaseSensitive(u, "model"), *ui = cJSON_GetObjectItemCaseSensitive(u, "in"), *uo = cJSON_GetObjectItemCaseSensitive(u, "out"), *uc = cJSON_GetObjectItemCaseSensitive(u, "calls");
        cJSON *ucr = cJSON_GetObjectItemCaseSensitive(u, "cr"), *ucw = cJSON_GetObjectItemCaseSensitive(u, "cw");
        if (cJSON_IsString(um) && cJSON_IsNumber(ui) && cJSON_IsNumber(uo))
            usage_add(um->valuestring, (long)ui->valuedouble, (long)uo->valuedouble, cJSON_IsNumber(ucr) ? (long)ucr->valuedouble : 0,
                      cJSON_IsNumber(ucw) ? (long)ucw->valuedouble : 0, cJSON_IsNumber(uc) ? (int)uc->valuedouble : 0);
    }
    g_session.last_prompt_tokens = 0;
    cJSON *cwd = cJSON_GetObjectItemCaseSensitive(o, "cwd");
    /* recap: title, size, and how the last exchange ended */
    char *title = session_title_of(g_messages);
    int n = cJSON_GetArraySize(g_messages);
    printf(C_GREEN "✓ resumed session %s" C_RESET C_DIM " · %d message%s · %s%s%s" C_RESET "\n", id, n, n == 1 ? "" : "s", title,
           cJSON_IsString(cwd) && strcmp(cwd->valuestring, g_cwd) ? " · started in " : "", cJSON_IsString(cwd) && strcmp(cwd->valuestring, g_cwd) ? cwd->valuestring : "");
    free(title);
    if (g_cfg.interactive) {   /* a one-shot run (-p, a pipe) prints its answer and leaves the ↑ history alone */
        if (g_typed_known) { for (int i = 0; i < g_typed_n; i++) if (g_typed[i].raw[0] && !hist_contains(g_typed[i].raw)) hist_add(g_typed[i].raw); hist_save(); }
        replay_conversation();
        sbuf tl; sb_init(&tl); tools_todo_render(&tl);
        if (tl.len) { printf(C_BOLD "Tasks" C_RESET C_DIM " (the list as the session left it)" C_RESET "\n"); print_task_lines(tl.data); }
        sb_free(&tl);
    }
    cJSON_Delete(o);
    term_status_refresh();
    return true;
}

/* /resume [ID|all]: menu of sessions (this directory first; "all" for every directory) */
static void cmd_resume(const char *arg) {
    if (arg && *arg && strcmp(arg, "all")) { if (!session_load(arg)) printf(C_RED "no session %s" C_RESET " — /resume lists them\n", arg); return; }
    bool all = arg && !strcmp(arg, "all");
    int n; session_info *v = sessions_list(!all, &n);
    if (!n && !all) { sessions_free(v, n); v = sessions_list(false, &n); all = true; }
    if (!n) { printf(C_DIM "no saved sessions yet" C_RESET "\n"); sessions_free(v, n); return; }
    if (!g_cfg.interactive) { for (int i = 0; i < n; i++) printf("%s  %s\n", v[i].id, v[i].title); sessions_free(v, n); return; }
    const char **items = xmalloc(sizeof(char*) * (size_t)n), **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **bufs = xmalloc(sizeof(char*) * (size_t)n * 2);
    for (int i = 0; i < n; i++) {
        char when[32]; strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&v[i].updated));
        sbuf a; sb_init(&a); sb_printf(&a, "%s  %s", when, v[i].title); bufs[2*i] = sb_detach(&a); items[i] = bufs[2*i];
        sbuf b; sb_init(&b); sb_printf(&b, "%d messages · %s", v[i].nmsg, v[i].model);
        if (strcmp(v[i].cwd, g_cwd)) sb_printf(&b, " · %s", v[i].cwd);
        if (!strcmp(v[i].id, g_session_id)) sb_puts(&b, " · current");
        bufs[2*i+1] = sb_detach(&b); descs[i] = bufs[2*i+1];
    }
    int r = term_select(all ? "Resume a session (all directories)" : "Resume a session (this directory; /resume all for every directory)", items, descs, n, 0);
    if (r >= 0) session_load(v[r].id);
    else printf(C_DIM "cancelled" C_RESET "\n");
    for (int i = 0; i < 2 * n; i++) free(bufs[i]);
    free(bufs); free(items); free(descs); sessions_free(v, n);
}

/* ---------- /usage: tokens per model, and what the hosted ones cost ----------
 * The tokens are the ones each API reported for the calls of this session; the money is those
 * tokens at a price per million (model_price()). It is an estimate: the providers only tell
 * what was really billed to an admin key, and then for the whole organisation, not this session. */
static void fmt_usd(double v, char *out, size_t n) { snprintf(out, n, v > 0 && v < 0.01 ? "$%.4f" : "$%.2f", v); }

/* what the session's hosted calls come to at their prices; *priced says whether any had one */
static double usage_cost_total(bool *priced) {
    double total = 0;
    *priced = false;
    for (int i = 0; i < g_n_usage; i++) {
        int known = 0; double cost = usage_cost_usd(g_usage[i].model, g_usage[i].in, g_usage[i].out, g_usage[i].cr, g_usage[i].cw, &known);
        if (known) { total += cost; *priced = true; }
    }
    return total;
}

static void usage_price(const char *v) {
    char model[200], word[32]; double pi, po;
    while (*v == ' ') v++;
    if (sscanf(v, "%199s %lf %lf", model, &pi, &po) == 3 && pi >= 0 && po >= 0) {
        price_set(model, pi, po); config_save();
        printf(C_GREEN "✓ %s: $%g in · $%g out per million tokens" C_RESET "\n", model, pi, po);
    } else if (sscanf(v, "%199s %31s", model, word) == 2 && !strcmp(word, "default")) {
        price_set(model, -1, -1); config_save();
        if (model_price(model, &pi, &po)) printf(C_GREEN "✓ %s: back to the list price, $%g in · $%g out per million tokens" C_RESET "\n", model, pi, po);
        else printf(C_GREEN "✓ %s: no price" C_RESET "\n", model);
    } else {
        printf("usage: /usage price MODEL IN OUT   (USD per million input and output tokens, e.g. /usage price openai:gpt-5.2 1.75 14)\n"
               "       /usage price MODEL default  (back to the built-in list price)\n");
        for (int i = 0; i < g_cfg.n_prices; i++) printf(C_DIM "  %s  $%g in · $%g out" C_RESET "\n", g_cfg.prices[i].model, g_cfg.prices[i].in, g_cfg.prices[i].out);
    }
}

/* The last response's rate limits of a hosted model, as a line of /usage ("" when there are none):
 * what is left of each per-minute limit, and when the input limit is back in full. */
static void usage_limits_line(const char *model, sbuf *o) {
    ratelimit_info rl;
    if (!provider_find(model, NULL) || !ratelimit_get(model, &rl) || !rl.have) return;
    char a[32], b[32], c[32], d[32];
    fmt_tokens(rl.in_rem, a, sizeof a); fmt_tokens(rl.in_lim, b, sizeof b);
    fmt_tokens(rl.out_rem, c, sizeof c); fmt_tokens(rl.out_lim, d, sizeof d);
    sb_printf(o, "      " C_DIM "limits at its last reply: input %s of %s · output %s of %s · requests %lld of %lld a minute",
              a, b, c, d, rl.req_rem, rl.req_lim);
    if (strlen(rl.reset) >= 16) sb_printf(o, " · input back in full at %.5sZ UTC", rl.reset + 11);   /* HH:MM from RFC 3339 */
    sb_puts(o, C_RESET "\n");
}

/* /usage: the session's tokens and cost per model. On a full-screen conversation it comes up as a
 * centred window that the running request goes on underneath (Esc closes it); otherwise it is printed. */
static void cmd_usage(const char *arg) {
    if (arg && !strncmp(arg, "price", 5) && (arg[5] == ' ' || !arg[5])) { usage_price(arg + 5); return; }
    if (arg) { printf("usage: /usage   ·   /usage price MODEL IN OUT   ·   /usage price MODEL default\n"); return; }
    if (!g_n_usage) { printf(C_DIM "no model has been called in this session yet" C_RESET "\n"); return; }
    sbuf o; sb_init(&o);
    sb_printf(&o, C_BOLD "usage" C_RESET C_DIM " (this session: the tokens each API counted, input ↑ (cached included) and output ↓)" C_RESET "\n");
    int w = 0;
    for (int i = 0; i < g_n_usage; i++) { int l = (int)strlen(g_usage[i].model); if (l > w) w = l; }
    if (w > 40) w = 40;
    double total = 0; bool priced = false, unknown = false;
    for (int i = 0; i < g_n_usage; i++) {
        const usage_row *u = &g_usage[i];
        char tin[32], tout[32], usd[32]; fmt_tokens(u->in, tin, sizeof tin); fmt_tokens(u->out, tout, sizeof tout);
        double pi = 0, po = 0; int known = model_price(u->model, &pi, &po);
        sb_printf(&o, "  %-*s  %4d call%s  ↑%-6s ↓%-6s  ", w, u->model, u->calls, u->calls == 1 ? " " : "s", tin, tout);
        if (known) {
            int kn = 0; double cost = usage_cost_usd(u->model, u->in, u->out, u->cr, u->cw, &kn);
            total += cost; priced = true;
            fmt_usd(cost, usd, sizeof usd);
            sb_printf(&o, "≈ %s" C_DIM "  ($%g in · $%g out per million, %s)" C_RESET "\n", usd, pi, po, known == 2 ? "set with /usage price" : "list price");
        } else if (provider_find(u->model, NULL)) {
            unknown = true;
            sb_printf(&o, C_YELLOW "no price known" C_RESET C_DIM " — /usage price %s IN OUT" C_RESET "\n", u->model);
        } else if (model_is_cloud(u->model)) sb_printf(&o, C_DIM "an Ollama cloud model: on your ollama.com plan" C_RESET "\n");
        else sb_printf(&o, C_DIM "local: no charge" C_RESET "\n");
        if (u->cr || u->cw) {   /* the cache: its hits are billed at a fraction of the input, its writes above it */
            char tr[32], tw[32]; fmt_tokens(u->cr, tr, sizeof tr); fmt_tokens(u->cw, tw, sizeof tw);
            sb_printf(&o, C_DIM "      ⟳ cache: %s read at %.3g%% of the input price" C_RESET "\n", tr, 100 * model_cache_read_ratio(u->model));
            if (u->cw) sb_printf(&o, C_DIM "      ⟳ cache: %s written at %.4g× the input price" C_RESET "\n", tw, model_cache_write_ratio(u->model));
        }
        usage_limits_line(u->model, &o);
    }
    if (priced) {
        char usd[32]; fmt_usd(total, usd, sizeof usd);
        sb_printf(&o, "  estimated cost  " C_BOLD "≈ %s" C_RESET "%s\n", usd, unknown ? C_DIM "  (without the models that have no price)" C_RESET : "");
        sb_printf(&o, C_DIM "  an estimate from token counts and prices per million tokens — what the provider bills is on its own usage page" C_RESET "\n");
    }
    if (!term_overlay_show(o.data)) fputs(o.data, stdout);
    sb_free(&o);
}

/* ---------- /cost ---------- */
static void fmt_dur(double sec, char *out, size_t n) {
    if (sec < 60) snprintf(out, n, "%.1fs", sec);
    else if (sec < 3600) snprintf(out, n, "%dm %ds", (int)sec / 60, (int)sec % 60);
    else snprintf(out, n, "%dh %dm", (int)sec / 3600, ((int)sec % 3600) / 60);
}
static void cmd_cost(void) {
    char tin[32], tout[32], ttot[32], wall[32], model[32], gen[32];
    fmt_tokens(g_session.prompt_tokens, tin, sizeof tin);
    fmt_tokens(g_session.eval_tokens, tout, sizeof tout);
    fmt_tokens(g_session.prompt_tokens + g_session.eval_tokens, ttot, sizeof ttot);
    fmt_dur(difftime(time(NULL), g_session.started), wall, sizeof wall);
    fmt_dur(g_session.model_seconds, model, sizeof model);
    fmt_dur(g_session.eval_seconds, gen, sizeof gen);
    bool priced; double money = usage_cost_total(&priced);
    printf(C_BOLD "session cost" C_RESET C_DIM " (%s)" C_RESET "\n", priced ? "the local model: tokens and time; the hosted one: money too" : "local model: no money, just tokens and time");
    printf("  tokens        %s  " C_DIM "(↑%s in · ↓%s out)" C_RESET "\n", ttot, tin, tout);
    printf("  model calls   %d  " C_DIM "(%d request%s · %d tool call%s)" C_RESET "\n", g_session.calls, g_session.turns, g_session.turns == 1 ? "" : "s", g_session.tool_calls, g_session.tool_calls == 1 ? "" : "s");
    printf("  model time    %s  " C_DIM "(%s generating", model, gen);
    if (g_session.eval_seconds - g_session.advisor_eval_seconds > 0)   /* the main model's speed: the advisor's is another model's */
        printf(" · %.1f tok/s", (g_session.eval_tokens - g_session.advisor_eval_tokens) / (g_session.eval_seconds - g_session.advisor_eval_seconds));
    if (g_session.think_chunks) { char tk[32], th[32]; fmt_tokens(g_session.think_chunks, tk, sizeof tk); fmt_dur(g_session.think_seconds, th, sizeof th); printf(" · %s thinking ≈%s tok — a lower /effort, /think auto or /think off spends less", th, tk); }
    printf(")" C_RESET "\n");
    if (g_session.advisor_calls || g_session.advisor_tokens) {
        char at[32], ad[32]; fmt_tokens(g_session.advisor_tokens, at, sizeof at); fmt_dur(g_session.advisor_seconds, ad, sizeof ad);
        printf("  advisor       %d consultation%s  " C_DIM "(%s tokens · %s · %s — included in the figures above)" C_RESET "\n", g_session.advisor_calls, g_session.advisor_calls == 1 ? "" : "s", at, ad, g_cfg.advisor ? g_cfg.advisor : "off now");
    }
    if (priced) { char usd[32]; fmt_usd(money, usd, sizeof usd); printf("  est. cost     ≈ %s  " C_DIM "(the hosted models' tokens at their price per million — /usage has it per model)" C_RESET "\n", usd); }
    printf("  wall time     %s\n", wall);
    if (g_cfg.num_ctx > 0 && g_session.last_prompt_tokens > 0)
        printf("  context       %d of %d tokens (%d%%)\n", g_session.last_prompt_tokens, g_cfg.num_ctx, (int)(100.0 * g_session.last_prompt_tokens / g_cfg.num_ctx));
    printf("  model         %s\n", model_set() ? g_cfg.model : "(none)");
}

/* ---------- /diff: what changed in the working tree (not sent to the model) ---------- */
#define DIFF_MAX_LINES 400
static void cmd_diff(const char *arg) {
    if (system("git rev-parse --is-inside-work-tree >/dev/null 2>&1") != 0) { printf(C_DIM "not inside a git repository" C_RESET "\n"); return; }
    sbuf cmd; sb_init(&cmd);
    sb_printf(&cmd, "git --no-pager diff --stat %s 2>&1; echo; git --no-pager diff %s 2>&1", arg && *arg ? arg : "", arg && *arg ? arg : "");
    if (!arg || !*arg) sb_puts(&cmd, "; git status --short --untracked-files=all 2>/dev/null | grep '^?\?' | sed 's/^?? /untracked: /'");
    FILE *f = popen(cmd.data, "r");
    sb_free(&cmd);
    if (!f) { printf(C_RED "✗ cannot run git" C_RESET "\n"); return; }
    char line[4096]; int n = 0, hidden = 0; bool any = false;
    int w = term_width();
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line); if (l && line[l-1] == '\n') line[--l] = 0;
        if (!l && !any) continue;
        any = true;
        if (++n > DIFF_MAX_LINES) { hidden++; continue; }
        const char *col = "";
        if (!strncmp(line, "+++", 3) || !strncmp(line, "---", 3)) col = C_BOLD;
        else if (line[0] == '+') col = C_GREEN;
        else if (line[0] == '-') col = C_RED;
        else if (!strncmp(line, "@@", 2)) col = C_CYAN;
        else if (!strncmp(line, "diff --git", 10)) col = C_BOLD C_YELLOW;
        else if (!strncmp(line, "untracked:", 10)) col = C_MAGENTA;
        /* truncate to the terminal width on a UTF-8 boundary */
        int vis = 0; size_t i = 0; while (line[i] && vis < w - 1) { i++; while (((unsigned char)line[i] & 0xC0) == 0x80) i++; vis++; }
        printf("%s%.*s%s" C_RESET "\n", col, (int)i, line, line[i] ? "…" : "");
    }
    pclose(f);
    if (!any) printf(C_DIM "no changes%s" C_RESET "\n", arg && *arg ? "" : " (working tree clean)");
    if (hidden) printf(C_DIM "… %d more lines (run !git diff %s for everything)" C_RESET "\n", hidden, arg && *arg ? arg : "");
}

/* ---------- /rewind (also Esc Esc at an empty prompt) ----------
 * Pick an earlier request; then restore the files changed since it, truncate the
 * conversation to just before it (the request text comes back into the editor), or both. */
static int cmd_rewind(void) {
    int total = cJSON_GetArraySize(g_messages);
    int idx[256]; int n = 0;
    for (int i = 0; i < total && n < 256; i++) {
        cJSON *m = cJSON_GetArrayItem(g_messages, i);
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
        if (cJSON_IsString(r) && !strcmp(r->valuestring, "user")) idx[n++] = i;
    }
    if (!n) { printf(C_DIM "nothing to rewind: the conversation is empty" C_RESET "\n"); return 0; }
    if (!g_cfg.interactive) { printf(C_DIM "/rewind needs an interactive session" C_RESET "\n"); return 0; }
    const char **items = xmalloc(sizeof(char*) * (size_t)n), **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **bufs = xmalloc(sizeof(char*) * (size_t)n * 2);
    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(g_messages, idx[i]), "content");
        const char *t = cJSON_IsString(c) ? c->valuestring : ""; while (*t == ' ' || *t == '\n') t++;
        size_t l = strcspn(t, "\n"); if (l > 60) { l = 60; while (l && ((unsigned char)t[l] & 0xC0) == 0x80) l--; }
        sbuf a; sb_init(&a); sb_printf(&a, "%d. %.*s%s", i + 1, (int)l, t, t[l] ? "…" : ""); bufs[2*i] = sb_detach(&a); items[i] = bufs[2*i];
        sbuf names; sb_init(&names); sb_append(&names, "", 0);
        int nf = tools_checkpoint_files(idx[i], &names);
        sbuf b; sb_init(&b);
        if (nf) sb_printf(&b, "%d file%s changed since: %.80s%s", nf, nf == 1 ? "" : "s", names.data, strlen(names.data) > 80 ? "…" : ""); else sb_puts(&b, "no file changes since");
        bufs[2*i+1] = sb_detach(&b); descs[i] = bufs[2*i+1]; sb_free(&names);
    }
    int r = term_select("Rewind to before which request?", items, descs, n, n - 1);
    int rc = 0;
    if (r >= 0) {
        int at = idx[r];
        int nf = tools_checkpoint_files(at, NULL);
        const char *what[] = { "Restore conversation and files", "Restore conversation only", "Restore files only" };
        const char *wdesc[3]; char d0[96], d2[96];
        snprintf(d0, sizeof d0, "forget %d message%s and undo %d file change%s", total - at, total - at == 1 ? "" : "s", nf, nf == 1 ? "" : "s");
        snprintf(d2, sizeof d2, "undo %d file change%s, keep the conversation", nf, nf == 1 ? "" : "s");
        wdesc[0] = d0; wdesc[1] = "the request comes back into the editor for you to change"; wdesc[2] = d2;
        int w = term_select(items[r], what, wdesc, 3, 0);
        if (w >= 0) {
            int restored = 0;
            if (w == 0 || w == 2) restored = tools_checkpoint_restore(at);
            if (w == 0 || w == 1) {
                cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(g_messages, at), "content");
                char *text = cJSON_IsString(c) ? xstrdup(c->valuestring) : NULL;
                while (cJSON_GetArraySize(g_messages) > at) cJSON_DeleteItemFromArray(g_messages, cJSON_GetArraySize(g_messages) - 1);
                g_session.last_prompt_tokens = 0;
                if (text && !strchr(text, '<')) term_editor_prefill(text);   /* (skip prompts with attached files/skills) */
                free(text);
                session_save();
            }
            printf(C_GREEN "↩ rewound to before request %d" C_RESET C_DIM " · %s%d file%s restored%s" C_RESET "\n", r + 1,
                   (w == 0 || w == 1) ? "conversation truncated · " : "", restored, restored == 1 ? "" : "s",
                   (w == 0 || w == 1) ? " · the request is back in the editor" : "");
            term_status_refresh();
        } else printf(C_DIM "cancelled" C_RESET "\n");
    } else printf(C_DIM "cancelled" C_RESET "\n");
    for (int i = 0; i < 2 * n; i++) free(bufs[i]);
    free(bufs); free(items); free(descs);
    return rc;
}

/* ---------- /permissions ---------- */
/* /hooks: the project's hooks, as tools.c keeps them (see there for what a line means) */
static void cmd_hooks(const char *arg) {
    if (arg && !strncmp(arg, "add ", 4)) {
        const char *r = arg + 4; while (*r == ' ') r++;
        const char *sp = strchr(r, ' ');
        size_t el = sp ? (size_t)(sp - r) : strlen(r);
        bool ev = (el == 3 && !strncmp(r, "pre", 3)) || (el == 4 && !strncmp(r, "post", 4));
        bool ok = ev ? (sp && *(sp + 1) && strchr(sp + 1, ' ') && *(strchr(sp + 1, ' ') + 1))
                     : ((el == 5 && !strncmp(r, "start", 5)) || (el == 4 && !strncmp(r, "stop", 4))) && sp && *(sp + 1);
        if (!ok) { printf("usage: /hooks add pre TOOL COMMAND | post TOOL COMMAND | start COMMAND | stop COMMAND\n"); return; }
        printf(tools_hooks_add(r) ? C_GREEN "✓ hook added: %s" C_RESET "\n" : C_DIM "already there: %s" C_RESET "\n", r);
        return;
    }
    if (arg && !strncmp(arg, "remove ", 7)) {
        int i = atoi(arg + 7);
        if (tools_hooks_remove(i - 1)) printf(C_GREEN "✓ removed hook %d" C_RESET "\n", i); else printf(C_RED "no hook %d" C_RESET "\n", i);
        return;
    }
    if (arg && !strcmp(arg, "clear")) { tools_hooks_clear(); printf(C_GREEN "✓ project hooks cleared" C_RESET "\n"); return; }
    if (arg && *arg) { printf("usage: /hooks [add pre|post TOOL COMMAND | add start|stop COMMAND | remove N | clear]\n"); return; }
    int n = tools_hooks_count();
    printf(C_BOLD "project hooks" C_RESET C_DIM " (kept in %s/hooks — they run shell commands, so only what you add here runs)" C_RESET "\n", config_dir());
    if (!n) printf(C_DIM "  none — e.g. /hooks add pre bash echo \"use make\" >&2; exit 2 (exit 2 stops the call)" C_RESET "\n");
    for (int i = 0; i < n; i++) printf("  %2d. %s\n", i + 1, tools_hooks_get(i));
    if (n) printf(C_DIM "  /hooks remove N · /hooks clear" C_RESET "\n");
}

static void cmd_permissions(const char *arg) {
    if (arg && !strncmp(arg, "add ", 4)) {
        const char *r = arg + 4; while (*r == ' ') r++;
        bool guard = !strncmp(r, "deny ", 5) || !strncmp(r, "ask ", 4);   /* deny|ask bash|edit|fetch <pattern> */
        if (guard) {
            const char *g = strchr(r, ' ') + 1; while (*g == ' ') g++;
            const char *sp = strchr(g, ' ');
            bool known = (!strncmp(g, "bash ", 5) || !strncmp(g, "edit ", 5) || !strncmp(g, "fetch ", 6)) && sp && sp[1];
            if (!known) { printf("usage: /permissions add deny|ask bash|edit|fetch <pattern>   (* matches any text, ? one character)\n"); return; }
        } else if (strcmp(r, "edit") && strncmp(r, "bash ", 5) && strncmp(r, "fetch ", 6)) {
            printf("usage: /permissions add edit | add bash <leading words> | add fetch <host> | add deny|ask bash|edit|fetch <pattern>\n"); return;
        }
        printf(tools_permissions_add(r) ? C_GREEN "✓ added: %s" C_RESET "\n" : C_DIM "already there: %s" C_RESET "\n", r);
        return;
    }
    if (arg && !strncmp(arg, "remove ", 7)) {
        int i = atoi(arg + 7);
        if (tools_permissions_remove(i - 1)) printf(C_GREEN "✓ removed rule %d" C_RESET "\n", i); else printf(C_RED "no rule %d" C_RESET "\n", i);
        return;
    }
    if (arg && !strcmp(arg, "clear")) { tools_permissions_clear(); printf(C_GREEN "✓ project permissions cleared" C_RESET "\n"); return; }
    if (arg && *arg) { printf("usage: /permissions [add edit|add bash <words>|add fetch <host>|add deny|ask bash|edit|fetch <pattern>|remove N|clear]\n"); return; }
    int n = tools_permissions_count();
    printf(C_BOLD "project permissions" C_RESET C_DIM " (kept in %s/permissions — answers of \"always allow … in this project\")" C_RESET "\n", config_dir());
    if (!n) printf(C_DIM "  none — pick \"always allow … in this project\" (p) at a confirmation, or /permissions add bash git status" C_RESET "\n");
    for (int i = 0; i < n; i++) {
        const char *r = tools_permissions_get(i);
        if (!strcmp(r, "edit")) printf("  %2d. file writes and edits\n", i + 1);
        else if (!strncmp(r, "fetch ", 6)) printf("  %2d. web pages from " C_BOLD "%s" C_RESET "\n", i + 1, r + 6);
        else if (!strncmp(r, "deny ", 5) || !strncmp(r, "ask ", 4)) {   /* a guard: "deny|ask <tool> <pattern>" */
            bool deny = !strncmp(r, "deny ", 5);
            const char *t = r + (deny ? 5 : 4), *sp = strchr(t, ' ');
            if (sp) printf("  %2d. %s %.*s matching " C_BOLD "%s" C_RESET "\n", i + 1, deny ? C_RED "deny" C_RESET : C_ORANGE "ask" C_RESET, (int)(sp - t), t, sp + 1);
            else printf("  %2d. %s\n", i + 1, r);
        }
        else printf("  %2d. shell commands starting with " C_BOLD "%s" C_RESET "\n", i + 1, r + 5);
    }
    if (n) printf(C_DIM "  /permissions remove N · /permissions clear" C_RESET "\n");
}

/* ---------- system prompt ---------- */
static void load_project_instructions(void) {
    const char *names[] = { "AGENTS.md", "CORBIENEST.md", "CLAUDE.md", NULL };
    free(g_project_instructions); g_project_instructions = NULL;
    for (int i = 0; names[i]; i++) {
        if (is_file(names[i])) {
            size_t n; char *d = read_whole_file(names[i], &n, 32 * 1024);
            if (d) {
                sbuf b; sb_init(&b);
                sb_printf(&b, "\n\n# Project instructions (from %s)\n%s", names[i], d);
                g_project_instructions = sb_detach(&b);
                free(d);
            }
            break;
        }
    }
}

static char *build_system_prompt(void) {
    sbuf b; sb_init(&b);
    struct utsname u; uname(&u);
    time_t now = time(NULL); char date[64]; strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    bool git = is_dir(".git");
    sb_puts(&b,
        "You are Corbie Nest (corbienest), an expert software engineering assistant running as a coding agent inside the user's terminal. "
        "You help with programming tasks: exploring and understanding codebases, writing new code, editing existing code, "
        "running shell and git commands, building, testing and debugging.\n\n");
    sb_printf(&b, "# Environment\n- Working directory: %s\n- OS: %s %s\n- Shell: /bin/sh\n- Date: %s\n- Git repository: %s\n",
              g_cwd, u.sysname, u.machine, date, git ? "yes" : "no");
    if (g_model_tools && !g_cfg.no_tools) {
        sb_printf(&b,
            "\n# Tools\nYou have these tools: %s. Use them proactively "
            "instead of asking the user to paste files or run commands for you.\n", tools_summary_line());
        sb_puts(&b,
            "- Explore first: use list_dir / grep / read_file to understand relevant code before changing it.\n"
            "- Batch independent calls: when you need several reads, searches or fetches that do not depend on each other's results, request them all in one reply rather than one per turn.\n"
            "- Always read a file before editing it. Use edit_file for targeted changes (old_string must match exactly); "
            "use write_file only for new files or full rewrites.\n"
            "- Use bash for shell work: git (status, diff, log, add, commit, branch...), building, running tests, "
            "package managers, find, etc. Commands run non-interactively with /bin/sh in the working directory. "
            "Never run interactive programs (editors, pagers, prompts).\n"
            "- The user must approve each write, edit, shell command and page fetch, so proceed and call the tool rather than asking permission in text.\n"
            "- After making code changes, verify them when practical (compile, run tests, or run the program).\n"
            "- Do not fabricate tool results. If a tool errors, read the error and adjust.\n");
        if (g_cfg.web) sb_puts(&b,
            "- Never guess at another project's API, options or errors: web_search for its docs, web_fetch to read "
            "them — the version this project uses (check its lockfile, manifest or image tag). Say which page you "
            "used; skip the web when the repo answers it.\n");
        sb_puts(&b,
            "- When the user says they are finished and want to leave (\"exit\", \"bye\", \"that's all\"), reply with a short goodbye and call exit in the same turn; the program ends after that reply. Do not call exit otherwise.\n"
            "- For work of three or more steps, keep a task list with todo_write: one task per step, at most one in_progress, and mark each completed as you finish it. Skip it for a single change or a question.\n"
            "- Use task to hand a broad, self-contained investigation (\"find all places that…\", \"how does X work across the code\") to a read-only sub-agent "
            "and get back a report, keeping the noise out of this conversation; give it a complete prompt, it knows nothing of this chat.\n");
        if (g_cfg.advisor) {   /* no model name in it: changing the advisor leaves the prompt, and its cache, alone (the guidance does not) */
            const advisor_guidance_def *g = advisor_guidance();
            sb_puts(&b, "- advisor is a stronger but much slower model that is shown this whole conversation and tells you how to proceed. ");
            if (g_cfg.advisor_guidance == GUIDANCE_LIGHT)
                sb_puts(&b, "Consult it only when you are stuck: when the same error has survived two fixes or a result makes no sense. ");
            else if (g_cfg.advisor_guidance == GUIDANCE_NORMAL)
                sb_puts(&b, "Use it — a wrong turn costs more than a question. Consult it "
                    "before you commit to an approach for a non-trivial change (explore first, then ask — put your plan in the question — then edit), as soon as a fix of yours has not worked "
                    "or a result makes no sense, and before you call a difficult task done (after the build or the tests have run, so it sees their output). "
                    "It is brought in by itself when your tool calls keep failing. ");
            else
                sb_printf(&b, "Lean on it — it is there to be used, and a wrong turn costs more than a question: once you have read the code involved, ask before you commit to an approach "
                    "(put your plan in the question), again before each non-trivial change, and as soon as a build, a test or a command fails in a way you do not understand at once — "
                    "do not try a second fix on a guess. It reviews your work by itself before a request ends in which you changed a lot (many lines or several files; at the top level, any change), "
                    "and it is brought in when your tool calls keep failing%s. ",
                    g->check_first_edit ? "; it also checks the first change of a request before it is made" : "");
            if (g->uses == 1) sb_puts(&b, "Not for what a tool call can tell you, and at most once per request");
            else sb_printf(&b, "Not for what a tool call can tell you, and at most %d times per request", g->uses);
            if (g_cfg.advisor_window > 0) sb_printf(&b, " (in a request that runs long the count starts again every %s)", fmt_window());
            sb_puts(&b, ". ");
            sb_puts(&b, "Weigh its advice seriously, but it cannot look at anything itself: "
                "where a file or a command's output contradicts it, they are right — say so and carry on.\n");
        }
    } else {
        sb_puts(&b, "\n(No tools are available in this session; if you need file contents or command output, ask the user to provide them.)\n");
    }
    sb_puts(&b,
        "\n# Style\n- Be concise and direct. Do not pad answers with pleasantries.\n"
        "- Use markdown lightly (code blocks for code, short lists). Reference code as path:line.\n"
        "- When writing code, follow the existing conventions of the project and write complete, working code.\n");
    sb_puts(&b, "\n# Options\nWhen there are two or more reasonable ways to do the work and the user should choose between them, end your reply "
                "with one fenced block named options: one line per option, the one you recommend marked [recommended], and after a dash what it does and what it costs:\n"
                "```options\n- [recommended] Advisory lock — survives a crash, needs flock\n- Lock file — simple, but a crash leaves it behind\n```\n"
                "Only for a real choice between approaches; for anything you can decide yourself, just do it. The user picks one with the arrow keys, and the choice comes back as their next message.\n");
    if (g_model_tools && !g_cfg.no_tools && g_cfg.mode != MODE_PLAN)
        sb_puts(&b,
            "\n# Finishing a task\nA report closes work you have finished; it is not how every turn ends. Before you "
            "write one, read the user's request again and check that every part of it is carried out. If anything is "
            "left — a step you skipped, a tool the user denied, a build or test that failed, work you stopped partway — "
            "then you are not finished: write no report, and say in a line or two what is done, what is not, and what "
            "comes next.\n"
            "When it is done, end your turn with a short report:\n"
            "- What was done: a few bullets — the changes you made, the files they touched (path:line), and how you "
            "verified them (build, tests, running the program) or that you did not. Never claim a check you did not run.\n"
            "- Suggestions: up to three bullets on what the user may want to do next — follow-up work you left out on "
            "purpose, a weakness you noticed nearby, something worth testing. One line each, saying what and why. "
            "Do not start on them, they are the user's to decide; leave the whole section out rather than inventing work.\n"
            "Keep the report under about ten lines. There is no report after a turn that only answered a question, or "
            "one that stops to ask the user something rather than finishing.\n");
    if (g_cfg.mode == MODE_PLAN)
        sb_puts(&b, "\n# Plan mode (read-only)\n"
            "The user has put you in plan mode. Explore the codebase with read_file, list_dir, grep and read-only shell commands, "
            "then present a concrete implementation plan as text: which files to change and how, the order of steps, and any risks or open questions. "
            "Do NOT modify files in this mode — write_file and edit_file are unavailable. When the plan is approved the user will switch modes and ask you to implement it.\n");
    if (g_model_tools && !g_cfg.no_tools) { char *sk = skills_prompt_section(); if (sk) { sb_puts(&b, sk); free(sk); } }
    if (g_cfg.system_prompt && *g_cfg.system_prompt) sb_printf(&b, "\n# Additional instructions from the user\n%s\n", g_cfg.system_prompt);
    if (g_project_instructions) sb_puts(&b, g_project_instructions);
    if (g_memory) sb_printf(&b, "\n\n# Project memory (from " MEMORY_PATH ")\n"
        "Facts remembered from earlier sessions. Verify anything that names a file, function or flag before relying on it. "
        "You may edit this file with edit_file when the user asks you to remember or forget something; it is also refreshed automatically after each request.\n\n%s", g_memory);
    return sb_detach(&b);
}

/* messages array with system prompt prepended (references, so cheap) */
static cJSON *messages_with_system(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    char *sp = build_system_prompt();
    cJSON_AddStringToObject(sys, "content", sp);
    g_sys_bytes = strlen(sp);
    free(sp);
    cJSON_AddItemToArray(arr, sys);
    cJSON *m; cJSON_ArrayForEach(m, g_messages) cJSON_AddItemReferenceToArray(arr, m);
    return arr;
}

static void add_message(const char *role, const char *content) {
    char *clean = utf8_fix(content);   /* a pasted or @mentioned file may not be UTF-8 */
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", clean);
    free(clean);
    cJSON_AddItemToArray(g_messages, m);
}

/* A hosted model's num_ctx is only the local budget (what is kept and sent). It is not a
 * parameter of the API. Clamp in memory when it is unset or past the model's window; a later
 * save is what persists it. */
static void clamp_ctx_budget(void) {
    if (model_hosted() && g_model_max_ctx > 0 && (g_cfg.num_ctx == 0 || g_cfg.num_ctx > g_model_max_ctx))
        g_cfg.num_ctx = g_model_max_ctx;
}

/* ---------- model info ---------- */
#define MODEL_LIST_WAIT_MS 3000   /* how long a list of models is waited for (the suggestions, the start-up check) */
static void refresh_model_caps(bool quiet) {
    g_model_bad = false; g_model_err[0] = 0;
    if (g_cfg.model && !*g_cfg.model) return;   /* no model yet: nothing to look up */
    const provider_def *hosted = g_cfg.model ? provider_find(g_cfg.model, NULL) : NULL;
    if (hosted && hosted->style != PROVIDER_MESSAGES) {
        g_model_bad = true;
        snprintf(g_model_err, sizeof g_model_err, "%s is an advisor, not the model that does the work — use /advisor %s", hosted->label, g_cfg.model);
        if (!quiet) printf(C_RED "✗ %s" C_RESET "\n", g_model_err);
        return;
    }
    if (hosted) {
        int rc = provider_model_info(g_cfg.model, &g_model_info, g_model_err, sizeof g_model_err);
        g_model_info.tools = true;   /* the endpoint does not say; a Messages model does the work with the same tools */
        g_model_tools = true;
        g_model_max_ctx = g_model_info.context_length;
        g_placement_checked = true;   /* nothing is loaded here for /api/ps to place */
        if (rc < 0) { g_model_bad = true; if (!quiet) printf(C_RED "✗ %s" C_RESET "\n", g_model_err); return; }
        if (rc > 0 && !quiet) printf(C_YELLOW "⚠ %s — using what this provider's models generally take" C_RESET "\n", g_model_err);
        clamp_ctx_budget();
        const char *saved = effort_get(g_cfg.model);
        if (saved && !quiet && !effort_resolve(&g_model_info, saved, NULL))
            printf(C_YELLOW "⚠ effort '%s' is saved for %s, which does not offer it (any more) — it is left to the model; /effort to pick one" C_RESET "\n", saved, g_cfg.model);
        return;
    }
    cJSON *list = quiet ? ollama_list_models_quiet(MODEL_LIST_WAIT_MS) : ollama_list_models();
    if (!list) return;
    bool found = false;
    cJSON *m;
    if (!g_cfg.model) {   /* pick a default: first tool-capable model */
        cJSON_ArrayForEach(m, list) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools"))) { g_cfg.model = xstrdup(cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring); break; }
        }
        if (!g_cfg.model && cJSON_GetArraySize(list) > 0)
            g_cfg.model = xstrdup(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, 0), "name")->valuestring);
    }
    if (!g_cfg.model) { cJSON_Delete(list); return; }
    bool tag_tools = false, tag_think = false;
    cJSON_ArrayForEach(m, list) {
        const char *n = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring;
        char withtag[512]; snprintf(withtag, sizeof withtag, "%s:latest", g_cfg.model);
        if (!strcmp(n, g_cfg.model) || !strcmp(n, withtag)) {
            found = true;
            tag_tools = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools"));
            tag_think = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "thinking"));
            break;
        }
    }
    cJSON_Delete(list);
    /* /api/show knows the model as it is now — and knows cloud models, which /api/tags does not
     * list; the capabilities in /api/tags are only the fallback for a server too old to send them */
    bool shown = ollama_model_show(g_cfg.model, &g_model_info) == 0;
    if (!g_model_info.caps_known) { g_model_info.tools = found ? tag_tools : true; g_model_info.thinking = found && tag_think; model_think_profile(&g_model_info); }
    g_model_tools = g_model_info.tools;
    g_model_max_ctx = g_model_info.context_length;
    g_placement_checked = false;
    if (!found && !shown) {
        g_model_tools = true;   /* unknown (maybe remote/cloud); try */
        if (!quiet) printf(C_YELLOW "⚠ model '%s' not found locally — ollama may need to pull it (see /models)" C_RESET "\n", g_cfg.model);
    } else if (!g_model_tools && !quiet) {
        printf(C_YELLOW "⚠ model '%s' does not support tool calling; running in chat-only mode (no file/shell tools)" C_RESET "\n", g_cfg.model);
    }
    const char *saved = effort_get(g_cfg.model);
    if (saved && !quiet && !effort_resolve(&g_model_info, saved, NULL))
        printf(C_YELLOW "⚠ effort '%s' is saved for %s, which does not offer it (any more) — it is left to the model; /effort to pick one" C_RESET "\n", saved, g_cfg.model);
}

/* ---------- context window ---------- */
/* "32768" -> "32k", "131072" -> "128k", "1000000" -> "1M" (static buffer, 4 rotating) */
static const char *fmt_ctx(int n) {
    static char bufs[4][32]; static int bi = 0;
    char *b = bufs[bi++ & 3];
    if (n <= 0) snprintf(b, 32, "default");
    else if (n % 1024 == 0 && n < 1024 * 1024) snprintf(b, 32, "%dk", n / 1024);
    else if (n % (1024 * 1024) == 0) snprintf(b, 32, "%dM", n / (1024 * 1024));
    else if (n >= 1000000 && n % 1000000 == 0) snprintf(b, 32, "%dM", n / 1000000);
    else snprintf(b, 32, "%d", n);
    return b;
}

/* Parses a context size: "65536", "64k", "512kb", "128K", "1m", "1MB", "max" (the model's trained
 * length), "default"/"0" (leave to the server). Returns -1 if unusable. */
static int parse_ctx_arg(const char *a) {
    if (!a) return -1;
    while (*a == ' ') a++;
    if (!strcasecmp(a, "default") || !strcmp(a, "0")) return 0;
    if (!strcasecmp(a, "max")) return g_model_max_ctx > 0 ? g_model_max_ctx : -1;
    char *end; double v = strtod(a, &end);
    if (end == a || v <= 0) return -1;
    while (*end == ' ') end++;
    if (*end == 'k' || *end == 'K') { v *= 1024; end++; if (*end == 'b' || *end == 'B') end++; }
    else if (*end == 'm' || *end == 'M') { v *= 1024 * 1024; end++; if (*end == 'b' || *end == 'B') end++; }
    if (*end) return -1;
    if (v < 512 || v > 64.0 * 1024 * 1024) return -1;
    return (int)v;
}

static void set_ctx(int n) {
    if (model_hosted() && g_model_max_ctx > 0 && (n == 0 || n > g_model_max_ctx)) {
        if (n > g_model_max_ctx) printf(C_YELLOW "clamped to %s, the model's window" C_RESET "\n", fmt_ctx(g_model_max_ctx));
        n = g_model_max_ctx;
    }
    if (g_while_busy) {
        g_pending_ctx = n;
        printf(C_GREEN "✓ %s: %s" C_RESET C_DIM " — from the next request" C_RESET "\n", model_hosted() ? "context budget" : "context window", n ? fmt_ctx(n) : "server default");
        return;
    }
    g_cfg.num_ctx = n; config_save();
    if (model_hosted()) {
        printf(C_GREEN "✓ context budget: %s" C_RESET "\n" C_DIM "  what is kept and sent on each call; the API has no num_ctx, and this is not sent to it" C_RESET "\n", fmt_ctx(n));
        term_status_refresh();
        return;
    }
    if (n > 0) {
        printf(C_GREEN "✓ context window: %s (num_ctx %d)" C_RESET, fmt_ctx(n), n);
        if (g_model_max_ctx > 0 && n > g_model_max_ctx) printf(C_YELLOW " — larger than the model's trained length (%s); quality may degrade" C_RESET, fmt_ctx(g_model_max_ctx));
        printf("\n" C_DIM "  takes effect on the next request; larger windows need more RAM/VRAM and reload the model" C_RESET "\n");
    } else printf(C_GREEN "✓ context window: server default" C_RESET "\n");
    term_status_refresh();
}

/* /ctx: no argument opens a picker of sizes up to the model's trained maximum;
 * /ctx N|Nk|max|default sets it directly. */
/* The context sizes worth offering for the current model: the standard powers of two up to
 * the model's trained maximum (or, with `all`, up to 1M whatever the model was trained for: a
 * local model takes any num_ctx), the maximum itself, and `extra` (the current setting; 0 = none),
 * ascending. Returns the count. */
static int ctx_size_list(int *sizes, int cap, int extra, bool all) {
    static const int STD[] = { 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576 };
    int n = 0;
    for (size_t i = 0; i < sizeof STD / sizeof *STD && n < cap; i++)
        if (all || !g_model_max_ctx || STD[i] <= g_model_max_ctx) sizes[n++] = STD[i];
    int add[2] = { g_model_max_ctx, extra };
    for (int k = 0; k < 2; k++) {
        if (add[k] <= 0 || n >= cap) continue;
        bool has = false; for (int i = 0; i < n; i++) if (sizes[i] == add[k]) has = true;
        if (!has) sizes[n++] = add[k];
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && sizes[j-1] > sizes[j]; j--) { int t = sizes[j]; sizes[j] = sizes[j-1]; sizes[j-1] = t; }
    return n;
}

static void cmd_ctx(const char *arg) {
    if (arg) {
        int n = parse_ctx_arg(arg);
        if (n < 0) {
            if (!strcasecmp(arg, "max")) printf(C_YELLOW "the model's maximum context length is unknown; give a size, e.g. /ctx 64k" C_RESET "\n");
            else printf(C_RED "bad size '%s'" C_RESET " — use e.g. /ctx 32768, /ctx 64k, /ctx max, /ctx default\n", arg);
            return;
        }
        set_ctx(n);
        return;
    }
    if (!g_cfg.interactive) {
        printf("num_ctx: %d", g_cfg.num_ctx);
        if (g_model_max_ctx > 0) printf(" (model max %d)", g_model_max_ctx);
        printf("\n");
        return;
    }
    int sizes[16], n = ctx_size_list(sizes, 14, g_cfg.num_ctx, !model_hosted());
    sizes[n++] = 0;   /* server default, last */
    const char **items = xmalloc(sizeof(char*) * (size_t)n), **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **own = xmalloc(sizeof(char*) * (size_t)n * 2);
    int current = -1;
    for (int i = 0; i < n; i++) {
        sbuf a; sb_init(&a); sbuf d; sb_init(&d);
        if (sizes[i] == 0) { sb_puts(&a, model_hosted() ? "model window" : "server default"); sb_puts(&d, model_hosted() ? "keep and send up to the model's own window" : "let ollama decide (usually 4k)"); }
        else {
            sb_printf(&a, "%s", fmt_ctx(sizes[i]));
            sb_printf(&d, "%d tokens", sizes[i]);
            if (sizes[i] == g_model_max_ctx) sb_puts(&d, " · model maximum");
            else if (g_model_max_ctx > 0 && sizes[i] > g_model_max_ctx) sb_puts(&d, " · beyond the model's trained length");
        }
        if (sizes[i] == g_cfg.num_ctx) { sb_puts(&d, " · current"); current = i; }
        own[2*i] = sb_detach(&a); own[2*i+1] = sb_detach(&d);
        items[i] = own[2*i]; descs[i] = own[2*i+1];
    }
    char title[128];
    if (g_model_max_ctx > 0) snprintf(title, sizeof title, "Context window (%s supports up to %s)", g_cfg.model, fmt_ctx(g_model_max_ctx));
    else snprintf(title, sizeof title, "Context window (num_ctx)");
    int r = term_select(title, items, descs, n, current);
    if (r >= 0 && sizes[r] != g_cfg.num_ctx) set_ctx(sizes[r]);
    else printf(C_DIM "context window unchanged: %s" C_RESET "\n", fmt_ctx(g_cfg.num_ctx));
    for (int i = 0; i < 2 * n; i++) free(own[i]);
    free(own); free(items); free(descs);
}

/* ---------- output helpers ---------- */
static void print_result_preview(const char *text, int max_lines) {
    int lines = 0, total = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') total++;
    if (*text && text[strlen(text)-1] != '\n') total++;
    const char *p = text;
    bool first = true;
    while (*p && lines < max_lines) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int w = term_width() - 6; if (w < 20) w = 20;   /* "  ⎿  " prefix + ellipsis */
        size_t cut = n > (size_t)w ? (size_t)w : n;
        while (cut > 0 && cut < n && ((unsigned char)p[cut] & 0xC0) == 0x80) cut--;   /* don't split UTF-8 */
        printf("  %s" C_DIM "%.*s%s" C_RESET "\n", first ? "⎿  " : "   ", (int)cut, p, cut < n ? "…" : "");
        first = false; lines++;
        if (!e) break;
        p = e + 1;
    }
    if (total > lines) printf("     " C_DIM "… +%d lines" C_RESET "\n", total - lines);
    if (!*text) printf("  ⎿  " C_DIM "(empty)" C_RESET "\n");
}

/* A tool's result in the transcript. One line is shown as it is; more than that is folded away
 * under a line that says how much there is and how it begins — what grep, cat and a build print
 * is for the model, and the user asked for it out of the way — and a click on that line shows
 * it (term_fold_begin). Where nothing can be clicked, the first lines are shown as before. */
#define FOLD_MAX_LINES 400
/* the body of an open fold: `total` lines of plain text, at most FOLD_MAX_LINES of them */
static void fold_body(const char *text, int total) {
    int lines = 0;
    for (const char *p = text; *p && lines < FOLD_MAX_LINES; lines++) {
        size_t len = strcspn(p, "\n");
        fputs("     " C_DIM, stdout);
        for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)p[i]; if (c == '\t') fputs("    ", stdout); else if (c >= 32 && c != 127) fputc(c, stdout); }   /* no escapes, no carriage returns: they would move the model's cursor */
        fputs(C_RESET "\n", stdout);
        p += len; if (*p == '\n') p++;
    }
    if (total > lines) printf("     " C_DIM "… +%d lines" C_RESET "\n", total - lines);
}
static int count_lines(const char *text) {
    int total = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') total++;
    if (*text && text[strlen(text) - 1] != '\n') total++;
    return total;
}
/* columns a string takes on the screen: its escapes do not count */
static int plain_width(const char *s) {
    int w = 0;
    for (const char *p = s; *p; p++) {
        if (*p == 27) { while (*p && *p != 'm') p++; if (!*p) break; continue; }
        if (((unsigned char)*p & 0xC0) != 0x80) w++;
    }
    return w;
}
/* `lead` is what the head starts with (NULL: the usual "  ⎿  "); a bash call's result line passes its
 * header, so the call is one line with its output's fold on it */
/* A task list is shown whole, not folded: it is the state the user follows. */
static void print_task_lines(const char *text) {
    const char *p = text ? text : "";
    bool first = true;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        printf("%s%.*s" C_RESET "\n", first ? "  ⎿  " : "     ", (int)l, p);
        first = false;
        if (!nl) break;
        p = nl + 1;
    }
}
static void print_result_folded(const char *text, int preview_lines, const char *lead) {
    int total = count_lines(text);
    bool plain = !lead;
    if (plain) lead = "  ⎿  ";
    if (total < 2 && plain) { print_result_preview(text, preview_lines); return; }
    size_t n = strcspn(text, "\n");
    char count[48]; snprintf(count, sizeof count, TERM_FOLD_CLOSED " %d lines · ", total);
    int w = term_width() - plain_width(lead) - 1 - (int)strlen(count); if (w < 12) w = 12;   /* the ellipsis */
    size_t cut = n > (size_t)w ? (size_t)w : n;
    while (cut > 0 && cut < n && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;   /* don't split UTF-8 */
    sbuf head; sb_init(&head);
    sb_printf(&head, "%s" C_DIM "%s", lead, count);
    for (size_t i = 0; i < cut; i++) sb_putc(&head, (unsigned char)text[i] < 32 || text[i] == 127 ? ' ' : text[i]);
    sb_printf(&head, "%s" C_RESET, cut < n ? "…" : "");
    bool folded = term_fold_begin(head.data);
    sb_free(&head);
    if (!folded) { if (!plain) printf("%s\n", lead); print_result_preview(text, preview_lines); return; }
    fold_body(text, total);
    term_fold_end();
}

#define AUTO_COMPACT_PCT 85   /* auto-compact once the last request used this much of num_ctx */
#define COMPACT_REQUEST_MAX 4000  /* bytes of the pending request kept when compacting mid-turn */
#define COMPACT_HINT_PCT 70   /* from here on the stats line suggests /compact */
static void print_stats(chat_stats *st) {
    if (!st->eval_tokens && !st->prompt_tokens) return;
    double tps = st->eval_seconds > 0 ? st->eval_tokens / st->eval_seconds : 0;
    printf(C_GRAY "  ↳ %d in · %d out · %.1f tok/s", st->prompt_tokens, st->eval_tokens, tps);
    if (st->prompt_seconds >= 1.0) printf(" · prefill %.1fs", st->prompt_seconds);   /* long = the prompt cache missed / cold model */
    if (st->think_chunks) { char tk[32]; fmt_tokens(st->think_chunks, tk, sizeof tk); printf(" · thought %.0fs (≈%s tok)", st->think_seconds, tk); }
    printf(" · %.1fs", st->total_seconds);
    if (g_cfg.num_ctx > 0) {
        int pct = (int)(100.0 * st->prompt_tokens / g_cfg.num_ctx);
        printf(" · ctx %d%%", pct);
        if (pct >= COMPACT_HINT_PCT && pct < AUTO_COMPACT_PCT) {   /* at AUTO_COMPACT_PCT it compacts by itself */
            if (g_model_max_ctx > g_cfg.num_ctx) printf(C_YELLOW " (near limit — /compact, or /ctx to enlarge: model supports %s)", fmt_ctx(g_model_max_ctx));
            else printf(C_YELLOW " (near limit — consider /compact)");
        }
    }
    printf(C_RESET "\n");
}

/* Once per model: ask /api/ps where it was loaded and warn when part of it sits in system
 * memory — the usual reason a local model crawls (too large a num_ctx or model for the GPU).
 * `verbose` also reports a fully-GPU model (for /status). */
static void check_model_placement(bool verbose) {
    if (model_hosted()) return;   /* a hosted model is not loaded here */
    if (!g_cfg.interactive || (g_placement_checked && !verbose)) return;
    /* a request is in flight and ollama.c's streaming callbacks are global: no second HTTP call */
    if (g_while_busy) { if (verbose) printf(C_BOLD "placement  " C_RESET C_DIM "(not checked while the model is working)" C_RESET "\n"); return; }
    g_placement_checked = true;
    double size, vram;
    if (ollama_model_placement(g_cfg.model, &size, &vram) != 0 || size <= 0) { if (verbose) printf(C_BOLD "placement  " C_RESET "(model not loaded)\n"); return; }
    if (vram >= size) { if (verbose) printf(C_BOLD "placement  " C_RESET "100%% in GPU memory (%.1f GB)\n", size / 1e9); return; }
    if (verbose) printf(C_BOLD "placement  " C_RESET);
    printf(C_YELLOW "⚠ model is only %d%% in GPU memory (%.1f of %.1f GB) — generation will be slow; try a smaller /ctx or a smaller model" C_RESET "\n",
           (int)(100.0 * vram / size + 0.5), vram / 1e9, size / 1e9);
}

/* short one-line summary of a tool call's main argument */
static void tool_arg_summary(const char *name, cJSON *args, char *out, size_t n) {
    const char *keys[] = { !strcmp(name, "grep") ? "pattern" : "command", "url", "query", "path", "pattern", "question", NULL };
    const char *v = NULL;
    for (int i = 0; keys[i] && !v; i++) {
        cJSON *a = cJSON_GetObjectItemCaseSensitive(args, keys[i]);
        if (cJSON_IsString(a)) v = a->valuestring;
    }
    /* fit "● name(summary)" on one terminal line */
    int room = term_width() - (int)strlen(name) - 6; if (room < 20) room = 20;
    if ((size_t)room >= n) room = (int)n - 1;
    if (v) {
        snprintf(out, n, "%.*s", room, v);
        char *nl = strchr(out, '\n'); if (nl) *nl = 0;
        if (nl && (int)strlen(out) < room && strlen(out) + 4 <= n) strcat(out, "…");   /* more lines follow: the header shows the first one only, within room */
        size_t vl = strcspn(v, "\n");
        if (vl > (size_t)room) {   /* truncated: back off to a UTF-8 boundary and add an ellipsis */
            size_t cut = (size_t)room - 1;
            while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
            snprintf(out + cut, n - cut, "…");
        }
    }
    else out[0] = 0;
    if (!strcmp(name, "grep") && v) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
        if (cJSON_IsString(p)) { size_t l = strlen(out); snprintf(out + l, n - l, " in %s", p->valuestring); }
    }
}

/* ---------- a tool result becomes a message ----------
 * The tools cap their own output (64 KB for read_file, 32 KB for bash), but a cap in bytes is
 * meaningless next to a small window: at num_ctx 8k a single full file read is the whole
 * context, and since Ollama trims an over-long prompt from the front, the result would push the
 * request that asked for it out of the prompt. So no one result may take more than
 * TOOL_RESULT_PCT of the window; the middle goes, both ends stay, and the note tells the model
 * how to get the rest. */
#define TOOL_RESULT_PCT 25
/* The result a tool call gets when it did not run because the request was interrupted (Esc or
 * Ctrl-C): the model has to know the call did not happen. */
static const char *TOOL_NOT_RUN =
    "not run: the user interrupted the work before this tool call started. The user's next message "
    "follows; call this tool again if it is still needed.";

static const char *tool_call_id(cJSON *call) {
    cJSON *id = call ? cJSON_GetObjectItemCaseSensitive(call, "id") : NULL;
    return cJSON_IsString(id) && id->valuestring[0] ? id->valuestring : NULL;
}

static cJSON *tool_result_build(const char *name, const char *id, const char *res) {
    cJSON *tm = cJSON_CreateObject();
    cJSON_AddStringToObject(tm, "role", "tool");
    cJSON_AddStringToObject(tm, "tool_name", name);
    if (id && *id) cJSON_AddStringToObject(tm, "tool_call_id", id);   /* the Messages API matches a result to its call by this */
    size_t len = strlen(res);
    size_t cap = g_cfg.num_ctx > 0 ? (size_t)(g_cfg.num_ctx * (TOOL_RESULT_PCT / 100.0) * g_bytes_per_token) : 0;
    if (!cap || len <= cap) { cJSON_AddStringToObject(tm, "content", res); return tm; }
    size_t head = cap * 2 / 3, tail = cap - head;
    while (head && ((unsigned char)res[head] & 0xC0) == 0x80) head--;            /* UTF-8 boundaries */
    const char *t = res + len - tail;
    while (tail && ((unsigned char)*t & 0xC0) == 0x80) { t++; tail--; }
    sbuf b; sb_init(&b);
    sb_append(&b, res, head);
    sb_printf(&b, "\n\n[... %zu KB cut: this result alone would fill more than %d%% of the %s context window. "
                 "Ask for less — read_file offset/limit, a narrower grep, `| head` — if you need what is missing ...]\n\n",
              (len - head - tail) / 1024, TOOL_RESULT_PCT, fmt_ctx(g_cfg.num_ctx));
    sb_append(&b, t, tail);
    cJSON_AddStringToObject(tm, "content", b.data);
    sb_free(&b);
    printf("  ⎿  " C_GRAY "result trimmed to %zu KB: more than %d%% of the context window" C_RESET "\n", cap / 1024, TOOL_RESULT_PCT);
    return tm;
}

/* every result enters the conversation valid UTF-8: a command's output may be binary, and one bad byte
 * makes the next request body invalid JSON (see messages_repair in util.c) */
static cJSON *tool_result_message(const char *name, const char *id, const char *res) {
    char *clean = utf8_fix(res);
    cJSON *tm = tool_result_build(name, id, clean);
    free(clean);
    return tm;
}

/* the name of a call as the model made it */
static const char *call_name(cJSON *call) {
    cJSON *fn = call ? cJSON_GetObjectItemCaseSensitive(call, "function") : NULL;
    cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
    return cJSON_IsString(nm) ? nm->valuestring : "?";
}

/* A reply's calls that the request stops before running: each still gets a result, so the
 * conversation stays well-formed (the server refuses a tool call without its result). */
static void answer_calls_unrun(cJSON *calls, const char *why) {
    cJSON *call;
    cJSON_ArrayForEach(call, calls) cJSON_AddItemToArray(g_messages, tool_result_message(call_name(call), tool_call_id(call), why));
}

/* tools to offer the model in the current mode (plan mode drops the mutating ones) */
static cJSON *tools_for_mode(void) {
    if (!g_model_tools || g_cfg.no_tools) return NULL;
    if (g_cfg.mode != MODE_PLAN) return g_tools;
    cJSON *arr = cJSON_CreateArray(), *t;
    cJSON_ArrayForEach(t, g_tools) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
        cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        const char *name = cJSON_IsString(nm) ? nm->valuestring : "";
        if (!strcmp(name, "write_file") || !strcmp(name, "edit_file")) continue;
        cJSON_AddItemReferenceToArray(arr, t);
    }
    return arr;
}

/* ---------- sub-agents (the `task` tool) ----------
 * A fresh, read-only agent loop: own message list, own short system prompt (plus the project
 * instructions), tools limited to read_file / list_dir / grep / bash, at most SUBAGENT_ITERS
 * tool rounds. Its streaming output is not shown; each tool call is echoed as one line and
 * the final report is previewed and returned to the parent as the tool result. */
#define SUBAGENT_ITERS 25
/* ---------- sub-agents: the model they run on (/subagent) ----------
 * A sub-agent (task) runs on the model doing the work unless /subagent names another. That model's
 * capabilities are read once and kept, as the advisor's are; its effort is the one kept for that model. */
static model_info g_subagent_info;
static char *g_subagent_info_for = NULL;   /* the model g_subagent_info describes */
static char g_subagent_err[400];           /* why subagent_info() said no */

/* the model sub-agents run on when it is not the one doing the work, else NULL */
static const char *subagent_model(void) {
    return g_cfg.subagent && *g_cfg.subagent && !model_same(g_cfg.subagent, g_cfg.model) ? g_cfg.subagent : NULL;
}
/* what that model can be set to, asked of its server once. False: it cannot be used (g_subagent_err says why). */
static bool subagent_info(void) {
    const char *m = subagent_model();
    if (!m) return true;
    if (g_subagent_info_for && !strcmp(g_subagent_info_for, m)) return true;
    free(g_subagent_info_for); g_subagent_info_for = NULL;
    g_subagent_err[0] = 0;
    if (provider_find(m, NULL)) {
        int r = provider_model_info(m, &g_subagent_info, g_subagent_err, sizeof g_subagent_err);
        if (r < 0) return false;
        if (r > 0) return true;   /* not checked (unreachable): usable with what its provider's models take, and asked again next time */
    } else if (ollama_model_show(m, &g_subagent_info) != 0) {
        snprintf(g_subagent_err, sizeof g_subagent_err, "the server does not know a model '%s'", m);
        return false;
    }
    g_subagent_info_for = xstrdup(m);
    return true;
}

/* ---------- sub-agents: the log of each run (← on an empty prompt line) ----------
 * Every run keeps what it did as plain text: its task, each tool call and the head of its
 * result, the report. The live transcript shows the same lines as they come; this is what is
 * left when the turn is over, or to read a run that is still going. */
#define AGENTS_KEEP   16              /* runs the list keeps; a running one is never dropped */
#define AGENT_LOG_MAX (96 * 1024)     /* bytes of log kept per run (util.c agent_log_add) */
#define AGENT_RESULT_LINES 8          /* lines of a tool result the log keeps */
enum { AG_RUNNING, AG_DONE, AG_STOPPED, AG_FAILED };
typedef struct {
    int id;
    char *desc;
    char *model;          /* the model the run was on, NULL = the model doing the work */
    int state, rounds;
    char activity[200];   /* what it is doing now, or did last */
    sbuf log;
} subagent_rec;
static subagent_rec *g_agents[AGENTS_KEEP];   /* oldest first */
static int g_agent_n = 0, g_agent_seq = 0;

static void agent_free(subagent_rec *r) { free(r->desc); free(r->model); sb_free(&r->log); free(r); }
static subagent_rec *agent_begin(const char *desc, const char *model) {
    if (g_agent_n == AGENTS_KEEP) {
        int i = 0; while (i < g_agent_n && g_agents[i]->state == AG_RUNNING) i++;
        if (i == g_agent_n) return NULL;   /* (every one running: cannot happen, runs do not nest) */
        agent_free(g_agents[i]);
        memmove(g_agents + i, g_agents + i + 1, sizeof *g_agents * (size_t)(g_agent_n - i - 1));
        g_agent_n--;
    }
    subagent_rec *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->id = ++g_agent_seq; r->desc = xstrdup(desc); r->model = model ? xstrdup(model) : NULL;
    r->state = AG_RUNNING; sb_init(&r->log);
    g_agents[g_agent_n++] = r;
    return r;
}
static void agent_say(subagent_rec *r, const char *text) { if (r && text) agent_log_add(&r->log, text, AGENT_LOG_MAX); }
static void agent_act(subagent_rec *r, const char *what) { if (r) snprintf(r->activity, sizeof r->activity, "%s", what); }
/* the head of a tool result, the way the transcript previews it, into the log */
static void agent_say_result(subagent_rec *r, const char *res) {
    if (!r || !res) return;
    int lines = 0; const char *p = res;
    while (*p) {
        const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (lines == AGENT_RESULT_LINES) { agent_say(r, "    …\n"); break; }
        char line[204]; size_t cut = len < 200 ? len : 200;
        while (cut && cut < len && ((unsigned char)p[cut] & 0xC0) == 0x80) cut--;   /* not in the middle of a character */
        memcpy(line, p, cut); line[cut] = 0;
        agent_say(r, "    "); agent_say(r, line); agent_say(r, len > cut ? "…\n" : "\n");
        lines++;
        if (!nl) break;
        p = nl + 1;
    }
}
static const char *agent_state_word(const subagent_rec *r) {
    switch (r->state) {
    case AG_RUNNING: return "running";
    case AG_DONE:    return "done";
    case AG_STOPPED: return "stopped";
    default:         return "failed";
    }
}
/* ← on an empty prompt line: the sub-agents of this session, newest first, and the log of the one picked */
/* ---------- the strip: the running sub-agents, between the message box and the status bar ----------
 * One line each, newest first, as far as the strip has rows. term.c keeps the selection and draws it;
 * strip_open shows the log of the one picked (the same view as the agents menu). */
static int running_count(void) { int k = 0; for (int i = 0; i < g_agent_n; i++) if (g_agents[i]->state == AG_RUNNING) k++; return k; }
static const subagent_rec *running_at(int idx) {
    int k = 0;
    for (int i = g_agent_n - 1; i >= 0; i--) if (g_agents[i]->state == AG_RUNNING && k++ == idx) return g_agents[i];
    return NULL;
}
static int strip_lines(sbuf *o, int cols, int max) {
    int total = running_count();
    int more = total > max ? total - (max - 1) : 0;   /* past the rows the strip has: the last row says how many are not shown */
    int shown = more ? max - 1 : total;
    int rows = shown + (more ? 1 : 0);
    for (int i = 0; i < shown; i++) {
        const subagent_rec *r = running_at(i);
        char line[480];
        snprintf(line, sizeof line, "◆ #%d %s · round %d · %s", r->id, r->desc, r->rounds, r->activity);
        /* cut to the width, on a character boundary, with an ellipsis */
        int room = cols - 4 > 1 ? cols - 4 : 1;
        if (plain_width(line) > room) {
            size_t cut = 0; int w = 0;
            while (line[cut] && w < room - 1) { cut++; while ((unsigned char)line[cut] & 0x80 && ((unsigned char)line[cut] & 0xC0) == 0x80) cut++; w++; }
            line[cut] = 0; strcat(line, "…");
        }
        if (i) sb_putc(o, '\n');
        sb_printf(o, "  " C_ORANGE "%s" C_RESET, line);
    }
    if (more) { if (shown) sb_putc(o, '\n'); sb_printf(o, "  " C_DIM "+%d more running" C_RESET, more); }
    return rows;
}
static void strip_open(int idx) {
    const subagent_rec *r = running_at(idx);
    if (!r) return;
    char head[400];
    snprintf(head, sizeof head, "▾ sub-agent #%d %s · %s%s%s", r->id, r->desc, agent_state_word(r),
             r->model ? " · on " : "", r->model ? r->model : "");
    term_show_block(head, r->log.len ? r->log.data : "(nothing logged yet)\n");
}

static void agents_menu(void) {
    if (!g_agent_n) { printf(C_DIM "no sub-agents have run in this session yet" C_RESET "\n"); return; }
    int n = g_agent_n;
    const char **items = xmalloc(sizeof *items * (size_t)(n + 1)), **descs = xmalloc(sizeof *descs * (size_t)(n + 1));
    items[0] = "main agent"; descs[0] = "the conversation";
    for (int k = 1; k <= n; k++) {
        const subagent_rec *r = g_agents[n - k];
        char *label = xmalloc(64), *desc = xmalloc(320);
        snprintf(label, 64, "#%d %s", r->id, r->desc);
        if (r->state == AG_RUNNING) snprintf(desc, 320, "running · round %d · %s", r->rounds, r->activity);
        else snprintf(desc, 320, "%s · %d round%s", agent_state_word(r), r->rounds, r->rounds == 1 ? "" : "s");
        items[k] = label; descs[k] = desc;
    }
    int pick = term_select("sub-agents", items, descs, n + 1, -1);
    if (pick > 0) {
        const subagent_rec *r = g_agents[n - pick];
        char head[400];
        snprintf(head, sizeof head, "▾ sub-agent #%d %s · %s%s%s", r->id, r->desc, agent_state_word(r),
                 r->model ? " · on " : "", r->model ? r->model : "");
        term_show_block(head, r->log.len ? r->log.data : "(nothing logged yet)\n");
    }
    for (int k = 1; k <= n; k++) { free((char *)items[k]); free((char *)descs[k]); }
    free(items); free(descs);
}

static int run_subagent(const char *description, const char *prompt, sbuf *out) {
    printf("  " C_CYAN "⤷ sub-agent" C_RESET " " C_BOLD "%s" C_RESET "\n", description);
    const char *sm = subagent_model();
    subagent_rec *rec = agent_begin(description, sm);
    agent_say(rec, "task: "); agent_say(rec, prompt); agent_say(rec, "\n");
    if (sm) {
        if (!subagent_info()) {
            sb_printf(out, "error: the sub-agents' model %s cannot be used: %s. Do the task with the tools yourself, or say what is missing.", sm, g_subagent_err);
            agent_say(rec, out->data); if (rec) rec->state = AG_FAILED;
            printf("    " C_RED "⎿ %s" C_RESET "\n", out->data);
            return 1;
        }
        printf(C_DIM "    on %s (/subagent)" C_RESET "\n", sm);
    }
    cJSON *msgs = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    sbuf sp; sb_init(&sp);
    sb_printf(&sp, "You are a sub-agent of Corbie Nest (corbienest), a terminal coding agent. The main agent delegated one self-contained task to you.\n"
                   "Working directory: %s\n"
                   "You have read-only tools: read_file, list_dir, grep, bash (for read-only shell commands such as git log, find, wc; do not modify files, do not run interactive programs)%s. "
                   "Batch independent calls: when you need several reads, searches or fetches that do not depend on each other's results, request them all in one reply rather than one per turn. "
                   "Do the research thoroughly, then finish with ONE final message: a clear, complete report with concrete file paths, line numbers and code snippets where useful — "
                   "the main agent sees only that final message, nothing else you did. Do not ask questions; make reasonable assumptions and state them.",
                   g_cwd, g_cfg.web ? " and web_search/web_fetch for the documentation of external projects" : "");
    if (g_project_instructions) sb_puts(&sp, g_project_instructions);
    size_t sp_bytes = sp.len;
    cJSON_AddStringToObject(sys, "content", sp.data); sb_free(&sp);
    cJSON_AddItemToArray(msgs, sys);
    cJSON *u = cJSON_CreateObject(); cJSON_AddStringToObject(u, "role", "user"); cJSON_AddStringToObject(u, "content", prompt);
    cJSON_AddItemToArray(msgs, u);
    cJSON *tools = cJSON_CreateArray(), *t;
    cJSON_ArrayForEach(t, g_tools) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
        cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        const char *name = cJSON_IsString(nm) ? nm->valuestring : "";
        if (!strcmp(name, "read_file") || !strcmp(name, "list_dir") || !strcmp(name, "grep") || !strcmp(name, "bash") ||
            !strcmp(name, "web_fetch") || !strcmp(name, "web_search")) cJSON_AddItemReferenceToArray(tools, t);
    }
    int rc = 1, tool_rounds = 0, round_first = -1;
    bool interrupted = false;     /* the user stopped it (Esc / Ctrl-C) */
    const char *final = NULL;
    for (int iter = 0; iter <= SUBAGENT_ITERS; iter++) {
        /* the sub-agent has its own conversation and up to SUBAGENT_ITERS rounds of results in
         * it: keep it inside the window the same way, on bytes (nothing here is measured) */
        if (round_first > 0 && g_cfg.num_ctx > 0 &&
            (double)(sp_bytes + tools_bytes() + conv_bytes(msgs)) / g_bytes_per_token > g_cfg.num_ctx * (ELIDE_PCT / 100.0))
            elide_tool_results(msgs, round_first);
        chat_stats st; bool aborted = false;
        if (rec) rec->rounds = tool_rounds + 1;
        agent_act(rec, "waiting for the model");
        char label[48]; snprintf(label, sizeof label, "sub-agent · round %d", tool_rounds + 1);
        ollama_quiet = true; ollama_call.busy = label;
        ollama_call.followup = iter > 0;   /* think=auto: think about the task once, not after every tool result */
        if (sm) { ollama_call.model = sm; ollama_call.info = &g_subagent_info; }
        cJSON *reply = model_chat(msgs, g_model_tools ? tools : NULL, &st, &aborted);
        ollama_call_reset(); ollama_quiet = false;
        account_as(sm ? sm : g_cfg.model, &st);   /* its own model's tokens: /usage prices them as that model's */
        term_status_refresh();
        if (!reply) { sb_puts(out, aborted ? "error: sub-agent interrupted by the user" : "error: sub-agent model call failed"); interrupted = aborted; break; }
        cJSON_AddItemToArray(msgs, reply);
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(reply, "content");
        if (aborted) { sb_puts(out, "error: sub-agent interrupted by the user"); interrupted = true; break; }
        if (!calls || cJSON_GetArraySize(calls) == 0) { final = cJSON_IsString(content) ? content->valuestring : ""; rc = 0; break; }
        if (iter == SUBAGENT_ITERS) { sb_printf(out, "error: sub-agent stopped after %d tool rounds without a report", SUBAGENT_ITERS); if (cJSON_IsString(content) && content->valuestring[0]) sb_printf(out, "\nIts last message:\n%s", content->valuestring); break; }
        cJSON *call;
        round_first = cJSON_GetArraySize(msgs);
        cJSON_ArrayForEach(call, calls) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            const char *name = cJSON_IsString(nm) ? nm->valuestring : "?";
            cJSON *parsed = NULL;
            if (cJSON_IsString(args)) { parsed = cJSON_Parse(args->valuestring); args = parsed; }
            char summ[160]; tool_arg_summary(name, args, summ, sizeof summ);
            printf("    " C_DIM "⎿ %s%s%s%s" C_RESET "\n", name, summ[0] ? "(" : "", summ, summ[0] ? ")" : "");
            agent_say(rec, "› "); agent_say(rec, name); agent_say(rec, summ[0] ? "(" : ""); agent_say(rec, summ); agent_say(rec, summ[0] ? ")\n" : "\n");
            char act[200]; snprintf(act, sizeof act, "%s%s%s%s", name, summ[0] ? "(" : "", summ, summ[0] ? ")" : "");
            agent_act(rec, act);
            sbuf o; sb_init(&o);
            g_session.tool_calls++;
            tool_status ts;
            if (!strcmp(name, "write_file") || !strcmp(name, "edit_file") || !strcmp(name, "task")) { sb_printf(&o, "error: %s is not available to a sub-agent (read-only)", name); ts = TOOL_ERROR; }
            else if (!strcmp(name, "advisor")) { sb_puts(&o, "error: the advisor is not available to a sub-agent (it would be shown the wrong conversation); report what you found"); ts = TOOL_ERROR; }
            else ts = tools_execute(name, args, &o);
            (void)ts;
            agent_say_result(rec, o.data);
            cJSON_AddItemToArray(msgs, tool_result_message(name, tool_call_id(call), o.data ? o.data : ""));
            sb_free(&o);
            if (parsed) cJSON_Delete(parsed);
        }
        tool_rounds++;
        if (term_poll_interrupt()) { sb_puts(out, "error: sub-agent interrupted by the user"); interrupted = true; break; }
    }
    if (rc == 0) {
        sb_printf(out, "%s", final && *final ? final : "(the sub-agent returned an empty report)");
        printf("    " C_DIM "⎿ report after %d tool round%s:" C_RESET "\n", tool_rounds, tool_rounds == 1 ? "" : "s");
        print_result_preview(final && *final ? final : "(empty)", 8);
    } else printf("    " C_RED "⎿ %s" C_RESET "\n", out->data ? out->data : "sub-agent failed");
    if (rec) {
        rec->state = interrupted ? AG_STOPPED : rc == 0 ? AG_DONE : AG_FAILED;
        agent_say(rec, rc == 0 ? "report:\n" : "result:\n");
        agent_say(rec, out->data ? out->data : "");
        agent_say(rec, "\n");
        rec->activity[0] = 0;
    }
    cJSON_Delete(tools);
    cJSON_Delete(msgs);
    return rc;
}

/* ---------- the advisor (/advisor, the `advisor` tool) ----------
 * A stronger model the agent may consult when the work is hard — a bigger local model, one of
 * Ollama's cloud models (NAME-cloud, which the local server relays once `ollama signin` is done),
 * or a hosted API (xai:MODEL, openai:MODEL, anthropic:MODEL — provider.c). It is not part of the
 * conversation: it is *shown* it, as one quoted text (transcript_text()) with the agent's question
 * under it, has no tools, and what comes back is advice — the tool result. A weak model writes a
 * poor brief, which is why the transcript goes along whatever the question says.
 *
 * What it costs is time, and on one machine more than that: a second local model may push the
 * main one out of memory, and the main model's next call then reloads it and reads the whole
 * conversation again. Hence a limit per request (the guidance's), a window of its own (a 70B
 * model pays several times the memory per token of context that a small one does) and
 * keep_alive 0, so that its memory is free again when the main model wants it back. Its tokens
 * are counted in the session totals, but never in the context estimate: that is another
 * model's prompt.
 *
 * How much it is leaned on is /advisor guidance (ADVISOR_GUIDANCE in util.c): from strong up,
 * corbienest consults it as well — it reviews a request that changed a lot before the request
 * ends (any change at max), and at max it checks the request's first change before the change is made. Those two
 * come as a tool call of the agent's own (advisor_inject()), so the conversation keeps the shape
 * every chat template knows, and they do not count against the agent's consultations. */
static const char *effort_label(const char *model, const model_info *mi);
static void effort_command(const char *who, const char *model, const model_info *mi, const char *arg);
#define ADVISOR_IDLE_MS (30 * 60 * 1000)   /* a cold 30B model loading and then reading 12k tokens on a CPU is past http.c's ten minutes */
static model_info g_advisor_info;
static char *g_advisor_info_for = NULL;   /* the model g_advisor_info describes (fetched on first use; per host) */
static char g_advisor_err[400];           /* why advisor_info() said no — or, when it said yes, why the model could not be checked */

static bool advisor_is_main(void) { return model_same(g_cfg.advisor, g_cfg.model); }

/* What the advisor model can be set to: /api/show, or a hosted API's model endpoint. False when
 * it cannot be used (the server does not know it; a hosted API without a key, refusing the key,
 * or without such a model) — g_advisor_err says which. */
static bool advisor_info(void) {
    if (g_advisor_info_for && !strcmp(g_advisor_info_for, g_cfg.advisor)) return true;
    free(g_advisor_info_for); g_advisor_info_for = NULL;
    g_advisor_err[0] = 0;
    if (provider_find(g_cfg.advisor, NULL)) {
        int r = provider_model_info(g_cfg.advisor, &g_advisor_info, g_advisor_err, sizeof g_advisor_err);
        if (r < 0) return false;
        if (r > 0) return true;   /* not checked (unreachable): usable with what its provider's models take, and asked again next time */
    } else if (ollama_model_show(g_cfg.advisor, &g_advisor_info) != 0) {
        snprintf(g_advisor_err, sizeof g_advisor_err, "the server does not know a model '%s'", g_cfg.advisor);
        return false;
    }
    g_advisor_info_for = xstrdup(g_cfg.advisor);
    return true;
}
static void advisor_plan_now(advisor_plan *p) {
    bool known = g_cfg.advisor && g_advisor_info_for && !strcmp(g_advisor_info_for, g_cfg.advisor);   /* /advisor ctx may come before any advisor is chosen */
    advisor_plan_for(g_cfg.advisor, known ? g_advisor_info.context_length : 0, p);
}

/* its system prompt: only ever sent to the advisor, so it may follow the guidance freely */
static void advisor_system(sbuf *b) {
    const advisor_guidance_def *g = advisor_guidance();
    sb_puts(b,
        "You are the advisor to a coding agent called Corbie Nest. A smaller, faster model is doing the work in the user's terminal — "
        "reading and editing files, running commands — and has stopped to consult you, because you are the stronger model. "
        "You are shown its conversation so far: what the user asked for, what the agent said, the tools it called and what they returned "
        "(long outputs are cut in the middle, and old messages may be left out). You have no tools and see nothing beyond that transcript, "
        "so never claim to have checked a file or run anything.\n\n");
    sb_printf(b, "Answer the agent, not the user. Be concrete and brief — under about %d words, since your answer goes straight into the agent's small context:\n", g->words);
    sb_puts(b,
        "- Start with the verdict: whether the agent is on the right track, and which approach you recommend. Then the steps, in order, naming the files, functions and commands.\n"
        "- If the agent is on the wrong track, misread an error, or skipped a check, say so plainly. Do not agree with it because it sounds sure of itself.\n"
        "- If the transcript does not tell you enough, say exactly what the agent should read or run to find out — do not guess at what a file contains.\n"
        "- Keep to the project's rules when they are shown to you; advice that breaks them is of no use to the agent.\n"
        "- If the work looks finished and correct, say so in a sentence and name what is still worth verifying.\n");
    if (g_cfg.advisor_guidance >= GUIDANCE_STRONG)
        sb_puts(b, "- The agent is a much smaller model: it follows exact steps far better than general advice. Give each step its file and function, "
                   "and where a change is small, write it out — the lines to replace and what replaces them.\n");
    sb_puts(b, "No preamble, and no recap of the transcript.");
}

enum { CONSULT_ASKED, CONSULT_REVIEW, CONSULT_CHECK };                /* the agent asked; corbienest did (guidance strong/max) */
enum { ADVICE_GIVEN, ADVICE_NONE, ADVICE_SKIPPED };  /* what came of it (ADVICE_SKIPPED: Esc or Ctrl-C) */

/* One toolless call to the advisor model — a hosted API or a model on the Ollama server — with
 * a system prompt and one user message, accounted to it. The answer, malloc'd, or NULL: then
 * `r` says whether the user stopped it, or why it failed. */
typedef struct { chat_stats st; bool aborted, not_found, signin, unusable; char err[512]; } advisor_reply;
static char *advisor_call(const char *sys, const char *user, const advisor_plan *plan, bool known, int reply, const char *label, advisor_reply *r) {
    memset(r, 0, sizeof *r);
    char *answer = NULL;
    if (plan->api) {
        provider_request rq = { sys, user, reply, effort_get(g_cfg.advisor), label, ADVISOR_IDLE_MS };
        if (known) answer = provider_chat(g_cfg.advisor, &g_advisor_info, &rq, &r->st, &r->aborted, r->err, sizeof r->err);
        else { snprintf(r->err, sizeof r->err, "%s", g_advisor_err); r->unusable = true; }   /* no key, the key refused, no such model */
    } else {
        cJSON *msgs = cJSON_CreateArray();
        cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "system"); cJSON_AddStringToObject(m, "content", sys); cJSON_AddItemToArray(msgs, m);
        m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", user); cJSON_AddItemToArray(msgs, m);
        ollama_quiet = true;
        ollama_call.busy = label; ollama_call.num_predict = reply; ollama_call.idle_ms = ADVISOR_IDLE_MS;
        ollama_call.model = g_cfg.advisor; ollama_call.info = known ? &g_advisor_info : NULL;
        ollama_call.num_ctx = plan->send_ctx; ollama_call.keep_alive = plan->keep_alive;
        cJSON *rep = ollama_chat(msgs, NULL, &r->st, &r->aborted);
        ollama_call_reset(); ollama_quiet = false;
        cJSON_Delete(msgs);
        cJSON *c = rep ? cJSON_GetObjectItemCaseSensitive(rep, "content") : NULL;
        if (rep) answer = xstrdup(cJSON_IsString(c) ? c->valuestring : "");
        else { snprintf(r->err, sizeof r->err, "%s", ollama_error[0] ? ollama_error : "request failed"); r->not_found = strstr(ollama_error, "not found") != NULL; r->signin = strcasestr(ollama_error, "unauthorized") != NULL; }
        if (rep) cJSON_Delete(rep);
    }
    account_as(g_cfg.advisor, &r->st);
    g_session.advisor_tokens += r->st.prompt_tokens + r->st.eval_tokens; g_session.advisor_seconds += r->st.total_seconds;
    g_session.advisor_eval_tokens += r->st.eval_tokens; g_session.advisor_eval_seconds += r->st.eval_seconds;
    term_status_refresh();
    return answer;
}

/* " · ≈ $0.03" for a call to a model that has a price (estimated as /usage does), else nothing */
static void cost_note(const char *model, const chat_stats *st, char *out, size_t n) {
    double pi, po; out[0] = 0;
    if (!model_price(model, &pi, &po)) return;
    char usd[32]; fmt_usd(((double)st->prompt_tokens * pi + (double)st->eval_tokens * po) / 1e6, usd, sizeof usd);
    snprintf(out, n, " · ≈ %s", usd);
}

/* One consultation. Prints the ⤷ line, then for ADVICE_GIVEN the advice as a fold (its head says how
 * long it is; a click shows it, the preview where there are no folds) and for anything else one ⎿
 * line for whatever comes of it; the advice (for ADVICE_GIVEN) goes into `advice`, and for
 * ADVICE_NONE the tool result that says why there is none into `why`. `what` is the middle of
 * the ⤷ line ("consultation 1 of 3 in this request"). */
static int consult(const char *question, const char *what, sbuf *advice, sbuf *why) {
    bool known = advisor_info();
    advisor_plan plan; advisor_plan_now(&plan);
    const advisor_guidance_def *g = advisor_guidance();
    struct utsname un; uname(&un);
    sbuf env; sb_init(&env);
    sb_printf(&env, "Working directory: %s · %s %s · git repository: %s", g_cwd, un.sysname, un.machine, is_dir(".git") ? "yes" : "no");
    sbuf sys; sb_init(&sys); advisor_system(&sys);
    /* after a compaction in mid-turn nothing points at the request any more: it is in message 0, under the summary */
    char *brief = advisor_brief(g_messages, g_prev_request_first >= 0 ? g_prev_request_first : 0, plan.budget > sys.len + 2048 ? plan.budget - sys.len : 2048,
                                env.data, g_cfg.mode == MODE_PLAN, g_project_instructions, question, g->words);
    sb_free(&env);
    printf("  " C_CYAN "⤷ advisor" C_RESET " " C_BOLD "%s" C_RESET C_DIM " · %s · %zu KB of the conversation · esc skips it" C_RESET "\n",
           g_cfg.advisor, what, (strlen(brief) + 1023) / 1024);
    /* another local model may push the main one out of memory; say so when it did, since the
     * wait that follows would otherwise look like a fault */
    double sz, vram; bool watch = !plan.same && !plan.cloud && g_cfg.interactive;
    bool main_was_loaded = watch && ollama_model_placement(g_cfg.model, &sz, &vram) == 0;

    char label[96]; snprintf(label, sizeof label, "advisor · %.70s", g_cfg.advisor);
    advisor_reply ar;
    char *answer = advisor_call(sys.data, brief, &plan, known, plan.reply, label, &ar);   /* NULL: it failed, and ar says why */
    chat_stats st = ar.st;
    bool aborted = ar.aborted, not_found = ar.not_found, signin = ar.signin, unusable = ar.unusable;
    const char *err = ar.err;
    free(brief); sb_free(&sys);

    int res = ADVICE_NONE;
    const char *text = strip_think_block(answer ? answer : "");
    bool cut = !strcmp(st.done_reason, "length");
    if (aborted) {
        printf("    " C_RED "⎿ interrupted" C_RESET "\n");
        res = ADVICE_SKIPPED;
    } else if (!answer && not_found) {
        sb_printf(why, "error: the advisor model %s is not installed on %s. Carry on without it.", g_cfg.advisor, g_cfg.host);
        printf("    " C_YELLOW "⎿ advisor '%s' not found — /advisor picks another, /advisor off stops offering it" C_RESET "\n", g_cfg.advisor);
    } else if (!answer) {
        sb_printf(why, "error: the advisor (%s) %s: %s. Carry on without it.", g_cfg.advisor, unusable ? "cannot be used" : "could not be reached", err);
        if (signin) printf("    " C_YELLOW "⎿ a cloud model needs an account: run \"ollama signin\" in a shell, then ask again" C_RESET "\n");
        else printf("    " C_RED "⎿ no advice: %s" C_RESET "\n", err);
        if (plan.api && effort_get(g_cfg.advisor) && strstr(err, "answered 400")) printf(C_DIM "      if it is the effort the model does not take: /advisor effort default leaves it to the model" C_RESET "\n");
    } else if (!*text) {
        sb_printf(why, "error: the advisor gave no answer%s. Carry on without it, or ask something narrower.", cut ? " — it was still thinking when it ran out of tokens" : "");
        printf("    " C_RED "⎿ no answer" C_RESET C_DIM "%s" C_RESET "\n", cut ? " — it ran out of tokens while still thinking: lower /advisor effort, or enlarge /advisor ctx" : "");
    } else {
        g_session.advisor_calls++;
        sb_puts(advice, text);
        if (cut) sb_puts(advice, "\n[… the answer was cut off here: the advisor ran out of tokens]");
        char tk[32]; fmt_tokens(st.prompt_tokens + st.eval_tokens, tk, sizeof tk);
        char cost[48]; cost_note(g_cfg.advisor, &st, cost, sizeof cost);   /* a model with a price: what this one consultation came to */
        sbuf info; sb_init(&info);   /* the head: plain text, so it can be cut to the width */
        sb_printf(&info, "advice · %s tokens · %.0fs%s%s · %d lines", tk, st.total_seconds, cost, st.load_seconds >= 1 ? " (of which loading the model)" : "", count_lines(advice->data));
        int w = term_width() - 8; if (w < 20) w = 20;   /* "    ▸ " prefix */
        size_t cut = info.len > (size_t)w ? (size_t)w : info.len;
        while (cut > 0 && cut < info.len && ((unsigned char)info.data[cut] & 0xC0) == 0x80) cut--;   /* don't split UTF-8 */
        sbuf head; sb_init(&head);
        sb_printf(&head, "    %s " C_DIM "%.*s%s" C_RESET, TERM_FOLD_CLOSED, (int)cut, info.data, cut < info.len ? "…" : "");
        if (term_fold_begin(head.data)) { fold_body(advice->data, count_lines(advice->data)); term_fold_end(); }   /* a click shows the advice */
        else { printf("    " C_DIM "%.*s%s" C_RESET "\n", (int)cut, info.data, cut < info.len ? "…" : ""); print_result_preview(text, 12); }   /* no folds here: as before */
        sb_free(&head); sb_free(&info);
        res = ADVICE_GIVEN;
    }
    if (main_was_loaded && !aborted && ollama_model_placement(g_cfg.model, &sz, &vram) != 0)
        printf("    " C_YELLOW "⎿ %s was unloaded to make room: its next reply reloads it and reads the conversation again" C_RESET "\n", g_cfg.model);
    free(answer);
    return res;
}

/* The tool. Every outcome is one ⎿ line (run_turn prints nothing more for this tool) and one
 * result for the model; none of them ends the turn. */
/* The limit is per request, and a request can run for an hour: what was a fair number for a
 * question is then far too few for the job. So the count — and with it the one stepping-in —
 * starts again every g_cfg.advisor_window seconds of a request (/advisor window; 0 = never).
 * Bounded by the clock, it still cannot become two models talking to each other. */
static void advisor_window_roll(void) {
    time_t now = time(NULL);
    if (g_cfg.advisor_window <= 0 || now - g_advisor_since < g_cfg.advisor_window) return;
    g_advisor_uses = 0; g_advisor_stepped_in = false; g_advisor_since = now;
}

static int run_advisor(const char *question, sbuf *out) {
    g_advisor_said = true;
    int most = advisor_guidance()->uses;
    advisor_window_roll();
    if (g_advisor_uses >= most) {   /* the tool stays on offer: taking it away would change the prompt, and with it the cache */
        char again[96] = "";
        if (g_cfg.advisor_window > 0) {
            long left = (long)g_cfg.advisor_window - (long)(time(NULL) - g_advisor_since), mins = (left + 59) / 60;
            snprintf(again, sizeof again, " It can be asked again in about %ld minute%s.", mins < 1 ? 1 : mins, mins <= 1 ? "" : "s");
        }
        sb_printf(out, "error: the advisor has been consulted %d time%s in this request, which is the limit.%s Carry on with the advice you have; if you are still stuck, tell the user where and why.", most, most == 1 ? "" : "s", again);
        printf("  " C_DIM "⎿ not asked: %d consultation%s per request is the limit%s (/advisor guidance, /advisor window)" C_RESET "\n", most, most == 1 ? "" : "s", again[0] ? " for now" : "");
        return 1;
    }
    int k = ++g_advisor_uses;   /* whatever comes of it: a consultation that ends in nothing took its minutes as well */
    char what[64]; snprintf(what, sizeof what, "consultation %d of %d in this request", k, most);
    sbuf advice, why; sb_init(&advice); sb_init(&why);
    int rc = 1;
    switch (consult(question, what, &advice, &why)) {
    case ADVICE_GIVEN:
        sb_printf(out, "%s\n\n[advice from %s — consultation %d of %d in this request. Weigh it seriously, but where the files or a command's output contradict it, they are right: say so and adapt.]",
                  advice.data, g_cfg.advisor, k, most);
        rc = 0; break;
    case ADVICE_SKIPPED:
        sb_puts(out, "error: the consultation was interrupted by the user — carry on without it, and do not consult the advisor again for this request unless the user asks");
        break;
    default:
        sb_puts(out, why.data ? why.data : "error: no advice. Carry on without it.");
    }
    sb_free(&advice); sb_free(&why);
    return rc;
}

/* An advisor call the agent did not make, put into the conversation as if it had: its reply
 * `reply` (already in g_messages) gets the call, and the advice follows as the call's result. A
 * conversation of that shape is one every chat template renders, and the advice is a tool result
 * like any other advice — where a note from the "user" would be neither. */
static void advisor_inject(cJSON *reply, const char *label, const char *result) {
    static int seq;
    char id[32]; snprintf(id, sizeof id, "call_adv_%d", ++seq);
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
    if (!calls) calls = cJSON_AddArrayToObject(reply, "tool_calls");
    cJSON *call = cJSON_CreateObject(), *fn = cJSON_AddObjectToObject(call, "function");
    cJSON_AddStringToObject(call, "id", id);
    cJSON_AddStringToObject(fn, "name", "advisor");
    cJSON *a = cJSON_AddObjectToObject(fn, "arguments"); cJSON_AddStringToObject(a, "question", label);
    cJSON_AddItemToArray(calls, call);
    /* a reply that already carries the blocks it must be sent back as needs this call in them
     * too, or the result that follows has no tool_use to match */
    cJSON *pc = cJSON_GetObjectItemCaseSensitive(reply, "provider_content");
    if (cJSON_IsArray(pc)) {
        cJSON *b = cJSON_CreateObject(), *in = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "type", "tool_use");
        cJSON_AddStringToObject(b, "id", id);
        cJSON_AddStringToObject(b, "name", "advisor");
        cJSON_AddStringToObject(in, "question", label);
        cJSON_AddItemToObject(b, "input", in);
        cJSON_AddItemToArray(pc, b);
    }
    cJSON_AddItemToArray(g_messages, tool_result_message("advisor", id, result));
}

/* guidance strong and up: before a request that changed a lot ends, the advisor looks at it. Strong reviews one
 * that changed ADVISOR_REVIEW_LINES lines or ADVISOR_REVIEW_FILES files; max any change. Tool calls alone are not a change: git work and
 * a reading run are not reviewed. Edits made by a shell command (sed, a script) are not checkpoints, so they are not
 * counted. */
static bool advisor_review_due(void) {
    if (!(g_cfg.advisor && advisor_guidance()->review && !g_advisor_reviewed && g_model_tools && !g_cfg.no_tools
          && g_cfg.mode != MODE_PLAN && g_prev_request_first >= 0)) return false;
    if (tools_checkpoint_files(g_prev_request_first, NULL) == 0) return false;
    if (advisor_guidance()->review_any) return true;
    int lines = 0, files = tools_request_changes(g_prev_request_first, &lines);
    return files >= ADVISOR_REVIEW_FILES || lines >= ADVISOR_REVIEW_LINES;
}

/* normal and up: the tool calls have failed ADVISOR_FAIL_ROUNDS rounds in a row. A small model
 * goes on guessing at this point far more often than it asks, and each guess is a round of its
 * time — so the advisor is asked for it. Once per request: if its advice does not help, the
 * agent can still ask again itself, and the request cannot turn into a dialogue of two models. */
static bool advisor_step_in_due(void) {
    advisor_window_roll();
    return g_cfg.advisor && advisor_guidance()->step_in && !g_advisor_stepped_in && g_fail_rounds >= ADVISOR_FAIL_ROUNDS
        && g_model_tools && !g_cfg.no_tools;
}
static void advisor_step_in(cJSON *reply) {
    g_advisor_stepped_in = true;
    sbuf advice, why; sb_init(&advice); sb_init(&why);
    int r = consult("The agent's tool calls have failed for " "two" " rounds in a row — the calls and their results are the last things in the conversation above. "
                    "It has not asked you; you are brought in because it may be guessing. What is going wrong, and what exactly should it do next? "
                    "If its approach is the problem, say which to take instead.",
                    "steps in: the tool calls keep failing", &advice, &why);
    if (r == ADVICE_GIVEN) {
        sbuf res; sb_init(&res);
        sb_printf(&res, "%s\n\n[the advisor (%s) was brought in because your tool calls failed %d rounds in a row — corbienest asked it, not you. Act on what it says, "
                        "or where the files or a command's output show it is wrong, say so.]", advice.data, g_cfg.advisor, ADVISOR_FAIL_ROUNDS);
        advisor_inject(reply, "(the tool calls keep failing)", res.data);
        sb_free(&res);
    }
    sb_free(&advice); sb_free(&why);
}

/* The review. True when it found something: the advice is in the conversation and the agent
 * gets another round to act on it. Once per request, whatever comes of it. */
static bool advisor_review(cJSON *reply) {
    g_advisor_reviewed = true;
    printf("\n");
    sbuf advice, why; sb_init(&advice); sb_init(&why);
    int r = consult("The agent has ended its turn with the reply above, and the request goes back to the user unless you object. "
                    "Review the work against what the user asked for: is it done, correct, and verified (built, tested) where it can be? "
                    "If nothing needs to change, answer with " ADVISOR_REVIEW_OK " alone. Otherwise say what is wrong or missing, and what the agent should do about it.",
                    "reviews the work before the request ends", &advice, &why);
    bool more = r == ADVICE_GIVEN && !advisor_approves(advice.data);
    if (more) {
        sbuf res; sb_init(&res);
        sb_printf(&res, "%s\n\n[the advisor (%s) reviewed the work before the request ended — corbienest asked it, not you. Act on what it says, "
                        "or where the files or a command's output show it is wrong, say so; then finish.]", advice.data, g_cfg.advisor);
        advisor_inject(reply, "(review of the work before the request ends)", res.data);
        sb_free(&res);
        printf("\n");
    } else if (r == ADVICE_GIVEN) printf("    " C_GREEN "⎿ nothing to change" C_RESET "\n");
    sb_free(&advice); sb_free(&why);
    return more;
}

/* guidance max: the first change of a request is looked at before it is made */
static bool advisor_check_due(const char *tool) {
    return g_cfg.advisor && advisor_guidance()->check_first_edit && !g_advisor_checked && g_cfg.mode != MODE_PLAN
        && (!strcmp(tool, "write_file") || !strcmp(tool, "edit_file"));
}

/* The check. ADVICE_GIVEN with objections: the change is held back and *out is the tool result
 * that says why; anything else: go ahead. */
static int advisor_check(const char *tool, cJSON *args, sbuf *out) {
    g_advisor_checked = true;
    char *a = args ? cJSON_PrintUnformatted(args) : xstrdup("{}");
    sbuf q; sb_init(&q);
    sb_printf(&q, "The agent is about to make the first change of this request: the last tool call in the transcript, %s. It has not been made yet. "
                  "Is it the right change, in the right place, and complete? If it is, answer with " ADVISOR_REVIEW_OK " alone. "
                  "If not, say what is wrong and what to do instead.\nThe call in full: ", tool);
    sb_put_cut(&q, a, 3000);
    free(a);
    sbuf advice, why; sb_init(&advice); sb_init(&why);
    int r = consult(q.data, "checks the first change before it is made", &advice, &why);
    sb_free(&q);
    if (r == ADVICE_GIVEN && !advisor_approves(advice.data))
        sb_printf(out, "not applied: the advisor (%s) checked this change before it was made — corbienest asked it, not you — and says:\n%s\n\n"
                       "[Reconsider, then make the change you settle on: this one again if the files or a command's output show the advisor is wrong.]", g_cfg.advisor, advice.data);
    else if (r == ADVICE_GIVEN) { printf("    " C_GREEN "⎿ go ahead" C_RESET "\n"); r = ADVICE_NONE; }
    else r = ADVICE_NONE;
    sb_free(&advice); sb_free(&why);
    return r;
}

/* what an advisor can be and how each kind is set up — a hosted one needs a key in the
 * environment, which nothing else on screen would tell — and what guidance and effort do to it.
 * Says whether a key is there, never the key. */
static void advisor_howto(void) {
    printf(C_DIM "  a stronger model the agent may consult when the work is hard — /advisor MODEL, where MODEL is\n"
                 "    a bigger local model    as /models lists it\n"
                 "    an Ollama cloud model   like gpt-oss:120b-cloud — run `ollama signin` once\n"
                 "    a hosted API            PROVIDER:MODEL — export its key before starting corbienest (it is never saved)\n" C_RESET);
    const provider_def *p;
    for (int i = 0; (p = provider_at(i)); i++) {
        char name[96]; snprintf(name, sizeof name, "%s:%s", p->name, p->example);
        const char *key = getenv(p->key_env); bool set = key && *key;
        printf(C_DIM "      %-24s %-18s" C_RESET "%s%s" C_RESET "\n", name, p->key_env, set ? C_GREEN : C_DIM, set ? "set" : "not set");
    }
    printf(C_DIM "    another endpoint for a provider: its …_BASE_URL (https, unless it is this machine)\n"
                 "  /advisor guidance LEVEL — how much the agent leans on it" C_RESET "\n");
    for (int i = 0; i < GUIDANCE_COUNT; i++)
        printf(C_DIM "    %-7s %s%s" C_RESET "\n", ADVISOR_GUIDANCE[i].name, ADVISOR_GUIDANCE[i].desc, &ADVISOR_GUIDANCE[i] == advisor_guidance() ? " · current" : "");
    printf(C_DIM "    from normal up it is also brought in, unasked, when tool calls keep failing" C_RESET "\n");
    printf(C_DIM "  /advisor window MINUTES — a request that runs long gets its consultations again after that long (now %s; off = never)" C_RESET "\n", g_cfg.advisor_window > 0 ? fmt_window() : "off");
    printf(C_DIM "  /advisor effort LEVEL — how hard the advisor itself thinks, once one is set: the levels are the model's own\n"
                 "    (off, on, or low … max), bare it lists them, and default leaves it to the model" C_RESET "\n");
}

static void advisor_report(void) {
    const advisor_guidance_def *g = advisor_guidance();
    if (!g_cfg.advisor) { printf("advisor: " C_DIM "none" C_RESET "\n"); advisor_howto(); return; }
    printf("advisor: " C_BOLD "%s" C_RESET, g_cfg.advisor);
    if (g_advisor_info_for && !strcmp(g_advisor_info_for, g_cfg.advisor)) printf(C_DIM " · effort %s" C_RESET, effort_label(g_cfg.advisor, &g_advisor_info));
    else if (effort_get(g_cfg.advisor)) printf(C_DIM " · effort %s" C_RESET, effort_get(g_cfg.advisor));
    advisor_plan p; advisor_plan_now(&p);
    printf(C_DIM " · ctx %s%s · guidance %s · %d consultation%s this session · at most %d per request%s%s" C_RESET "\n", p.cloud ? "its own" : fmt_ctx(p.send_ctx > 0 ? p.send_ctx : g_cfg.num_ctx),
           p.same ? " (the main model's)" : p.api ? " (a hosted API)" : p.cloud ? " (a cloud model)" : g_cfg.advisor_ctx > 0 ? "" : " (auto)", g->name,
           g_session.advisor_calls, g_session.advisor_calls == 1 ? "" : "s", g->uses, g_cfg.advisor_window > 0 ? ", again every " : "", g_cfg.advisor_window > 0 ? fmt_window() : "");
}

/* what this advisor costs, said once when it is chosen */
static void advisor_caveat(void) {
    advisor_plan p; advisor_plan_now(&p);
    const provider_def *pv = provider_find(g_cfg.advisor, NULL);
    if (p.same) printf(C_DIM "  it is the model doing the work: a second opinion, not a stronger one — and each consultation makes its next reply read the conversation again (one prompt cache)" C_RESET "\n");
    else if (pv) printf(C_DIM "  a hosted model: the conversation is sent to %s with each consultation, billed to the key in %s" C_RESET "\n", provider_base_url(pv), pv->key_env);
    else if (p.cloud) printf(C_DIM "  a cloud model: the conversation is sent to ollama.com with each consultation, and the Ollama daemon has to be signed in (ollama signin)" C_RESET "\n");
    else printf(C_DIM "  a local advisor may push %s out of memory: its next call then reloads it and reads the conversation again" C_RESET "\n", g_cfg.model);
}

/* the tool list is built once and the running turn holds it: rebuild it so the model sees (or stops seeing) the advisor */
static void advisor_tools_changed(void) { cJSON_Delete(g_tools); g_tools = tools_definitions(); g_tools_bytes = 0; }

static void advisor_set(const char *model) {
    if (!model) {
        if (!g_cfg.advisor) { printf(C_DIM "no advisor was set" C_RESET "\n"); return; }
        free(g_cfg.advisor); g_cfg.advisor = NULL; free(g_advisor_info_for); g_advisor_info_for = NULL; config_save(); advisor_tools_changed();
        printf(C_GREEN "✓ advisor off" C_RESET "\n");
        return;
    }
    char *old = g_cfg.advisor; g_cfg.advisor = xstrdup(model);
    bool hosted = provider_find(model, NULL) != NULL;
    if (!advisor_info()) {   /* /api/show knows cloud models too, which /models does not list */
        if (hosted) printf(C_RED "✗ %s" C_RESET "\n", g_advisor_err);
        else printf(C_RED "✗ the server does not know a model '%s'" C_RESET C_DIM " — /models lists what is installed; a cloud model is named like gpt-oss:120b-cloud, a hosted one like xai:grok-4.7" C_RESET "\n", model);
        free(g_cfg.advisor); g_cfg.advisor = old;
        return;
    }
    free(old); config_save(); advisor_tools_changed();
    printf(C_GREEN "✓ advisor: %s" C_RESET C_DIM " · effort %s%s" C_RESET "\n", g_cfg.advisor, effort_label(g_cfg.advisor, &g_advisor_info),
           g_model_tools && !g_cfg.no_tools ? "" : " · idle for now: the agent needs tools to ask it");
    if (g_advisor_err[0]) printf(C_YELLOW "  not checked: %s" C_RESET C_DIM " — it is tried at the first consultation" C_RESET "\n", g_advisor_err);
    const advisor_guidance_def *g = advisor_guidance();
    printf(C_DIM "  consulted through the advisor tool, at most %d time%s per request%s%s · guidance %s (/advisor guidance)" C_RESET "\n", g->uses, g->uses == 1 ? "" : "s",
           g_cfg.advisor_window > 0 ? ", and again every " : "", g_cfg.advisor_window > 0 ? fmt_window() : "", g->name);
    advisor_caveat();
}

static void guidance_set(int n) {
    if (g_while_busy) {
        g_pending_guidance = n;
        printf(C_GREEN "✓ advisor guidance: %s" C_RESET C_DIM " — from the next request" C_RESET "\n", ADVISOR_GUIDANCE[n].name);
        return;
    }
    g_cfg.advisor_guidance = n; config_save();
    const advisor_guidance_def *g = advisor_guidance();
    printf(C_GREEN "✓ advisor guidance: %s" C_RESET C_DIM " — %s" C_RESET "\n", g->name, g->desc);
    if (!g_cfg.advisor) printf(C_DIM "  no advisor is set yet: /advisor MODEL" C_RESET "\n");
    else if (cJSON_GetArraySize(g_messages) > 0) printf(C_DIM "  the agent's instructions say how to use it, so its next reply reads the conversation again" C_RESET "\n");
}

static void cmd_advisor_guidance(const char *v) {
    if (*v) {
        int n = advisor_guidance_parse(v);
        if (n < 0) { printf("usage: /advisor guidance light|normal|strong|max   (how much the agent leans on the advisor)\n"); return; }
        guidance_set(n);
        return;
    }
    if (!g_cfg.interactive || g_while_busy) {
        printf("advisor guidance: " C_BOLD "%s" C_RESET C_DIM " — %s" C_RESET "\n", advisor_guidance()->name, advisor_guidance()->desc);
        for (int i = 0; i < GUIDANCE_COUNT; i++) printf(C_DIM "  %-7s %s" C_RESET "\n", ADVISOR_GUIDANCE[i].name, ADVISOR_GUIDANCE[i].desc);
        return;
    }
    const char *names[GUIDANCE_COUNT], *descs[GUIDANCE_COUNT]; char dbuf[GUIDANCE_COUNT][200];
    for (int i = 0; i < GUIDANCE_COUNT; i++) {
        names[i] = ADVISOR_GUIDANCE[i].name;
        snprintf(dbuf[i], sizeof dbuf[i], "%s%s", ADVISOR_GUIDANCE[i].desc, i == g_cfg.advisor_guidance ? " · current" : "");
        descs[i] = dbuf[i];
    }
    int r = term_select("Advisor guidance: how much the agent leans on it", names, descs, GUIDANCE_COUNT, g_cfg.advisor_guidance);
    if (r < 0) printf(C_DIM "advisor guidance unchanged: %s" C_RESET "\n", advisor_guidance()->name);
    else guidance_set(r);
}

static void cmd_advisor(const char *arg) {
    if (arg && (!strcmp(arg, "off") || !strcmp(arg, "none"))) { advisor_set(NULL); return; }
    if (arg && !strncmp(arg, "guidance", 8) && (arg[8] == ' ' || !arg[8])) {
        const char *v = arg + 8; while (*v == ' ') v++;
        cmd_advisor_guidance(v);
        return;
    }
    if (arg && !strncmp(arg, "effort", 6) && (arg[6] == ' ' || !arg[6])) {
        const char *v = arg + 6; while (*v == ' ') v++;
        if (!g_cfg.advisor) { printf(C_DIM "no advisor is set — /advisor MODEL first" C_RESET "\n"); return; }
        if (g_while_busy && !(g_advisor_info_for && !strcmp(g_advisor_info_for, g_cfg.advisor))) {
            printf(C_DIM "the advisor's levels are not read while the model works — set them when it is idle" C_RESET "\n"); return;
        }
        if (!advisor_info()) {
            if (provider_find(g_cfg.advisor, NULL)) printf(C_RED "✗ %s" C_RESET "\n", g_advisor_err);
            else printf(C_RED "✗ the server does not know the advisor model '%s'" C_RESET "\n", g_cfg.advisor);
            return;
        }
        effort_command("advisor ", g_cfg.advisor, &g_advisor_info, *v ? v : NULL);
        if (*v && advisor_is_main()) printf(C_DIM "  (the advisor is the model doing the work: this is its effort there too)" C_RESET "\n");
        return;
    }
    if (arg && !strncmp(arg, "window", 6) && (arg[6] == ' ' || !arg[6])) {
        const char *v = arg + 6; while (*v == ' ') v++;
        char *end = NULL; double m = !strcmp(v, "off") ? 0 : strtod(v, &end);
        if (!*v) { printf("advisor window: %s" C_DIM " — a request's count of consultations (%d at guidance %s) starts again after that long; /advisor window MINUTES|off" C_RESET "\n",
                          g_cfg.advisor_window > 0 ? fmt_window() : "off", advisor_guidance()->uses, advisor_guidance()->name); return; }
        if ((end && (*end || end == v)) || m < 0 || m > 24 * 60) { printf("usage: /advisor window MINUTES|off   (after that long a request gets its consultations again; default %d, off = never)\n", ADVISOR_WINDOW_DEFAULT / 60); return; }
        g_cfg.advisor_window = (int)(m * 60 + 0.5); config_save();
        if (g_cfg.advisor_window > 0) printf(C_GREEN "✓ advisor window: %s" C_RESET C_DIM " — up to %d consultation%s per request, and again every %s of one that runs long" C_RESET "\n",
                                             fmt_window(), advisor_guidance()->uses, advisor_guidance()->uses == 1 ? "" : "s", fmt_window());
        else printf(C_GREEN "✓ advisor window: off" C_RESET C_DIM " — %d consultation%s per request, however long it runs" C_RESET "\n", advisor_guidance()->uses, advisor_guidance()->uses == 1 ? "" : "s");
        if (g_cfg.advisor && cJSON_GetArraySize(g_messages) > 0) printf(C_DIM "  the agent's instructions say so, so its next reply reads the conversation again" C_RESET "\n");
        return;
    }
    if (arg && !strncmp(arg, "ctx", 3) && (arg[3] == ' ' || !arg[3])) {
        const char *v = arg + 3; while (*v == ' ') v++;
        int n = !strcmp(v, "auto") ? 0 : parse_ctx_arg(v);
        if (!*v || n < 0 || (n > 0 && n < ADVISOR_CTX_MIN)) { printf("usage: /advisor ctx N|Nk|auto   (the context window of a consultation, %s at least; auto = the main window, at most %s)\n", fmt_ctx(ADVISOR_CTX_MIN), fmt_ctx(ADVISOR_CTX_AUTO)); return; }
        g_cfg.advisor_ctx = n; config_save();
        advisor_plan p; advisor_plan_now(&p);
        printf(C_GREEN "✓ advisor context window: %s%s" C_RESET "%s\n", n ? fmt_ctx(n) : "auto", n && p.send_ctx > 0 && p.send_ctx != n ? " (the model takes less)" : "",
               p.same ? C_DIM " — not used while the advisor is the main model: it keeps the main window" C_RESET : p.cloud ? C_DIM " — not used with a model that is not on this machine: it has its own" C_RESET : "");
        return;
    }
    if (arg) { advisor_set(arg); return; }
    if (!g_cfg.interactive || g_while_busy) { advisor_report(); return; }
    cJSON *list = ollama_list_models();
    if (!list) { advisor_report(); return; }
    int n = cJSON_GetArraySize(list);
    const char **names = xmalloc(sizeof(char*) * (size_t)(n + 1)), **descs = xmalloc(sizeof(char*) * (size_t)(n + 1));
    char **own = xmalloc(sizeof(char*) * (size_t)(n + 1));
    names[0] = "No advisor"; descs[0] = g_cfg.advisor ? "the agent works by itself" : "the agent works by itself · current"; own[0] = NULL;
    int current = 0;
    for (int i = 0; i < n; i++) {
        cJSON *mo = cJSON_GetArrayItem(list, i);
        names[i + 1] = cJSON_GetObjectItemCaseSensitive(mo, "name")->valuestring;
        bool cur = g_cfg.advisor && !strcmp(names[i + 1], g_cfg.advisor);
        sbuf d; sb_init(&d);
        sb_printf(&d, "%-7s%s%s%s", cJSON_GetObjectItemCaseSensitive(mo, "size")->valuestring,
                  cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mo, "thinking")) ? " · thinking" : "",
                  g_cfg.model && !strcmp(names[i + 1], g_cfg.model) ? " · the model doing the work" : "", cur ? " · current" : "");
        own[i + 1] = sb_detach(&d); descs[i + 1] = own[i + 1];
        if (cur) current = i + 1;
    }
    advisor_howto();
    int r = term_select("Select advisor", names, descs, n + 1, current);
    if (r < 0) printf(C_DIM "advisor unchanged: %s" C_RESET "\n", g_cfg.advisor ? g_cfg.advisor : "none");
    else if (r == 0) advisor_set(NULL);
    else advisor_set(names[r]);
    for (int i = 1; i <= n; i++) free(own[i]);
    free(own); free(names); free(descs); cJSON_Delete(list);
}

static char *expand_mentions(const char *input);

/* ---------- queued messages ---------- */
/* Echo a message the user queued while the model was busy, the way it would have
 * looked had they typed it at the prompt. */
static void echo_queued(const char *text) {
    char *b = term_user_band(C_BOLD C_ORANGE "› " C_RESET, text);   /* a user message: on the user's ground, text as typed (not dim) */
    fputs(b, stdout);
    free(b);
}

/* Deliver plain messages queued while the model was working (Claude Code style): they are
 * appended as user messages so the model sees them on its next call. Slash / ! lines stay
 * queued for the REPL to run after the turn, but they do not hold the messages behind them
 * back — those are what the model is waiting for. A queued message never stops work in flight;
 * one that arrives mid-turn goes in before the turn's next model call, the rest wait for the REPL.
 * Returns the number of messages injected. */
static int inject_queued(void) {
    int n = 0;
    char *text;
    while ((text = term_queue_pop_plain())) {
        echo_queued(text);
        char *msg = expand_mentions(text);
        typed_note(text, msg);
        add_message("user", msg);
        free(msg); free(text); n++;
    }
    if (n) { g_advisor_uses = 0; g_advisor_since = time(NULL); term_status_refresh(); printf("\n"); }   /* the user spoke: the advisor's budget is new, as it would be at the prompt */
    return n;
}

/* ---------- the agent loop ---------- */
static bool maybe_auto_compact(bool resuming, const char *request);
static bool compact_conversation(bool resuming, const char *request);
static bool cmd_compact(void) { return compact_conversation(false, NULL); }

/* The request the current turn is working on, as the user wrote it. A compaction in the
 * middle of a turn summarises it away, so it is handed back verbatim — a summary of the
 * work is not the same as the ask itself. NULL when there is nothing to point at (a second
 * compaction within one turn: the first one already reset g_prev_request_first). */
static const char *pending_request(void) {
    if (g_prev_request_first < 0 || g_prev_request_first >= cJSON_GetArraySize(g_messages)) return NULL;
    cJSON *m = cJSON_GetArrayItem(g_messages, g_prev_request_first);
    cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
    if (!cJSON_IsString(r) || strcmp(r->valuestring, "user") || !cJSON_IsString(c) || !c->valuestring[0]) return NULL;
    return c->valuestring;
}

/* Keep the next call inside the window. A whole tool round lands between two calls, so this
 * works on the estimate rather than on what was measured last: elide the tool results older
 * than the round the model is about to reason about (free), and compact only if that was not
 * enough. `keep_from` is the first message of that round. Returns true when it compacted —
 * the conversation was replaced, so every index into it is stale. */
static bool shrink_context(int keep_from) {
    elide_old_tool_results(keep_from);
    return maybe_auto_compact(true, pending_request());
}

/* An assistant turn with neither text nor a tool call: the model generated tokens the server
 * delivered as nothing at all. Ollama's tool-call parser swallowing a call the model rendered
 * badly looks exactly like this, and it rarely repeats. */
static bool reply_is_empty(cJSON *reply) {
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
    if (calls && cJSON_GetArraySize(calls) > 0) return false;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(reply, "content");
    return !cJSON_IsString(c) || !c->valuestring[0];
}

/* Returns true if the user interrupted the turn. */
static bool g_turn_failed = false;   /* the last turn ended on an error (no reply from the model): -p exits 1 */
static bool run_turn_body(void);
/* a request is over: the stop hooks run, once the turn has ended however it ended */
static bool run_turn(void) {
    bool aborted = run_turn_body();
    tools_hook_event("stop");
    return aborted;
}
static bool run_turn_body(void) {
    g_turn_failed = false;
    if (!model_set()) {   /* no model yet: say why, and take the message back out of the conversation */
        g_turn_failed = true;
        printf(C_YELLOW "%s" C_RESET "\n", g_model_err);
        size_t n = cJSON_GetArraySize(g_messages);
        cJSON *last = n ? cJSON_GetArrayItem(g_messages, (int)n - 1) : NULL;
        cJSON *role = last ? cJSON_GetObjectItemCaseSensitive(last, "role") : NULL;
        if (role && cJSON_IsString(role) && !strcmp(role->valuestring, "user")) cJSON_DeleteItemFromArray(g_messages, (int)n - 1);
        return false;
    }
    int iters = 0, shrinks = 0, empties = 0;
    int round_first = -1;   /* where this round's tool results start (-1 before the first round) */
    g_session.turns++;
    for (;;) {
        if (shrink_context(round_first >= 0 ? round_first : cJSON_GetArraySize(g_messages))) round_first = -1;
        cJSON *msgs = messages_with_system();
        cJSON *tools = tools_for_mode();
        size_t sent = prompt_bytes();
        chat_stats st; bool aborted = false;
        /* think=auto: let a thinking model think about the request once, not again after every
         * tool result — that is where most of a task's wall time goes on local models
         * (think_decide() knows when that is not an option) */
        ollama_call.followup = iters > 0;
        cJSON *reply = model_chat(msgs, tools, &st, &aborted);
        ollama_call_reset();
        if (tools && tools != g_tools) cJSON_Delete(tools);
        cJSON_Delete(msgs);
        if (st.prompt_tokens) { g_session.last_prompt_tokens = st.prompt_tokens; ctx_measured(st.prompt_tokens, sent); }
        account(&st);
        term_status_refresh();
        if (!reply) {
            /* The prompt did not fit and Ollama dropped messages from the front until the user's
             * own request was gone (see "how full the context is"): the estimate was too low, so
             * shrink for real and retry rather than losing the turn — first by eliding every tool
             * result, then, when there is nothing left to elide, by compacting. */
            if (!aborted && shrinks < 2 && ollama_error[0] && (strstr(ollama_error, "no user query") || strstr(ollama_error, "prompt is too long") || strstr(ollama_error, "too many tokens") || strstr(ollama_error, "exceed context"))) {
                shrinks++;
                printf(C_YELLOW "⚠ the conversation no longer fits in %s of context — shrinking it and retrying" C_RESET "\n", fmt_ctx(g_cfg.num_ctx));
                if (elide_tool_results(g_messages, cJSON_GetArraySize(g_messages))
                    || compact_conversation(true, pending_request())) { round_first = -1; continue; }
            }
            /* On failure, keep the conversation as is; the user can retry. */
            g_turn_failed = !aborted;
            return aborted;
        }
        cJSON_AddItemToArray(g_messages, reply);
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
        if (aborted || !calls || cJSON_GetArraySize(calls) == 0) {
            /* Nothing to show and nothing to run would end the request in silence, and the user
             * reads whatever was printed last — the stats line, the placement warning — as the
             * reason it stopped. Say it, and ask once more. The empty message stays out of the
             * conversation: it says nothing the model can build on, and every later call would
             * have to carry it — an assistant turn the chat template still has to render. */
            if (!aborted && reply_is_empty(reply)) {
                cJSON_DeleteItemFromArray(g_messages, cJSON_GetArraySize(g_messages) - 1);
                print_stats(&st);
                if (++empties < 2) {
                    printf(C_YELLOW "⚠ empty reply: %d token%s generated, delivered as neither text nor a tool call — asking again" C_RESET "\n",
                           st.eval_tokens, st.eval_tokens == 1 ? "" : "s");
                    continue;
                }
                printf(C_YELLOW "⚠ empty reply again — stopping here" C_RESET "\n");
                g_turn_failed = true;
                printf(C_DIM "  nothing was added to the conversation; send another message to continue, or /model to try a different one" C_RESET "\n");
                return false;
            }
            if (aborted && calls && cJSON_GetArraySize(calls) > 0) answer_calls_unrun(calls, TOOL_NOT_RUN);
            print_stats(&st);
            /* guidance strong and up: the advisor looks at a request that changed a lot before it
             * ends (any change at max); what it finds goes to the agent, which gets another round for it */
            if (!aborted && advisor_review_due() && advisor_review(reply)) { round_first = cJSON_GetArraySize(g_messages) - 1; inject_queued(); continue; }
            if (!aborted) { check_model_placement(false); maybe_auto_compact(false, NULL); }
            return aborted;
        }
        if (++iters > g_cfg.max_iters) {
            printf(C_YELLOW "⚠ stopped after %d tool rounds (loop guard). Send a message to continue." C_RESET "\n", g_cfg.max_iters);
            /* the reply that asked for these calls is already in the conversation: answer them, or the next request is refused */
            answer_calls_unrun(calls, "not run: this request reached its tool-round limit (/max_iters) before the call ran. "
                                      "The user's next message follows; call the tool again if it is still needed.");
            return false;
        }
        cJSON *call;
        round_first = cJSON_GetArraySize(g_messages);
        bool failed = false;   /* a call of this round failed (see advisor_step_in) */
        cJSON_ArrayForEach(call, calls) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            const char *name = cJSON_IsString(nm) ? nm->valuestring : "?";
            cJSON *parsed = NULL;
            if (cJSON_IsString(args)) { parsed = cJSON_Parse(args->valuestring); args = parsed; }
            char summ[160]; tool_arg_summary(name, args, summ, sizeof summ);
            /* a bash call is one line, grey: its header goes with its result (the fold's head), after the approval;
             * short of that the usual header comes first. The summary is kept to half the width, for the output's count. */
            bool bashline = !strcmp(name, "bash");
            char lead[480] = "";
            if (bashline) {
                char head[300];
                int room = term_width() / 2 - 10; if (room < 20) room = 20;
                snprintf(head, sizeof head, "%s", summ);
                if ((int)strlen(head) > room) { size_t cut = (size_t)room - 1; while (cut > 0 && ((unsigned char)head[cut] & 0xC0) == 0x80) cut--; snprintf(head + cut, sizeof head - cut, "…"); }
                snprintf(lead, sizeof lead, C_GRAY "● bash(%s)" C_RESET "  ", head);
            } else {
                printf(C_GREEN "●" C_RESET " " C_BOLD "%s" C_RESET, name);
                if (summ[0]) printf(C_DIM "(%s)" C_RESET, summ);
                printf("\n");
            }
            sbuf out; sb_init(&out);
            if (advisor_check_due(name)) {   /* guidance max: the advisor looks at the request's first change before it is made */
                int v = advisor_check(name, args, &out);
                if (v != ADVICE_NONE) {
                    printf("  ⎿  " C_YELLOW "held back — the advisor sees a problem with it" C_RESET "\n");
                    cJSON_AddItemToArray(g_messages, tool_result_message(name, tool_call_id(call), out.data));
                    sb_free(&out);
                    if (parsed) cJSON_Delete(parsed);
                    continue;
                }
            }
            g_session.tool_calls++;
            g_advisor_said = false;
            tool_status ts = tools_execute(name, args, &out);
            const char *res = out.data ? out.data : "";
            if (g_advisor_said) {}   /* run_advisor() has said what came of it: the advice is not shown twice */
            else if (ts == TOOL_DENIED) printf("%s" C_RED "denied" C_RESET "\n", bashline ? lead : "  ⎿  ");
            else if (ts == TOOL_ERROR) printf("%s" C_RED "%s" C_RESET "\n", bashline ? lead : "  ⎿  ", res);
            else if (!strcmp(name, "todo_write") && !bashline) print_task_lines(res);
            else print_result_folded(res, (!strcmp(name, "read_file") || !strcmp(name, "web_fetch")) ? 3 : 8, bashline ? lead : NULL);
            if (tool_result_failed(name, res, ts == TOOL_ERROR)) failed = true;
            cJSON_AddItemToArray(g_messages, tool_result_message(name, tool_call_id(call), res));
            sb_free(&out);
            if (parsed) cJSON_Delete(parsed);
        }
        g_fail_rounds = failed ? g_fail_rounds + 1 : 0;
        if (advisor_step_in_due()) advisor_step_in(reply);
        printf("\n");
        inject_queued();   /* messages the user queued meanwhile go in before the next model call (they never stop a call) */
    }
}

/* ---------- @file mentions ---------- */
static char *expand_mentions(const char *input) {
    sbuf extra; sb_init(&extra);
    const char *p = input;
    while (*p) {
        if (*p == '@' && (p == input || isspace((unsigned char)p[-1])) && p[1] && !isspace((unsigned char)p[1])) {
            const char *s = p + 1, *e = s;
            while (*e && !isspace((unsigned char)*e)) e++;
            while (e > s && strchr(",.;:)!?", e[-1])) e--;
            char *path = xstrndup(s, (size_t)(e - s));
            char *fp = expand_home(path);
            if (!is_file(fp) && !is_dir(fp)) {   /* not a path from here: the one file by that name, when there is just one */
                int hits = 0; sbuf names; sb_init(&names);
                char *found = file_find_named(path, &hits, &names);
                if (found) { free(fp); free(path); path = found; fp = xstrdup(found); }
                else if (hits > 1) printf(C_DIM "  (@%s could be %s%s — give more of the path)" C_RESET "\n", path, names.data, hits > 5 ? ", …" : "");
                sb_free(&names);
            }
            if (is_file(fp)) {
                size_t n; char *d = read_whole_file(fp, &n, 128 * 1024);
                if (d) {
                    sb_printf(&extra, "\n\n<file path=\"%s\">\n%s%s\n</file>", path, d, n >= 128 * 1024 ? "\n[truncated]" : "");
                    printf(C_DIM "  (attached %s, %zu bytes)" C_RESET "\n", path, n);
                    free(d);
                }
            } else if (is_dir(fp)) {
                cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", fp);
                sbuf o; sb_init(&o); tools_execute("list_dir", a, &o);
                sb_printf(&extra, "\n\n<directory path=\"%s\">\n%s</directory>", path, o.data ? o.data : "");
                printf(C_DIM "  (attached listing of %s)" C_RESET "\n", path);
                sb_free(&o); cJSON_Delete(a);
            }
            free(fp); free(path);
            p = e;
            continue;
        }
        p++;
    }
    if (!extra.len) { sb_free(&extra); return xstrdup(input); }
    sbuf r; sb_init(&r); sb_puts(&r, input); sb_append(&r, extra.data, extra.len);
    sb_free(&extra);
    return sb_detach(&r);
}

/* ---------- /orchestrate: a strong model plans and reviews, the local one does the work ----------
 * The advisor turned round. /advisor lends the agent a stronger model's opinion; here the
 * stronger model (the same one: g_cfg.advisor — hosted, cloud or a bigger local one) is in
 * charge and the model doing the work is its hands. Three kinds of call, one after another:
 *   plan    the request, the project's instructions, a listing of the directory and the
 *           conversation so far go to the orchestrator, which answers with tasks (orch_parse_plan());
 *   work    each task runs as an agent of its own on the main model (as a sub-agent does, but
 *           with write_file/edit_file — through confirm(), so the permission mode holds) with a
 *           fresh context: the task, and the reports of the ones that were done before it;
 *   review  the orchestrator is shown the task, the worker's report and the diff of what it
 *           wrote (tools_checkpoint_diff()) and accepts it or sends it back with what to do —
 *           ORCH_ATTEMPTS times at most; a task that is still not accepted takes the tasks that
 *           wait for it with it.
 * The expensive model never sees a worker's transcript, only plans, reports and diffs — that
 * is the point: its tokens are few, the local model's are free. It has no tools (provider.c
 * asks one question and reads one answer), so it plans from what it is shown; a plan may begin
 * with a task that explores, whose report the later workers get.
 *
 * Tasks that do not wait for each other run at the same time, up to /workers of them — and what
 * runs at the same time is only what takes the time: the model calls, each a job on a thread of
 * its own (http_job_*), asked for in one piece (ollama_chat_request/_reply). Everything else is
 * done here, on the main thread, one thing at a time: a worker's tools, the confirmations, what
 * is printed, the accounting, the reviews. So the workers are state machines that orch_schedule()
 * steps whenever one's call has come back, and no global of this program needs a lock. Which
 * tasks may overlap is the planner's to say ("AFTER: none", "AFTER: 1, 2"); a plan that says
 * nothing runs in order. Whether the overlap buys time is the server's: an Ollama with one slot
 * (OLLAMA_NUM_PARALLEL) queues the calls. In the conversation the whole run is two messages —
 * the request as the user's, and what came of each task as the agent's reply — so what the
 * user says next follows it like any other turn, and /rewind undoes it as one request. */
#define ORCH_ATTEMPTS 3        /* a task is tried, and sent back at most twice */
#define ORCH_REPLY_MIN 2048    /* tokens a plan may take, whatever the guidance allows a piece of advice */
static bool g_orch_no_ask = false;   /* "run these tasks?" was answered with "don't ask again" in this session */
static bool g_plan_no_ask = false;   /* "build this plan?" was answered with "don't ask again" in this session (/plan only) */

/* The roles the planner may name: every skill is one (its instructions and its model are the worker's). */
static void orch_roles(sbuf *b) {
    int n = skills_count();
    if (!n) { sb_puts(b, "No roles are defined: leave out ROLE:.\n"); return; }
    sb_puts(b, "Roles you may name with ROLE: (a role is a skill: its instructions and model are the worker's):\n");
    for (int i = 0; i < n; i++) sb_printf(b, "- %s: %.120s\n", skill_get(i)->name, skill_get(i)->desc);
}

static void orch_system(sbuf *b, int workers, bool layered) {
    sbuf roles; sb_init(&roles); orch_roles(&roles);
    sb_printf(b,
        "You are the orchestrator of Corbie Nest (corbienest), a coding agent in the user's terminal. You do not do the work yourself. "
        "You split the user's request into tasks for worker agents, and afterwards you review what each one did.\n\n"
        "The workers are a small local model. They run in the user's project, each with a fresh context that holds only "
        "your task text and the short reports of the tasks that were finished before it started. They have tools — read_file, list_dir, grep, bash, write_file, edit_file — "
        "and you have none: you see what you are shown here and nothing else, so never claim to have read a file or run anything.\n\n"
        "When you are asked to PLAN, answer with the plan and nothing else, in exactly this shape:\n\n"
        "TASK 1: <short title>\n<what to do>\nDONE WHEN: <how to tell it is done>\n\nTASK 2: <short title>\n...\n\n"
        "Under a title a task may have two lines more: \"ROLE: <name>\", the role from the list below that fits the task (the worker then runs with that role's instructions "
        "and model), and \"OWNS: <paths>\", the files or directories the task may write, comma-separated. A plan may open with a \"CONTRACT:\" block, before its first task, "
        "that states what the tasks share (names, signatures, fields, paths, packages), written once: every worker is shown it.\n"
        "- As few tasks as the request needs, at most %d. A small request is one task.\n"
        "- Every task stands by itself: name the files, the functions, the commands to run and what must not change. A worker knows nothing you do not tell it, and cannot ask.\n"
        "- Order them so that each builds on finished work; no two tasks change the same lines.\n"
        "- An OWNS: list is a promise: the task writes only there. Tasks that may run at the same time must not own the same paths; a file several tasks must change belongs to one later task.\n"
        "%s%s"
        "- Where you would need to read the code first, make the first task an exploration that reports what it found: the later workers get that report.\n"
        "- Where the project has a build or tests, a task says to run them. When the request changes code, the last task builds the project, runs its tests and fixes what fails.\n"
        "%s\n"
        "When you are asked to REVIEW a task, you are shown the task, the worker's report and the diff of the files it wrote. "
        "If the task is done and correct, answer with " ADVISOR_REVIEW_OK " alone. Otherwise say exactly what is wrong or missing and what to do about it, "
        "briefly: your answer goes to the worker as its next instruction. Do not ask for more than the task asked for.", ORCH_MAX_TASKS,
        workers > 1 ? "- The tasks run one after another unless you say otherwise. Tasks that do not need each other's result AND write different files may run at the same time: "
                      "give such a task a line \"AFTER: none\" (it can start at once) or \"AFTER: 1, 2\" (the tasks that must be finished first), right under its title. "
                      "A task without that line waits for the task before it. When in doubt, leave the line out.\n" : "",
        layered ? "- The request is built in LAYERS, in order. Under a title, a line \"LAYER: n\" (n = 1, 2, 3, ...) says which layer the task belongs to. "
                  "Layer 1 is the foundation the rest stands on (the build file, the configuration, the core types); a later layer uses only what the layers before it made. "
                  "The project is built after each layer, so every layer must leave it compiling. Tasks of one layer may run at the same time when they own different paths and say \"AFTER: none\".\n"
                  "- The CONTRACT is what makes the layers fit: it names the packages, classes, method signatures, fields and paths that the layers share, exactly.\n" : "",
        roles.data);
    sb_free(&roles);
}

/* One call of the orchestrating model, with its line in the transcript. The answer, malloc'd,
 * for ADVICE_GIVEN; NULL otherwise, and the line under it has said why. */
static char *orch_ask(const char *what, const char *user, chat_stats *st, int *how, bool layered) {
    bool known = advisor_info();
    advisor_plan plan; advisor_plan_now(&plan);
    sbuf sys; sb_init(&sys); orch_system(&sys, g_cfg.workers > 0 ? g_cfg.workers : ORCH_WORKERS_DEFAULT, layered);
    printf("  " C_CYAN "⤷ orchestrator" C_RESET " " C_BOLD "%s" C_RESET C_DIM " · %s · %zu KB · esc stops the run" C_RESET "\n", g_cfg.advisor, what, (strlen(user) + 1023) / 1024);
    char label[96]; snprintf(label, sizeof label, "orchestrator · %.70s", g_cfg.advisor);
    advisor_reply ar;
    char *answer = advisor_call(sys.data, user, &plan, known, plan.reply * 2 > ORCH_REPLY_MIN ? plan.reply * 2 : ORCH_REPLY_MIN, label, &ar);
    sb_free(&sys);
    *st = ar.st;
    const char *text = strip_think_block(answer ? answer : "");
    char *out = NULL;
    if (ar.aborted) { printf("    " C_RED "⎿ interrupted" C_RESET "\n"); *how = ADVICE_SKIPPED; }
    else if (!answer) {
        if (ar.signin) printf("    " C_YELLOW "⎿ a cloud model needs an account: run \"ollama signin\" in a shell, then ask again" C_RESET "\n");
        else printf("    " C_RED "⎿ no answer: %s" C_RESET "\n", ar.err);
        *how = ADVICE_NONE;
    } else if (!*text) {
        printf("    " C_RED "⎿ no answer" C_RESET C_DIM "%s" C_RESET "\n", !strcmp(ar.st.done_reason, "length") ? " — it ran out of tokens while still thinking: lower /advisor effort" : "");
        *how = ADVICE_NONE;
    } else { g_session.advisor_calls++; out = xstrdup(text); *how = ADVICE_GIVEN; }
    free(answer);
    return out;
}

/* what the orchestrator is shown of a call it made: "2.1k tokens · 9s · ≈ $0.03" */
static void orch_spent(const chat_stats *st, char *out, size_t n) {
    char tk[32], cost[48]; fmt_tokens(st->prompt_tokens + st->eval_tokens, tk, sizeof tk); cost_note(g_cfg.advisor, st, cost, sizeof cost);
    snprintf(out, n, "%s tokens · %.0fs%s", tk, st->total_seconds, cost);
}

/* /goal: what a round's candidates are told beyond a /orchestrate run */
typedef struct {
    const goal_spec *spec;       /* the metric, its direction, the goal */
    double base;                 /* the best score so far: a candidate must beat it */
    const char *prior;           /* what the other candidates of the last round did (NULL in the first) */
} goal_ctx;

/* A task of the run and the worker doing it. */
enum { T_WAITING, T_RUNNING, T_ACCEPTED, T_UNREVIEWED, T_REJECTED, T_FAILED, T_STOPPED, T_SKIPPED };
typedef struct {
    int state;
    const char *status;          /* what the report says came of it */
    cJSON *msgs; size_t sp_bytes;/* its conversation, and the size of the system prompt in it */
    http_job *job;               /* the model call it is waiting for (T_RUNNING) */
    int attempt, rounds, round_first;
    sbuf out, objection;         /* its last report, and what the orchestrator last held against it */
    const char *model;           /* the model it runs on (NULL = g_cfg.model); /goal's candidates differ */
    char *role_model;            /* the model its role names, a copy: the skills may be reloaded during the run */
    model_info info; bool has_info;   /* that model's capabilities, when it is a local model other than the main one */
    char *root;                  /* /goal: the worktree it works in */
    char commit[64];             /* /goal: the commit of its work, which is what it is kept by */
    double metric; bool scored;  /* /goal: what the metric said of it */
    int verdict; char why[200];  /* /goal: its GOAL: line */
} orch_worker;
typedef struct {
    const char *request;         /* as the user typed it */
    orch_task *tasks; orch_worker *w; int nt;
    cJSON *tools;                /* what a worker may call */
    int first; size_t budget;    /* the request's checkpoint turn; bytes a review may take */
    bool aborted;                /* the user stopped the run */
    const goal_ctx *goal;        /* /goal: candidates, not tasks — no reviews, a metric instead (NULL for /orchestrate) */
    char *contract;              /* the plan's CONTRACT: block: what the tasks share, shown to every worker (NULL: none) */
    int upto;                    /* /plan: the layer the run may start up to (its tasks of later layers wait); 0 = no layers */
} orch_run;
#define GOAL_METRIC_TIMEOUT 900
#define GOAL_GIT "GIT_AUTHOR_NAME=corbienest GIT_AUTHOR_EMAIL=corbienest@localhost GIT_COMMITTER_NAME=corbienest GIT_COMMITTER_EMAIL=corbienest@localhost "
static const char *worker_model(const orch_worker *w) { return w->model ? w->model : g_cfg.model; }
static bool worker_remote(const orch_worker *w) { return provider_find(worker_model(w), NULL) != NULL; }
static bool task_ok(const orch_worker *w) { return w->state == T_ACCEPTED || w->state == T_UNREVIEWED; }
static void task_end(orch_worker *w, int state, const char *status) { w->state = state; w->status = status; }

/* send the worker's conversation to the model: the call is a job, and the worker waits for it */
static void worker_call(orch_run *r, int i) {
    orch_worker *w = &r->w[i];
    /* its conversation is its own and nothing in it is measured: kept inside the window on bytes, as a sub-agent's is */
    if (w->round_first > 0 && g_cfg.num_ctx > 0 &&
        (double)(w->sp_bytes + tools_bytes() + conv_bytes(w->msgs)) / g_bytes_per_token > g_cfg.num_ctx * (ELIDE_PCT / 100.0))
        elide_tool_results(w->msgs, w->round_first);
    ollama_call.followup = w->rounds > 0 || w->attempt > 1;   /* think=auto: think about the task once, not after every tool result */
    if (w->model) { ollama_call.model = w->model; ollama_call.info = w->has_info ? &w->info : NULL; }
    char *body, why[512] = "";
    if (worker_remote(w)) {
        const char *hdrs[4]; sbuf keep;
        const provider_def *p = provider_find(worker_model(w), NULL);
        body = provider_turn_request(w->msgs, r->tools);   /* reads ollama_call; the headers are built here, the thread reads nothing */
        bool ok = body && p && provider_call_headers(worker_model(w), hdrs, &keep, why, sizeof why);
        w->job = ok ? http_job_start(provider_base_url(p), provider_chat_path(p), body, hdrs) : NULL;
        if (ok) sb_free(&keep);
    } else {
        body = ollama_chat_request(w->msgs, r->tools);
        w->job = http_job_start(g_cfg.host, "/api/chat", body, NULL);
    }
    ollama_call_reset();
    free(body);
    if (w->job) w->state = T_RUNNING;
    else { printf("    " C_RED "[%d] ⎿ %s" C_RESET "\n", i + 1, why[0] ? why : "the model call could not be started"); task_end(w, T_FAILED, "failed: the model call could not be started"); }
}

/* /goal: a candidate is one of several at the same goal, each in a copy of the project of its own */
static void goal_system(sbuf *sp, const orch_run *r, const orch_worker *w) {
    const goal_ctx *g = r->goal;
    sb_printf(sp, "You are a candidate agent of Corbie Nest (corbienest), a coding agent in the user's terminal. "
                  "Several candidates work at the same goal at the same time, each in a copy of the project of its own, some of them on other models; "
                  "the one whose result scores best is kept and the others are thrown away.\nWorking directory (your copy): %s\n", w->root);
    sb_printf(sp, "The score is what the command `%s` prints in your copy once you are done: its last line is a number, and %s is better. "
                  "The best score so far is %.6g.\n", g->spec->metric, g->spec->lower ? "lower" : "higher", g->base);
    if (g->prior && *g->prior) sb_printf(sp, "\n%s\n", g->prior);
    sb_puts(sp, "\nWork toward the goal in your copy: change what it needs, measure with the command yourself where you can, and try an approach that differs from the others'. "
                "Do not ask questions; make a reasonable assumption and say which. Your write_file and edit_file refuse paths outside your copy, and bash runs in it, "
                "but a shell command can still name a path outside it: do not.\n"
                "End with ONE final message: what you changed, the score, and on the last line exactly one of `GOAL: REACHED` (you believe the goal is met), "
                "`GOAL: IMPOSSIBLE <why>` (you are sure it cannot be met), or neither (you are not sure yet).\n");
}

/* The model a role names, for the worker that does the role's task. A local model must be on the server (its
 * capabilities are read now, as /goal reads them), a hosted one must have its key. Failing that the worker keeps
 * the model in use, and the line says why. */
static void worker_role_model(orch_worker *w, const skill_t *role) {
    if (provider_find(role->model, NULL)) {
        const char *hd[4]; sbuf keep; char why[256]; sb_init(&keep);
        bool ok = provider_call_headers(role->model, hd, &keep, why, sizeof why);
        sb_free(&keep);
        if (!ok) { printf("  " C_YELLOW "role %s: %s — the model in use" C_RESET "\n", role->name, why); return; }
    } else if (ollama_model_show(role->model, &w->info) == 0) {
        w->has_info = true;
    } else {
        printf("  " C_YELLOW "role %s: ollama does not have %s — the model in use" C_RESET "\n", role->name, role->model);
        return;
    }
    w->role_model = xstrdup(role->model); w->model = w->role_model;
}

static void worker_begin(orch_run *r, int i) {
    orch_worker *w = &r->w[i];
    w->msgs = cJSON_CreateArray(); w->attempt = 1;
    sbuf sp; sb_init(&sp);
    if (r->goal) goal_system(&sp, r, w);
    else sb_printf(&sp, "You are a worker agent of Corbie Nest (corbienest), a coding agent in the user's terminal. An orchestrator split the user's request into tasks; "
                   "you do ONE of them, in the user's project, with your tools.\nWorking directory: %s\n"
                   "Do this task and only this task — not the others; some of them may be under way at this very moment, in files that are not yours to touch. "
                   "Read the code before you change it (batch independent reads and searches into one reply rather than one per turn), change no more than the task needs, "
                   "and check your work where you can (build it, run the test). Do not ask questions: make a reasonable assumption and say which.\n"
                   "End with ONE final message, your report: what you changed and in which files, what you checked and how, and anything left undone or that you are unsure of. "
                   "The orchestrator sees only that report and the diff of the files you wrote.", g_cwd);
    if (g_project_instructions) sb_puts(&sp, g_project_instructions);
    /* a role (a skill named by the plan) adds its instructions, and may name the model; one that is not there is the default worker */
    const skill_t *role = NULL;
    if (!r->goal && r->tasks[i].role) {
        role = skill_find(r->tasks[i].role);
        if (!role) printf("  " C_DIM "role %s is not a skill here: the default worker" C_RESET "\n", r->tasks[i].role);
        else {
            sb_printf(&sp, "\n\n# Your role: %s\n", role->name); sb_puts(&sp, role->body);
            if (role->model) worker_role_model(w, role);
        }
    }
    w->sp_bytes = sp.len;
    cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "system"); cJSON_AddStringToObject(m, "content", sp.data); cJSON_AddItemToArray(w->msgs, m);
    sb_free(&sp);
    sbuf u; sb_init(&u);
    sb_puts(&u, r->goal ? "# The goal (you are one of several candidates at it)\n" : "# The user's request (for context — you do one part of it)\n"); sb_put_cut(&u, r->request, 4096);
    bool any = false;
    for (int j = 0; !r->goal && j < r->nt; j++) {   /* what is finished as this one starts, in the plan's order */
        if (!task_ok(&r->w[j])) continue;
        if (!any) { sb_puts(&u, "\n\n# Done before you\n"); any = true; }
        sb_printf(&u, "## Task %d: %s — %s\n", j + 1, r->tasks[j].title, r->w[j].status); sb_put_cut(&u, r->w[j].out.data ? r->w[j].out.data : "", 2048); sb_puts(&u, "\n\n");
    }
    if (r->contract) { sb_puts(&u, "\n# The contract (what all the tasks share)\n"); sb_put_cut(&u, r->contract, 4096); sb_putc(&u, '\n'); }
    sb_printf(&u, "%s# Your task (%d of %d): %s\n%s", any || r->contract ? "" : "\n\n", i + 1, r->nt, r->tasks[i].title, r->tasks[i].body);
    m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", u.data); cJSON_AddItemToArray(w->msgs, m);
    sb_free(&u);
    printf("  " C_CYAN "⤷ worker %d/%d" C_RESET " " C_BOLD "%s" C_RESET "%s%s%s\n", i + 1, r->nt, r->tasks[i].title,
           role ? C_DIM "  · role " : "", role ? role->name : "", role ? C_RESET : "");
    worker_call(r, i);
}

/* the worker has reported: the orchestrator looks at it, and it is accepted, sent back or given up */
static void worker_review(orch_run *r, int i) {
    orch_worker *w = &r->w[i];
    sbuf rb, diff; sb_init(&rb); sb_init(&diff);
    int nfiles = tools_checkpoint_diff(r->first, i + 1, &diff);
    sb_puts(&rb, "# The user's request\n"); sb_put_cut(&rb, r->request, r->budget / 8);
    sb_printf(&rb, "\n\n# The task (%d of %d): %s\n", i + 1, r->nt, r->tasks[i].title); sb_put_cut(&rb, r->tasks[i].body, r->budget / 8);
    sb_puts(&rb, "\n\n# The worker's report\n"); sb_put_cut(&rb, w->out.data ? w->out.data : "", r->budget / 6);
    sb_printf(&rb, "\n\n# The files it wrote, as a diff (%d file%s; what a shell command changed is not in it)\n", nfiles, nfiles == 1 ? "" : "s");
    sb_put_cut(&rb, nfiles ? diff.data : "(it wrote no file)", r->budget / 2);
    sb_printf(&rb, "\n\n# REVIEW this task (attempt %d of %d)\nIs it done and correct? " ADVISOR_REVIEW_OK " alone if so; otherwise what the worker must do.", w->attempt, ORCH_ATTEMPTS);
    char what[64], spent[96]; snprintf(what, sizeof what, "reviews task %d/%d", i + 1, r->nt);
    chat_stats st; int how;
    char *verdict = orch_ask(what, rb.data, &st, &how, false);
    sb_free(&rb); sb_free(&diff);
    if (how == ADVICE_SKIPPED) { task_end(w, T_STOPPED, "done, but the run was stopped before its review"); r->aborted = true; }
    else if (how != ADVICE_GIVEN) task_end(w, T_UNREVIEWED, "done, not reviewed (the orchestrator could not be asked)");
    else if (advisor_approves(verdict)) {
        orch_spent(&st, spent, sizeof spent);
        printf("    " C_GREEN "⎿ accepted" C_RESET C_DIM " · %s" C_RESET "\n", spent);
        task_end(w, T_ACCEPTED, w->attempt == 1 ? "accepted" : "accepted after corrections");
    } else {
        orch_spent(&st, spent, sizeof spent);
        printf("    " C_YELLOW "⎿ sent back" C_RESET C_DIM " · %s:" C_RESET "\n", spent); print_result_preview(verdict, 8);
        sb_clear(&w->objection); sb_puts(&w->objection, verdict);
        if (w->attempt == ORCH_ATTEMPTS) task_end(w, T_REJECTED, "not accepted");
        else {
            sbuf fb; sb_init(&fb);
            sb_printf(&fb, "The orchestrator reviewed your work on this task and does not accept it yet. It says:\n\n%s\n\nDo that — or, where the files or a command's output show it is wrong, say so — then report again.", verdict);
            cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", fb.data); cJSON_AddItemToArray(w->msgs, m);
            sb_free(&fb);
            w->attempt++;
            printf("  " C_CYAN "⤷ worker %d/%d" C_RESET " " C_BOLD "%s" C_RESET C_DIM " · %s" C_RESET "\n", i + 1, r->nt, r->tasks[i].title, w->attempt == 2 ? "second attempt" : "last attempt");
            worker_call(r, i);
        }
    }
    free(verdict);
}

/* /goal: a candidate has reported. Its work is committed in its copy (the commit is what it is kept by), and the metric is run there. */
static void goal_finish(orch_run *r, int i) {
    orch_worker *w = &r->w[i];
    w->verdict = goal_verdict(w->out.data, w->why, sizeof w->why);
    sbuf o; sb_init(&o); bool intr = false;
    tools_shell_in(w->root, GOAL_GIT "git add -A >/dev/null 2>&1 && " GOAL_GIT "git commit -q --no-verify --allow-empty -m corbie-goal-candidate >/dev/null 2>&1; git rev-parse --verify -q HEAD 2>/dev/null",
                   120, &o, &intr, "committing a candidate", "committing");
    if (intr) { r->aborted = true; task_end(w, T_STOPPED, "stopped by the user before it was measured"); sb_free(&o); return; }
    size_t n = o.data ? strlen(o.data) : 0;
    while (n && (o.data[n - 1] == '\n' || o.data[n - 1] == ' ')) n--;
    if (n < 40) { printf("    " C_RED "[%d] ⎿ its work could not be committed" C_RESET "\n", i + 1); sb_free(&o); task_end(w, T_FAILED, "failed: its work could not be committed"); return; }
    snprintf(w->commit, sizeof w->commit, "%.*s", (int)(n < 63 ? n : 63), o.data);
    sb_clear(&o);
    int rc = tools_shell_in(w->root, r->goal->spec->metric, GOAL_METRIC_TIMEOUT, &o, &intr, "measuring a candidate", "measuring");
    if (intr) { r->aborted = true; task_end(w, T_STOPPED, "stopped by the user before it was measured"); sb_free(&o); return; }
    w->scored = rc == 0 && goal_number(o.data, &w->metric);
    if (w->scored) printf("    " C_DIM "[%d] ⎿ score %.6g" C_RESET "\n", i + 1, w->metric);
    else printf("    " C_RED "[%d] ⎿ no score: the metric printed no number" C_RESET "\n", i + 1);
    sb_free(&o);
    task_end(w, w->scored ? T_ACCEPTED : T_FAILED, w->scored ? "scored" : "no score");
}

/* the worker's model call has come back: run the tools it asked for and call again, or take its report to the review */
static void worker_step(orch_run *r, int i) {
    orch_worker *w = &r->w[i];
    sbuf body; sb_init(&body); http_result res;
    int rc = http_job_finish(w->job, &body, &res);
    w->job = NULL;
    ratelimit_set(worker_model(w), &res.rl);   /* (a worker's call is a call of its own model: the main one unless /goal named another) */
    chat_stats st; char err[512] = "";
    cJSON *reply = rc == 0 ? (worker_remote(w) ? provider_turn_reply(worker_model(w), body.data, res.status, &st, err, sizeof err)
                                            : ollama_chat_reply(body.data, res.status, true, &st, err, sizeof err)) : NULL;
    if (rc != 0) snprintf(err, sizeof err, "%s", res.err);
    sb_free(&body);
    if (!reply) { printf("    " C_RED "[%d] ⎿ the model call failed: %s" C_RESET "\n", i + 1, err); task_end(w, T_FAILED, "failed: the model call did"); return; }
    account(&st);
    term_status_refresh();
    cJSON_AddItemToArray(w->msgs, reply);
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls"), *content = cJSON_GetObjectItemCaseSensitive(reply, "content"), *call;
    if (!calls || cJSON_GetArraySize(calls) == 0) {
        sb_clear(&w->out); sb_puts(&w->out, cJSON_IsString(content) && content->valuestring[0] ? content->valuestring : "(it returned an empty report)");
        printf("    " C_DIM "[%d] ⎿ report after %d tool round%s:" C_RESET "\n", i + 1, w->rounds, w->rounds == 1 ? "" : "s");
        print_result_preview(w->out.data, 6);
        if (r->goal) goal_finish(r, i); else worker_review(r, i);
        return;
    }
    if (w->rounds >= SUBAGENT_ITERS) {
        printf("    " C_RED "[%d] ⎿ stopped after %d tool rounds without a report" C_RESET "\n", i + 1, SUBAGENT_ITERS);
        sb_clear(&w->out); if (cJSON_IsString(content)) sb_puts(&w->out, content->valuestring);
        task_end(w, T_FAILED, "failed: no report after all its tool rounds");
        return;
    }
    w->round_first = cJSON_GetArraySize(w->msgs);
    tools_checkpoint_step(i + 1);   /* what it writes is this task's, whatever the others write meanwhile */
    cJSON_ArrayForEach(call, calls) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
        const char *name = cJSON_IsString(nm) ? nm->valuestring : "?";
        cJSON *parsed = NULL;
        if (cJSON_IsString(args)) { parsed = cJSON_Parse(args->valuestring); args = parsed; }
        char summ[160]; tool_arg_summary(name, args, summ, sizeof summ);
        printf("    " C_DIM "[%d] ⎿ %s%s%s%s" C_RESET "\n", i + 1, name, summ[0] ? "(" : "", summ, summ[0] ? ")" : "");
        sbuf o; sb_init(&o);
        g_session.tool_calls++;
        bool entered = w->root && tools_enter(w->root);   /* a /goal candidate's calls run in its copy, and stay in it */
        if (!strcmp(name, "task") || !strcmp(name, "advisor")) sb_printf(&o, "error: %s is not available to a worker; do the task with the other tools and report", name);
        else tools_execute(name, args, &o);
        if (entered) tools_leave();
        cJSON_AddItemToArray(w->msgs, tool_result_message(name, tool_call_id(call), o.data ? o.data : ""));
        sb_free(&o);
        if (parsed) cJSON_Delete(parsed);
    }
    tools_checkpoint_step(0);
    w->rounds++;
    if (term_poll_interrupt()) { task_end(w, T_STOPPED, "stopped by the user before it was finished"); r->aborted = true; return; }
    worker_call(r, i);
}

/* Run the tasks: start every one whose tasks-to-wait-for are accepted, up to `maxpar` at a time,
 * and step each worker whenever its model call is back. One that is not accepted takes the tasks
 * that wait for it with it; the others go on. Esc, or a message of the user's, stops them all. */
static void orch_schedule(orch_run *r, int maxpar) {
    term_raw(true);
    char label[64] = "";
    for (;;) {
        int running = 0;
        for (int i = 0; i < r->nt; i++) if (r->w[i].state == T_RUNNING) running++;
        for (int i = 0; i < r->nt && !r->aborted; i++) {
            if (r->w[i].state != T_WAITING || (r->upto && r->tasks[i].layer > r->upto)) continue;
            bool ready = true, lost = false;
            for (int j = 0; j < i; j++) {
                if (!(r->tasks[i].after & (1u << j)) || task_ok(&r->w[j])) continue;
                if (r->w[j].state == T_WAITING || r->w[j].state == T_RUNNING) ready = false; else lost = true;
            }
            /* a task that owns paths waits while a running task owns an overlapping one; a task with no OWNS: clashes with nothing */
            bool clash = false;
            for (int j = 0; j < r->nt && !clash; j++)
                if (j != i && r->w[j].state == T_RUNNING && owns_overlap(r->tasks[i].owns, r->tasks[i].nowns, r->tasks[j].owns, r->tasks[j].nowns)) clash = true;
            if (lost) task_end(&r->w[i], T_SKIPPED, "not started: a task it waits for was not accepted");
            else if (ready && !clash && running < maxpar) { worker_begin(r, i); if (r->w[i].state == T_RUNNING) running++; }
        }
        if (!running || r->aborted) break;
        char now[64]; snprintf(now, sizeof now, running == 1 ? "worker · 1 task running" : "workers · %d tasks running", running);
        if (strcmp(now, label)) { snprintf(label, sizeof label, "%s", now); term_busy(label); }
        bool stepped = false;
        for (int i = 0; i < r->nt && !r->aborted; i++)
            if (r->w[i].state == T_RUNNING && http_job_done(r->w[i].job)) { worker_step(r, i); stepped = true; label[0] = 0; }   /* (a tool or a review took the bar: say it again) */
        if (stepped) continue;
        if (term_poll_interrupt()) { r->aborted = true; break; }
        term_busy_tick();
        nanosleep(&(struct timespec){ 0, 50 * 1000 * 1000 }, NULL);
    }
    term_busy(NULL);
    if (!r->aborted) return;
    printf(C_YELLOW "⏹ the run was stopped" C_RESET "\n");
    for (int i = 0; i < r->nt; i++) if (r->w[i].state == T_RUNNING) http_job_cancel(r->w[i].job);
    for (int i = 0; i < r->nt; i++) {
        orch_worker *w = &r->w[i];
        if (w->state == T_RUNNING) { http_result res; http_job_finish(w->job, NULL, &res); w->job = NULL; task_end(w, T_STOPPED, "stopped by the user before it was finished"); }
        else if (w->state == T_WAITING) task_end(w, T_SKIPPED, "not started: the run was stopped");
    }
}

/* ---------- /plan: the same run, built layer by layer ----------
 * /plan is /orchestrate whose plan says "LAYER: n" under each task. The layers run one after
 * another: the tasks of a layer are one schedule of the run (orch_run.upto keeps the later ones
 * back), and once they are all accepted the project is built — plan_build_cmd(): build= in the
 * config, else the project's own build file — before the next layer starts. A failed build becomes
 * a fix task of the layer, appended to the same run so that its review is the orchestrator's; at
 * most ORCH_FIXES per layer, then the run stops and says why. Times are kept per layer and printed
 * and reported, so how fast a plan is gets measured rather than guessed. */
#define ORCH_FIXES 2
#define PLAN_BUILD_TIMEOUT 900

static double mono_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9; }
static void fmt_secs(double s, char *out, size_t n) {
    if (s < 60) snprintf(out, n, "%.0fs", s);
    else snprintf(out, n, "%dm%02ds", (int)(s / 60), (int)s % 60);
}

/* the command that builds the project: build= if set, else what the project's build file says (checked at each gate:
 * layer 1 may be the one that creates the build file). NULL when there is none. */
static char *plan_build_cmd(void) {
    if (g_cfg.build && *g_cfg.build) return xstrdup(g_cfg.build);
    if (is_file("mvnw") && !access("mvnw", X_OK)) return xstrdup("./mvnw -q -DskipTests compile");
    if (is_file("pom.xml")) return xstrdup("mvn -q -DskipTests compile");
    if (is_file("gradlew") && !access("gradlew", X_OK)) return xstrdup("./gradlew -q compileJava");
    if (is_file("build.gradle") || is_file("build.gradle.kts")) return xstrdup("gradle -q compileJava");
    if (is_file("Makefile")) return xstrdup("make");
    return NULL;
}

static int plan_layers(const orch_task *t, int nt) {   /* how many layers the plan has (they never go back, so a change is a new one) */
    int n = 0, last = 0;
    for (int i = 0; i < nt; i++) if (t[i].layer != last) { n++; last = t[i].layer; }
    return n;
}

/* a failed build's fix: a task of the run, in the slot reserved for it, of the layer that failed to build */
static void orch_add_fix(orch_run *r, int layer, const char *cmd, const char *out) {
    orch_task *t = &r->tasks[r->nt];
    sbuf b; sb_init(&b);
    sb_printf(&b, "The project does not build after layer %d. Its build command is `%s`; the end of its output:\n\n", layer, cmd);
    size_t ol = strlen(out), from = ol > 4096 ? ol - 4096 : 0;
    if (from) { while (from < ol && out[from] != '\n') from++; if (from < ol) from++; }   /* the errors are at the end; start on a whole line */
    sb_append(&b, out + from, ol - from);
    sb_puts(&b, "\n\nFind what the errors name and fix it. Change nothing else, and run the build command again to see it pass. Report what you fixed.");
    *t = (orch_task){ .title = xstrdup("Fix the build"), .body = sb_detach(&b), .layer = layer };
    r->nt++;
}

/* Runs the plan layer by layer (r->nt tasks of the plan, nplan of them: the fix tasks come after). Sets r->aborted
 * when the user stopped it; a layer with a task not accepted, or a build that still fails after its fixes, stops it. */
static void orch_layers(orch_run *r, int nplan, int maxpar, sbuf *report) {
    double t_all = mono_now(), t_build = 0; int fixes_all = 0, layers_done = 0; bool halted = false;
    int L = 0;
    for (;;) {
        int next = 0;   /* the lowest layer above the last one run */
        for (int i = 0; i < nplan; i++) if (r->tasks[i].layer > L && (!next || r->tasks[i].layer < next)) next = r->tasks[i].layer;
        if (!next) break;
        L = next; r->upto = L;
        int ntask = 0;
        for (int i = 0; i < nplan; i++) ntask += r->tasks[i].layer == L;
        double t0 = mono_now();
        orch_schedule(r, maxpar);
        double t_work = mono_now() - t0;
        if (r->aborted) break;
        bool ok = true;
        for (int i = 0; i < nplan; i++) if (r->tasks[i].layer == L && !task_ok(&r->w[i])) ok = false;
        if (!ok) {
            printf(C_YELLOW "⏹ layer %d: a task was not accepted, so the later layers are not run" C_RESET "\n", L);
            sb_printf(report, "\n[layer %d: a task was not accepted, so the later layers were not run.]\n", L);
            halted = true; break;
        }
        char *cmd = plan_build_cmd();
        int fixes = 0; double t_bl = 0; bool built = false, stop = false;
        if (!cmd) printf(C_DIM "    layer %d is not built: no build command (build= in the config, or a build file)" C_RESET "\n", L);
        while (cmd && !built && !stop) {
            sbuf out; sb_init(&out); bool intr = false;
            double tb = mono_now();
            int rc = tools_shell_in(NULL, cmd, PLAN_BUILD_TIMEOUT, &out, &intr, "building the project", "building");
            t_bl += mono_now() - tb;
            if (intr) { r->aborted = true; stop = true; }
            else if (rc == 0) { built = true; printf("    " C_GREEN "✓ built" C_RESET C_DIM " · %s" C_RESET "\n", cmd); }
            else if (fixes == ORCH_FIXES) {
                printf("    " C_RED "✗ still does not build after %d fixes" C_RESET "\n", fixes);
                stop = halted = true;
            } else {
                printf("    " C_YELLOW "✗ does not build (exit %d): a fix task" C_RESET "\n", rc);
                orch_add_fix(r, L, cmd, out.data ? out.data : "");
                fixes++;
                orch_schedule(r, maxpar);   /* (the fix is the only task of the layer still to run) */
                if (r->aborted) stop = true;
                else if (!task_ok(&r->w[r->nt - 1])) { printf(C_YELLOW "⏹ the fix was not accepted, so the later layers are not run" C_RESET "\n"); stop = halted = true; }
            }
            sb_free(&out);
        }
        const char *bs = !cmd ? "not built" : built ? "built" : "not built";
        free(cmd);
        if (r->aborted || halted) break;
        t_build += t_bl; fixes_all += fixes; layers_done++;
        char tw[24], tb[24]; fmt_secs(t_work, tw, sizeof tw); fmt_secs(t_bl, tb, sizeof tb);
        printf("  " C_DIM "layer %d: %d task%s · %s · %s%s%s · %d fix%s" C_RESET "\n", L, ntask, ntask == 1 ? "" : "s", tw,
               bs, built ? " in " : "", built ? tb : "", fixes, fixes == 1 ? "" : "es");
        sb_printf(report, "[layer %d: %d task%s, %s; %s%s%s; %d fix%s]\n", L, ntask, ntask == 1 ? "" : "s", tw,
                  bs, built ? " in " : "", built ? tb : "", fixes, fixes == 1 ? "" : "es");
    }
    char ta[24], tbt[24]; fmt_secs(mono_now() - t_all, ta, sizeof ta); fmt_secs(t_build, tbt, sizeof tbt);
    if (layers_done || halted || r->aborted) {
        printf("  " C_DIM "total: %s · building %s · %d fix%s · %d layer%s done" C_RESET "\n", ta, tbt, fixes_all, fixes_all == 1 ? "" : "es", layers_done, layers_done == 1 ? "" : "s");
        sb_printf(report, "[total: %s, of which building %s; %d fix%s; %d layer%s done]\n", ta, tbt, fixes_all, fixes_all == 1 ? "" : "es", layers_done, layers_done == 1 ? "" : "s");
    }
}

static int orchestrate_run(const char *arg, bool plan) {
    const char *cmd = plan ? "/plan" : "/orchestrate";
    int ntcap = 0;   /* the size of the tasks array (a /plan run keeps room for its fix tasks) */
    if (!arg || !*arg) {
        if (plan) printf("usage: /plan REQUEST\n" C_DIM "  the advisor model (%s) plans the request in layers, and you approve the plan; then %s builds each layer as workers and the project is built before the next one. Shift+tab's plan mode is different: it only reads" C_RESET "\n",
                         g_cfg.advisor ? g_cfg.advisor : "none set: /advisor MODEL", model_set() ? g_cfg.model : "the model");
        else printf("usage: /orchestrate REQUEST\n" C_DIM "  the advisor model (%s) plans the request as tasks, %s does them as workers — those that do not wait for each other at the same time (/workers) — and the advisor reviews each" C_RESET "\n",
               g_cfg.advisor ? g_cfg.advisor : "none set: /advisor MODEL", model_set() ? g_cfg.model : "the model");
        return 0;
    }
    if (!g_cfg.advisor) { printf(C_DIM "no advisor is set — /advisor MODEL names the model that plans and reviews (bare /advisor says what it can be)" C_RESET "\n"); return 0; }
    if (!g_model_tools || g_cfg.no_tools) { printf(C_DIM "the workers are %s, and it has no tools to work with here (/tools, or another /model)" C_RESET "\n", g_cfg.model ? g_cfg.model : "the model"); return 0; }
    if (!advisor_info()) { printf(C_RED "✗ %s" C_RESET "\n", g_advisor_err); return 0; }
    advisor_plan ap; advisor_plan_now(&ap);
    size_t budget = ap.budget > 12288 ? ap.budget - 6144 : 6144;   /* (less the system prompt and the headings) */

    /* what the planner is shown, taken before the request joins the conversation */
    char *before = cJSON_GetArraySize(g_messages) ? transcript_text(g_messages, -1, budget / 4) : NULL;
    char *request = expand_mentions(arg);
    int first = begin_request();
    { sbuf raw; sb_init(&raw); sb_printf(&raw, "%s %s", cmd, arg); typed_note(raw.data, request); sb_free(&raw); }
    add_message("user", request);
    struct utsname un; uname(&un);
    sbuf brief; sb_init(&brief);
    sb_printf(&brief, "# Where the work happens\nWorking directory: %s · %s %s · git repository: %s · the workers are the model %s\n", g_cwd, un.sysname, un.machine, is_dir(".git") ? "yes" : "no", g_cfg.model);
    if (g_project_instructions) { sb_puts(&brief, "\n# The project's instructions (the workers are given them too)\n"); sb_put_cut(&brief, g_project_instructions, budget / 4); sb_putc(&brief, '\n'); }
    {   cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", ".");
        sbuf ls; sb_init(&ls); tools_execute("list_dir", a, &ls);
        sb_puts(&brief, "\n# The working directory\n"); sb_put_cut(&brief, ls.data ? ls.data : "", 4096); sb_putc(&brief, '\n');
        sb_free(&ls); cJSON_Delete(a); }
    if (before) { sb_puts(&brief, "\n# The conversation before this request\n"); sb_puts(&brief, before); sb_putc(&brief, '\n'); free(before); }
    sb_printf(&brief, "\n# PLAN this request%s\n", plan ? " in layers (LAYER: n under each task)" : ""); sb_put_cut(&brief, request, budget / 3);

    bool aborted = false;
    sbuf report; sb_init(&report);   /* the agent's side of the conversation: what came of the run */
    chat_stats st; int how;
    char *plan_text = orch_ask("plans the request", brief.data, &st, &how, plan);
    sb_free(&brief);
    orch_task *tasks = NULL;
    char *contract = NULL;
    int nt = plan_text ? orch_parse_plan_ex(plan_text, &tasks, &contract) : 0, accepted = 0;
    char spent[96];
    if (how != ADVICE_GIVEN) {
        aborted = how != ADVICE_NONE;
        sb_printf(&report, "[%s: the orchestrator (%s) gave no plan — %s. Nothing was changed.]", cmd, g_cfg.advisor, aborted ? "the run was stopped" : "its call failed");
    } else if (!nt) {
        printf("    " C_RED "⎿ no tasks in its answer:" C_RESET "\n"); print_result_preview(plan_text, 8);
        sb_printf(&report, "[%s: the orchestrator (%s) answered without a plan. Nothing was changed. It said:]\n%s", cmd, g_cfg.advisor, plan_text);
    } else {
        orch_spent(&st, spent, sizeof spent);
        printf("    " C_DIM "⎿ plan · %d task%s%s · %s:" C_RESET "\n", nt, nt == 1 ? "" : "s", plan ? (plan_layers(tasks, nt) == 1 ? " in one layer" : "") : "", spent);
        for (int i = 0; i < nt; i++) {
            const char *title = tasks[i].title[0] ? tasks[i].title : "(untitled)";
            if (plan && (i == 0 || tasks[i].layer != tasks[i - 1].layer)) printf("    " C_CYAN "layer %d" C_RESET "\n", tasks[i].layer);
            int w = term_width() - 12 - 30; if (w < 20) w = 20;   /* "      12. " prefix + ellipsis + what it waits for */
            size_t n = strlen(title), cut = n > (size_t)w ? (size_t)w : n;
            while (cut > 0 && cut < n && ((unsigned char)title[cut] & 0xC0) == 0x80) cut--;   /* don't split UTF-8 */
            char wait[64] = "";   /* said only where the plan departs from "one after another" */
            if (i > 0 && tasks[i].after != (1u << (i - 1))) {
                size_t wl = (size_t)snprintf(wait, sizeof wait, tasks[i].after ? "  after" : "  with the tasks before it");
                for (int j = 0; j < i && wl < sizeof wait - 8; j++) if (tasks[i].after & (1u << j)) wl += (size_t)snprintf(wait + wl, sizeof wait - wl, " %d", j + 1);
            }
            printf("      %d. %.*s%s" C_DIM "%s" C_RESET "\n", i + 1, (int)cut, title, cut < n ? "…" : "", wait);
        }
        bool go = true; char *reason = NULL;
        if (g_cfg.mode == MODE_PLAN) {
            printf(C_DIM "    plan mode: the tasks are not run — shift+tab leaves it, then %s again" C_RESET "\n", cmd);
            sb_printf(&report, "[%s in plan mode: the orchestrator (%s) planned %d task%s; none was run.]\n\n%s", cmd, g_cfg.advisor, nt, nt == 1 ? "" : "s", plan_text);
            go = false;
        } else if (plan && !g_cfg.interactive) {
            printf(C_DIM "    /plan asks before it runs, and there is no terminal to ask in" C_RESET "\n");
            sb_printf(&report, "[/plan needs a terminal to ask before it runs. Nothing was changed.]\n\n%s", plan_text);
            go = false;
        } else if (plan ? !g_plan_no_ask : (g_cfg.interactive && g_cfg.mode != MODE_AUTO && !g_orch_no_ask)) {   /* /plan asks in every mode (the approval) until told not to */
            char q[200];
            if (plan) snprintf(q, sizeof q, "Build %d task%s in %d layer%s with %.60s, building after each layer?", nt, nt == 1 ? "" : "s", plan_layers(tasks, nt), plan_layers(tasks, nt) == 1 ? "" : "s", g_cfg.model);
            else snprintf(q, sizeof q, "Run %s with %.80s?", nt == 1 ? "this task" : "these tasks", g_cfg.model);
            int r = term_confirm(q, "Yes, and don't ask about a plan again this session", NULL, &reason);
            if (r == 2) { if (plan) g_plan_no_ask = true; else g_orch_no_ask = true; }
            if (!r) {
                go = false;
                printf(C_DIM "    the plan was not run" C_RESET "\n");
                sb_printf(&report, "[%s: the orchestrator (%s) planned %d task%s and the user declined to run them%s%s. Nothing was changed.]\n\n%s",
                          cmd, g_cfg.advisor, nt, nt == 1 ? "" : "s", reason ? ", saying: " : "", reason ? reason : "", plan_text);
            }
        }
        free(reason);
        if (go) sb_printf(&report, "[%s: %s planned %d task%s, %s did them as workers, and %s reviewed each.]\n", cmd, g_cfg.advisor, nt, nt == 1 ? "" : "s", g_cfg.model, g_cfg.advisor);

        if (go) {
            int maxpar = g_cfg.workers > 0 ? g_cfg.workers : ORCH_WORKERS_DEFAULT;
            ntcap = plan ? 3 * nt : nt;   /* a layered run: two fix tasks per layer at most, and there are at most nt layers */
            tasks = xrealloc(tasks, sizeof *tasks * (size_t)ntcap);
            memset(tasks + nt, 0, sizeof *tasks * (size_t)(ntcap - nt));
            orch_run run = { arg, tasks, xmalloc(sizeof(orch_worker) * (size_t)ntcap), nt, cJSON_CreateArray(), first, budget, false, NULL, .contract = contract };
            memset(run.w, 0, sizeof(orch_worker) * (size_t)ntcap);
            for (int i = 0; i < ntcap; i++) { sb_init(&run.w[i].out); sb_init(&run.w[i].objection); }
            /* what a worker may call: everything but another agent and the advisor (its reviewer is the orchestrator) */
            cJSON *t;
            cJSON_ArrayForEach(t, g_tools) {
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function"), *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
                if (cJSON_IsString(nm) && strcmp(nm->valuestring, "task") && strcmp(nm->valuestring, "advisor")) cJSON_AddItemReferenceToArray(run.tools, t);
            }
            if (plan) orch_layers(&run, nt, maxpar, &report);
            else orch_schedule(&run, maxpar);
            tools_checkpoint_step(0);
            aborted = run.aborted;
            int nr = run.nt;   /* the plan's tasks and the fix tasks that were added */
            for (int i = 0; i < nr; i++) {
                orch_worker *w = &run.w[i];
                if (task_ok(w)) accepted++;
                sb_printf(&report, "\n## Task %d of %d: %s — %s\n", i + 1, nr, tasks[i].title, w->status ? w->status : "not started");
                if (w->state != T_SKIPPED) sb_put_cut(&report, w->out.data && w->out.data[0] ? w->out.data : "(no report)", 6144);
                if (!task_ok(w) && w->objection.len) { sb_puts(&report, "\n\nThe orchestrator's objection:\n"); sb_put_cut(&report, w->objection.data, 2048); }
                sb_putc(&report, '\n');
                if (w->msgs) cJSON_Delete(w->msgs);
                free(w->role_model);
                sb_free(&w->out); sb_free(&w->objection);
            }
            for (int i = nr; i < ntcap; i++) { orch_worker *w = &run.w[i]; sb_free(&w->out); sb_free(&w->objection); }
            free(run.w); cJSON_Delete(run.tools);
            nt = nr;   /* (the tasks array is freed below with its room) */
            sbuf names; sb_init(&names);
            int nf = tools_checkpoint_files(first, &names);
            if (nf) sb_printf(&report, "\nFiles written: %s\n", names.data);
            printf("%s%s %s: %d of %d task%s accepted" C_RESET C_DIM "%s%s%s" C_RESET "\n", accepted == nt ? C_GREEN : C_YELLOW, accepted == nt ? "✓" : "!", plan ? "planned run" : "orchestrated", accepted, nt, nt == 1 ? "" : "s",
                   nf ? " · files written: " : "", nf ? names.data : "", accepted == nt ? "" : " — the tasks that are left were not run; /rewind undoes what was written");
            sb_free(&names);
        }
    }
    add_message("assistant", report.data ? report.data : "[/orchestrate: nothing was done]");
    sb_free(&report); free(plan_text); free(request);
    if (tasks) orch_tasks_free(tasks, ntcap ? ntcap : nt);
    free(contract);
    session_save();
    memory_note(first, aborted);
    return aborted ? -1 : 0;
}

/* ---------- /goal: candidates in git worktrees, a metric decides ----------
 * /goal "METRIC" [min|max] [-n ROUNDS] [-k CANDIDATES] [-m MODEL,...] GOAL. The metric is a shell
 * command whose last line is a number. The work is snapshotted first — the working tree as it is,
 * uncommitted and untracked files included, in a commit made through a temporary index, so neither
 * the index nor HEAD is touched — and every candidate starts from that snapshot in a git worktree of
 * its own under the config directory. The candidates of a round run on the orchestrate scheduler
 * (orch_schedule, worker_*), each on its model (-m, round-robin), with its tool calls inside its copy
 * (tools_enter). When a candidate reports, its work is committed and the metric run in its copy; the
 * best score that beats the best so far is kept and the next round starts from it, the others being told
 * what they did. The run stops at ROUNDS, when a kept candidate says GOAL: REACHED and the metric agrees,
 * or when every candidate of a round says GOAL: IMPOSSIBLE. The kept result is applied to the working
 * tree as uncommitted changes — only if the tree still is the snapshot — and is also a branch
 * (corbie-goal/ID). Every worktree is removed on the way out. Bash is the one escape: a shell command
 * can name a path outside the copy, and nothing stops it. */
static void goal_usage(void) {
    printf("usage: /goal \"METRIC\" [min|max] [-n ROUNDS] [-k CANDIDATES] [-m MODEL,MODEL,...] GOAL\n"
           C_DIM "  METRIC is a shell command whose last line is a number (higher is better; min: lower). Each round, CANDIDATES copies of the project (git worktrees)"
           " try the goal at once, on the models named (round-robin); the best score is kept. A candidate may say the goal is IMPOSSIBLE." C_RESET "\n");
}

/* a string as one shell word */
static void sbq(sbuf *b, const char *s) {
    sb_putc(b, '\'');
    for (; *s; s++) { if (*s == '\'') sb_puts(b, "'\\''"); else sb_putc(b, *s); }
    sb_putc(b, '\'');
}

/* a copy of s without its trailing newlines and blanks, malloc'd */
static char *goal_trim(const char *s) {
    size_t n = s ? strlen(s) : 0;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    char *t = xmalloc(n + 1); memcpy(t, s ? s : "", n); t[n] = 0;
    return t;
}

/* git worktree remove of one copy, and nothing else */
static void goal_remove_copy(const char *top, const char *dir) {
    sbuf c, o; sb_init(&c); sb_init(&o); bool intr = false;
    sb_puts(&c, "git -C "); sbq(&c, top); sb_puts(&c, " worktree remove --force "); sbq(&c, dir); sb_puts(&c, " >/dev/null 2>&1");
    tools_shell_in(NULL, c.data, 120, &o, &intr, "removing a copy", "worktree");
    sb_free(&c); sb_free(&o);
}

/* the metric of a commit, in a copy of its own that is made and removed here: 1 scored, 0 not, -1 stopped */
static int goal_measure(const char *top, const char *commit, const char *dir, const goal_spec *spec, const char *what, double *v) {
    sbuf c, o; sb_init(&c); sb_init(&o); bool intr = false;
    sb_puts(&c, "git -C "); sbq(&c, top); sb_puts(&c, " worktree add --detach "); sbq(&c, dir); sb_putc(&c, ' '); sbq(&c, commit); sb_puts(&c, " >/dev/null 2>&1");
    if (tools_shell_in(NULL, c.data, 120, &o, &intr, what, "worktree") != 0) { sb_free(&c); sb_free(&o); printf("  " C_RED "⎿ cannot make a copy to measure in" C_RESET "\n"); return 0; }
    sb_clear(&o);
    int rc = tools_shell_in(dir, spec->metric, GOAL_METRIC_TIMEOUT, &o, &intr, what, "measuring");
    int got = intr ? -1 : (rc == 0 && goal_number(o.data, v)) ? 1 : 0;
    sb_free(&o);
    goal_remove_copy(top, dir);
    sb_free(&c);
    return got;
}

static int cmd_goal(const char *arg) {
    if (!arg || !*arg) { goal_usage(); return 0; }
    goal_spec spec; char err[200];
    if (!goal_parse(arg, &spec, err, sizeof err)) { printf(C_RED "✗ %s" C_RESET "\n", err); goal_usage(); return 0; }
    if (!is_dir(".git")) { printf(C_DIM "/goal needs a git repository: its candidates are git worktrees of it" C_RESET "\n"); goal_spec_free(&spec); return 0; }
    if (!g_model_tools || g_cfg.no_tools) { printf(C_DIM "/goal needs tools: the candidates work with them (/tools, or another /model)" C_RESET "\n"); goal_spec_free(&spec); return 0; }
    /* every model named must be usable now: a hosted one has its key, a local one is on the server */
    model_info pre[ORCH_WORKERS_MAX];
    memset(pre, 0, sizeof pre);
    for (int k = 0; k < spec.nmodels; k++) {
        const char *m = spec.models[k];
        if (provider_find(m, NULL)) {
            const char *hd[4]; sbuf keep; char why[256]; sb_init(&keep);
            bool ok = provider_call_headers(m, hd, &keep, why, sizeof why);
            sb_free(&keep);
            if (!ok) { printf(C_RED "✗ %s: %s" C_RESET "\n", m, why); goal_spec_free(&spec); return 0; }
        } else if (ollama_model_show(m, &pre[k]) != 0) { printf(C_RED "✗ %s: ollama does not have that model" C_RESET "\n", m); goal_spec_free(&spec); return 0; }
    }

    char id[64]; snprintf(id, sizeof id, "%ld-%ld", (long)time(NULL), (long)getpid());
    sbuf rb; sb_init(&rb); sb_printf(&rb, "%s/goals/%s", config_dir(), id); char *root = sb_detach(&rb);
    int first = begin_request();
    add_message("user", spec.task);
    sbuf o, c; sb_init(&o); sb_init(&c); bool intr = false;
    sb_puts(&c, "mkdir -p "); sbq(&c, root);
    tools_shell_in(NULL, c.data, 30, &o, &intr, "preparing the goal", "goal");
    sb_clear(&c); sb_clear(&o);

    /* the snapshot: the tree as it is, in a commit of its own */
    sb_puts(&c, "git rev-parse --show-toplevel");
    int rc = tools_shell_in(NULL, c.data, 30, &o, &intr, "reading the repository", "repository");
    char *top = goal_trim(rc == 0 ? o.data : "");
    sb_clear(&c); sb_clear(&o);
    char tree[64] = "", snap[64] = "";
    if (rc == 0 && top[0]) {
        sb_puts(&c, "cd "); sbq(&c, top);
        sb_puts(&c, " && T=$(mktemp -u) && GIT_INDEX_FILE=$T git add -A && TREE=$(GIT_INDEX_FILE=$T git write-tree) && rm -f $T && P= && if git rev-parse -q --verify HEAD >/dev/null; then P='-p HEAD'; fi"
                    " && S=$(" GOAL_GIT "git commit-tree $TREE $P -m corbie-goal-snapshot) && echo \"$TREE $S\"");
        rc = tools_shell_in(NULL, c.data, 300, &o, &intr, "snapshotting the working tree", "snapshot");
        if (rc != 0 || sscanf(o.data ? o.data : "", "%63s %63s", tree, snap) != 2) rc = -1;
    }
    sb_clear(&c); sb_clear(&o);
    if (rc != 0 || !snap[0]) {
        printf(C_RED "✗ cannot snapshot the working tree of this repository" C_RESET "\n");
        sb_puts(&c, "rm -rf "); sbq(&c, root); tools_shell_in(NULL, c.data, 60, &o, &intr, "cleaning up", "goal");
        sb_free(&c); sb_free(&o); free(top); free(root); goal_spec_free(&spec);
        add_message("assistant", "[/goal: the working tree could not be snapshotted; nothing was run.]");
        return 0;
    }
    sb_free(&c); sb_free(&o);
    printf("  " C_CYAN "⤷ goal" C_RESET " " C_BOLD "%s" C_RESET C_DIM " · %s %s · %d round%s × %d candidate%s · esc stops it" C_RESET "\n",
           spec.task, spec.lower ? "min" : "max", spec.metric, spec.rounds, spec.rounds == 1 ? "" : "s", spec.cands, spec.cands == 1 ? "" : "s");

    char dirbuf[1200];
    snprintf(dirbuf, sizeof dirbuf, "%s/base", root);
    double base = 0;
    int got = goal_measure(top, snap, dirbuf, &spec, "measuring the baseline", &base);
    bool aborted = got < 0;
    if (got == 0 && !aborted) printf(C_RED "✗ the metric printed no number on the working tree as it is" C_RESET "\n");
    if (got == 1) printf("  " C_DIM "⎿ baseline score %.6g" C_RESET "\n", base);
    double best = base, base0 = base;
    char cur[64], winner[64] = ""; snprintf(cur, sizeof cur, "%s", snap);
    sbuf prior; sb_init(&prior);
    bool stop = aborted || got != 1;
    int K = spec.cands;
    for (int round = 1; round <= spec.rounds && !stop; round++) {
        printf("  " C_CYAN "⤷ goal round %d/%d" C_RESET C_DIM " · %d candidate%s from %.12s · best %.6g" C_RESET "\n", round, spec.rounds, K, K == 1 ? "" : "s", cur, best);
        orch_task *tasks = xmalloc(sizeof *tasks * (size_t)K);
        orch_worker *ws = xmalloc(sizeof *ws * (size_t)K);
        memset(ws, 0, sizeof *ws * (size_t)K);
        char **dirs = xmalloc(sizeof *dirs * (size_t)K);
        goal_ctx gc = { &spec, best, prior.len ? prior.data : NULL };
        for (int k = 0; k < K; k++) {
            const char *model = spec.nmodels ? spec.models[k % spec.nmodels] : NULL;
            if (model && !strcmp(model, g_cfg.model)) model = NULL;
            char title[200];
            snprintf(title, sizeof title, "candidate %d of %d%s%s", k + 1, K, model ? " on " : "", model ? model : "");
            tasks[k].title = xstrdup(title);
            tasks[k].body = xstrdup("Make the goal true in your copy, as the goal describes it, and report.");
            tasks[k].after = 0;
            ws[k].model = model;
            if (model && !provider_find(model, NULL)) {
                int idx = k % spec.nmodels;
                ws[k].info = pre[idx]; ws[k].has_info = true;
            }
            dirs[k] = xmalloc(1200);
            snprintf(dirs[k], 1200, "%s/r%d-%d", root, round, k + 1);
            sb_clear(&c);
            sb_puts(&c, "git -C "); sbq(&c, top); sb_puts(&c, " worktree add --detach "); sbq(&c, dirs[k]); sb_putc(&c, ' '); sbq(&c, cur); sb_puts(&c, " >/dev/null 2>&1");
            if (tools_shell_in(NULL, c.data, 120, &o, &intr, "making a candidate's copy", "worktree") != 0) {
                printf("  " C_RED "⎿ candidate %d: its copy could not be made" C_RESET "\n", k + 1);
                ws[k].state = T_FAILED; ws[k].status = "failed: its copy could not be made";
                free(dirs[k]); dirs[k] = NULL;
                continue;
            }
            ws[k].root = dirs[k];
            sb_clear(&o);
        }
        orch_run run = { .request = spec.task, .tasks = tasks, .w = ws, .nt = K, .tools = cJSON_CreateArray(), .first = first, .goal = &gc };
        cJSON *t;
        cJSON_ArrayForEach(t, g_tools) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function"), *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            if (cJSON_IsString(nm) && strcmp(nm->valuestring, "task") && strcmp(nm->valuestring, "advisor")) cJSON_AddItemReferenceToArray(run.tools, t);
        }
        orch_schedule(&run, K);
        if (run.aborted) aborted = true;

        /* the round's outcome: the best candidate that beats the best so far, and whether all of them gave up */
        int win = -1; bool all_imp = true;
        sb_clear(&prior); sb_puts(&prior, "# The candidates of the last round (the scores are the metric's)\n");
        for (int k = 0; k < K; k++) {
            orch_worker *w = &ws[k];
            if (w->verdict != GOAL_IMPOSSIBLE) all_imp = false;
            if (w->scored && goal_better(w->metric, best, spec.lower) && (win < 0 || goal_better(w->metric, ws[win].metric, spec.lower))) win = k;
            char sc[48]; if (w->scored) snprintf(sc, sizeof sc, "score %.6g", w->metric); else snprintf(sc, sizeof sc, "no score");
            sb_printf(&prior, "- candidate %d (%s): %s", k + 1, worker_model(w), sc);
            if (w->verdict == GOAL_IMPOSSIBLE) sb_printf(&prior, ", it says the goal is impossible: %s", w->why);
            if (w->out.len) { sb_puts(&prior, " — "); sb_put_cut(&prior, w->out.data, 400); }
            sb_putc(&prior, '\n');
        }
        bool reached = false;
        if (win >= 0) {
            snprintf(winner, sizeof winner, "%s", ws[win].commit);
            snprintf(cur, sizeof cur, "%s", ws[win].commit);
            best = ws[win].metric;
            reached = ws[win].verdict == GOAL_REACHED;
            printf("  " C_GREEN "⎿ candidate %d kept: score %.6g" C_RESET "\n", win + 1, best);
        } else if (!aborted) printf("  " C_YELLOW "⎿ no candidate beat %.6g: the run stops" C_RESET "\n", best);
        if (reached) { printf("  " C_GREEN "⎿ the kept candidate says the goal is reached, and its score agrees" C_RESET "\n"); stop = true; }
        if (all_imp) { printf("  " C_YELLOW "⎿ every candidate says the goal is IMPOSSIBLE" C_RESET "\n"); stop = true; }
        if (win < 0 || aborted) stop = true;

        for (int k = 0; k < K; k++) {
            if (dirs[k]) goal_remove_copy(top, dirs[k]);
            free(dirs[k]);
            free(tasks[k].title); free(tasks[k].body);
            if (ws[k].msgs) cJSON_Delete(ws[k].msgs);
            sb_free(&ws[k].out); sb_free(&ws[k].objection);
        }
        free(dirs); free(ws); free(tasks); cJSON_Delete(run.tools);
    }

    /* the kept result: measured once more, applied as uncommitted changes if the tree is still the snapshot, and kept as a branch */
    sbuf report; sb_init(&report);
    char id_branch[96]; snprintf(id_branch, sizeof id_branch, "corbie-goal/%s", id);
    bool kept = winner[0] != 0 && !aborted;
    if (kept) {
        double again = 0;
        snprintf(dirbuf, sizeof dirbuf, "%s/check", root);
        int g2 = goal_measure(top, winner, dirbuf, &spec, "checking the kept candidate", &again);
        if (g2 == 1 && goal_better(again, base0, spec.lower)) best = again;
        else { printf("  " C_RED "⎿ the kept candidate did not hold up on the second measurement: nothing is applied" C_RESET "\n"); kept = false; }
    }
    int applied = 0;
    if (kept) {
        sb_clear(&c); sb_clear(&o);
        sb_puts(&c, "cd "); sbq(&c, top);
        sb_puts(&c, " && T=$(mktemp -u) && GIT_INDEX_FILE=$T git add -A && GIT_INDEX_FILE=$T git write-tree; rm -f $T");
        tools_shell_in(NULL, c.data, 300, &o, &intr, "checking the working tree", "checking");
        bool same = o.data && !strncmp(o.data, tree, strlen(tree)) && strlen(tree) > 0;
        sb_clear(&c); sb_clear(&o);
        if (!same) printf("  " C_YELLOW "⎿ the working tree changed during the goal: nothing is applied; the result is branch %s" C_RESET "\n", id_branch);
        else {
            sb_puts(&c, "cd "); sbq(&c, top); sb_puts(&c, " && git diff --name-only "); sb_puts(&c, snap); sb_putc(&c, ' '); sb_puts(&c, winner);
            tools_shell_in(NULL, c.data, 120, &o, &intr, "reading the result", "result");
            for (char *p = o.data; p && *p; ) {   /* each file the result changes is saved first, so /rewind undoes the goal */
                char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
                if (len) { char *abs = xmalloc(strlen(top) + len + 2); sprintf(abs, "%s/%.*s", top, (int)len, p); tools_checkpoint_path(abs); free(abs); }
                if (!e) break;
                p = e + 1;
            }
            sb_clear(&c); sb_clear(&o);
            sb_puts(&c, "cd "); sbq(&c, top); sb_puts(&c, " && git diff --binary "); sb_puts(&c, snap); sb_putc(&c, ' '); sb_puts(&c, winner); sb_puts(&c, " | git apply");
            rc = tools_shell_in(NULL, c.data, 300, &o, &intr, "applying the kept candidate", "applying");
            if (rc == 0) {
                applied = 1;
                printf("  " C_GREEN "✓ applied as uncommitted changes" C_RESET C_DIM " · %s" C_RESET "\n", id_branch);
            } else printf("  " C_RED "⎿ the result did not apply cleanly: it is branch %s" C_RESET "\n", id_branch);
        }
        sb_clear(&c); sb_clear(&o);
        sb_puts(&c, "git -C "); sbq(&c, top); sb_puts(&c, " branch "); sb_puts(&c, id_branch); sb_putc(&c, ' '); sb_puts(&c, winner);
        tools_shell_in(NULL, c.data, 60, &o, &intr, "keeping the result", "branch");
        sb_clear(&c); sb_clear(&o);
    }
    printf("%s%s goal: score %.6g → %.6g%s" C_RESET "\n", aborted ? C_YELLOW : applied ? C_GREEN : C_YELLOW, aborted ? "!" : applied ? "✓" : "·",
           base0, best, aborted ? " — the run was stopped" : applied ? "" : " — nothing applied");
    sb_printf(&report, "[/goal: %s, %d round%s × %d candidate%s; the score went from %.6g to %.6g%s%s. %s]\n",
              spec.task, spec.rounds, spec.rounds == 1 ? "" : "s", K, K == 1 ? "" : "s", base0, best,
              applied ? "; the result is applied to the working tree as uncommitted changes and kept as branch " : "", applied ? id_branch : "",
              aborted ? "The run was stopped." : applied ? "" : "Nothing was applied.");
    add_message("assistant", report.data);
    sb_free(&report);

    /* cleanup on every way out: the copies, the snapshot's worktrees, the folder */
    sb_clear(&c); sb_clear(&o);
    sb_puts(&c, "git -C "); sbq(&c, top); sb_puts(&c, " worktree prune >/dev/null 2>&1; rm -rf "); sbq(&c, root);
    tools_shell_in(NULL, c.data, 120, &o, &intr, "cleaning up", "goal");
    sb_free(&c); sb_free(&o); sb_free(&prior);
    free(top); free(root); goal_spec_free(&spec);
    session_save();
    memory_note(first, aborted);
    return aborted ? -1 : 0;
}

/* ---------- /btw: a side question, answered from the conversation and not kept in it ----------
 * The question goes to the model with the conversation so far as its context (the same prefix, so the
 * prompt cache serves most of it), and the answer is printed. Nothing joins g_messages, the session is
 * not saved and the memory is not updated for it. Tools are offered as in a turn, so the prompt is the
 * same, but a tool call in the answer is not run: the answer is text. */
static int cmd_btw(const char *arg) {
    if (!arg || !*arg) { printf(C_DIM "usage: /btw QUESTION — a side question: the model answers from the conversation so far, and the answer is not kept in it" C_RESET "\n"); return 0; }
    cJSON *msgs = messages_with_system();
    sbuf q; sb_init(&q);
    sb_puts(&q, "(A side question, asked aside. Answer it briefly, from what has been done so far. Do not call tools.)\n\n");
    sb_puts(&q, arg);
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content", q.data);
    sb_free(&q);
    cJSON_AddItemToArray(msgs, u);
    cJSON *tools = g_model_tools && !g_cfg.no_tools ? tools_for_mode() : NULL;
    printf(C_CYAN "? " C_RESET C_BOLD "%s" C_RESET "\n", arg);
    chat_stats st; bool aborted = false;
    ollama_call.busy = "side question";
    cJSON *reply = model_chat(msgs, tools, &st, &aborted);
    ollama_call_reset();
    if (tools && tools != g_tools) cJSON_Delete(tools);
    cJSON_Delete(msgs);
    account(&st);
    term_status_refresh();
    if (!reply) {
        if (!aborted) printf(C_RED "✗ no answer" C_RESET "\n");
        return aborted ? -1 : 0;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(reply, "content");
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
    if (calls && cJSON_GetArraySize(calls) > 0) printf(C_DIM "(it asked for a tool; /btw answers in text, so none was run)" C_RESET "\n");
    else if (!cJSON_IsString(content) || !content->valuestring[0]) printf(C_DIM "(no answer)" C_RESET "\n");
    cJSON_Delete(reply);
    print_stats(&st);
    return 0;
}

/* ---------- /init: have the model write AGENTS.md ---------- */
static int cmd_init(void) {
    const char *existing = is_file("AGENTS.md") ? "AGENTS.md" : is_file("CORBIENEST.md") ? "CORBIENEST.md" : is_file("CLAUDE.md") ? "CLAUDE.md" : NULL;
    sbuf b; sb_init(&b);
    sb_puts(&b, "Please analyze this codebase and create an AGENTS.md file, which will be given to you (and future instances of you) as project instructions at the start of every session in this directory.\n\n"
                "What to add:\n"
                "1. Commands that will be commonly used, such as how to build, lint, and run tests — including how to run a single test.\n"
                "2. High-level code architecture and structure that requires reading multiple files to understand: the main components, how they fit together, where things live. Do not list every file.\n"
                "3. Project conventions worth knowing (style, naming, patterns, things to avoid).\n\n"
                "Usage notes:\n"
                "- Explore first: list the directory, read the README, build files (Makefile, package.json, Cargo.toml, pyproject.toml, go.mod …), CI config and the main entry points before writing anything.\n"
                "- Keep it concise (aim for well under 100 lines); prefer facts that are not obvious from a glance at the tree.\n"
                "- Do not repeat instructions that are already covered by an existing rules file, and do not make things up: only include commands you have verified exist.\n"
                "- Write the file with write_file, then summarise what you put in it in one short paragraph.\n");
    if (existing) sb_printf(&b, "\nNote: a %s already exists in this directory. Read it first and improve it in place (keep what is right, fix what is wrong, fill the gaps) — write AGENTS.md only if you would otherwise clobber a hand-written %s.\n", existing, existing);
    printf(C_DIM "  /init: analysing the project and writing AGENTS.md…" C_RESET "\n");
    int first = begin_request();
    typed_note("/init", b.data);
    add_message("user", b.data); sb_free(&b);
    bool aborted = run_turn();
    session_save();
    memory_note(first, aborted);
    apply_pending();
    load_project_instructions();   /* pick up the new file right away */
    if (!aborted && g_project_instructions) printf(C_GREEN "✓ project instructions loaded" C_RESET "\n");
    return aborted ? -1 : 0;
}

/* ---------- /skills ---------- */
static void cmd_skills(const char *arg) {
    if (arg && !strncmp(arg, "new", 3) && (arg[3] == ' ' || arg[3] == 0)) {
        const char *name = arg + 3; while (*name == ' ') name++;
        if (!*name) { printf("usage: /skills new NAME\n"); return; }
        char path[1200];
        int rc = skill_scaffold(name, path, sizeof path);
        if (rc == 1) printf(C_YELLOW "%s already exists" C_RESET "\n", path);
        else if (rc < 0) printf(C_RED "✗ cannot create skill '%s': %s" C_RESET "\n", name, strerror(errno));
        else printf(C_GREEN "✓ created %s" C_RESET " — edit it, then run it with /%s [args]\n", path, name);
        skills_load();
        return;
    }
    if (arg && !strcmp(arg, "reload")) skills_load();
    int n = skills_count();
    if (!n) {
        printf(C_DIM "no skills found." C_RESET "\n"
               "  put a SKILL.md in .corbienest/skills/NAME/ (project) or %s/skills/NAME/ (user), or run " C_BOLD "/skills new NAME" C_RESET "\n", config_dir());
        return;
    }
    int w = term_width();
    printf(C_BOLD "%d skill%s" C_RESET " " C_DIM "(run with /NAME [args] · /skills reload · /skills new NAME)" C_RESET "\n", n, n == 1 ? "" : "s");
    int nw = 8; for (int i = 0; i < n; i++) { int l = (int)strlen(skill_get(i)->name) + 1; if (l > nw && l < 32) nw = l; }
    for (int i = 0; i < n; i++) {
        const skill_t *s = skill_get(i);
        int room = w - nw - 14; if (room < 20) room = 20;
        printf("  " C_ORANGE "/%-*s" C_RESET " %.*s%s " C_DIM "[%s]" C_RESET "\n", nw - 1, s->name, room, s->desc, (int)strlen(s->desc) > room ? "…" : "", s->source);
    }
}

/* ---------- slash commands ---------- */
static void cmd_help(void) {
    printf(C_BOLD "corbienest %s" C_RESET " — chat with local Ollama models, or Anthropic (anthropic:MODEL), with file & shell tools\n\n", CORBIE_VERSION);
    printf(C_BOLD "Commands\n" C_RESET
           "  /help                 this help\n"
           "  /model [name]         the model that does the work: a local one, or anthropic:MODEL / claude:MODEL (ANTHROPIC_API_KEY);\n"
           "                        the name completes as you type; no argument opens the local-model menu (saved as the default)\n"
           "  /models               list models available in ollama\n"
           "  /clear                start a new conversation\n"
           "  /compact              summarise the conversation to free context\n"
           "  /memory [on|off|clear|update|every N|idle N]  show the project memory (" MEMORY_PATH ", curated by the model every N requests, after N seconds idle at the prompt, and at exit; default every 5 / 15s idle), toggle it, run the update now, set the cadence, or delete it\n"
           "  /status               show model, context usage, settings\n"
           "  /cost                 tokens, model calls, model time and wall time of this session\n"
           "  /usage                tokens per model in this session — the advisor's too — and the estimated cost of the hosted ones;\n"
           "                        /usage price MODEL IN OUT sets a price (USD per million tokens), /usage price MODEL default takes the built-in one\n"
           "  /diff [git args]      show the working-tree diff (stat + patch + untracked), without sending it to the model; e.g. /diff --staged\n"
           "  /rewind               (or Esc Esc at an empty prompt) go back to an earlier request: undo the file changes since, the conversation, or both\n"
           "  /subagent [MODEL|default]  the model sub-agents (task) run on — the one doing the work, unless set here; /subagent effort LEVEL sets how hard it thinks\n"
           "  /system [text|clear]  show/set extra system instructions\n"
           "  /think on|off|auto    when a thinking-capable model thinks: on = every call, auto = only the first call of a request (default), off\n"
           "  /think show|hide      show or hide thinking tokens\n"
           "  /effort [LEVEL]       how hard this model thinks, in the levels it has (gpt-oss: low medium high · qwen3.8: off low medium high · most others: off on);\n"
           "                        no argument opens a picker, default leaves it to the model. Kept per model; a level is sent with every call\n"
           "  /orchestrate REQUEST  the advisor model in charge: it plans the request as tasks, this model does them as workers with a fresh context\n"
           "                        each — those that do not wait for each other at the same time — and the advisor reviews every task's report and diff\n"
           "  /plan REQUEST         /orchestrate with the plan in layers (LAYER: n): you approve it, then each layer runs and the project is built before the next.\n"
           "                        Build command: build=CMD in the config; else mvn, gradle or make, by the build file. Not the plan mode of shift+tab\n"
           "  /goal \"METRIC\" [min|max] [-n R] [-k K] [-m MODEL,...] GOAL\n"
           "                        candidates in git worktrees race to the goal, each on its model; the metric's best is kept as uncommitted changes\n"
           "  /btw QUESTION         a side question: the model answers from the conversation so far; the answer is printed, not kept in it\n"
           "  /workers [N]          how many tasks of /orchestrate may run at the same time (the model calls, each on a thread; default 3, at most 20, 1 = in turn)\n"
           "  /advisor [MODEL|off]  a stronger model the agent may consult when the work is hard (the advisor tool): a bigger local model, a cloud one\n"
           "                        (NAME-cloud, after `ollama signin`) or a hosted API: xai:MODEL, openai:MODEL, anthropic:MODEL (key from XAI_API_KEY,\n"
           "                        OPENAI_API_KEY, ANTHROPIC_API_KEY). It is shown the conversation and answers with advice; it has no tools.\n"
           "                        /advisor guidance light|normal|strong|max (how much the agent leans on it: consultations per request, how\n"
           "                        detailed; from normal it is brought in, unasked, when tool calls keep failing; strong+ also reviews a request that\n"
           "                        changed a lot (40 lines or 3 files) before it ends; max reviews any change, and checks the first change)\n"
           "                        /advisor effort [LEVEL] (how hard it thinks) · /advisor ctx N|auto (its context window; auto = the main one, at most 16k)\n"
           "                        /advisor window MINUTES|off (a request that runs long gets its consultations again after that long; default 15)\n"
           "  /skills [reload|new NAME]  list skills (SKILL.md files); run one with /NAME [args]\n"
           "  /init                 have the model explore the project and write an AGENTS.md (project instructions)\n"
           "  /mode [name]          permission mode: manual · accept-edits · plan · auto (or press shift+tab to cycle)\n"
           "  /permissions [...]    list the project's saved \"always allow\" rules (kept in the config directory, not in the project); add/remove/clear\n"
           "  /web [on|off|engine URL]  let the model look documentation up with web_search/web_fetch (on by default); set the search engine\n"
           "  /yolo [on|off]        shortcut for /mode auto / /mode manual (dangerous!)\n"
           "  /tools on|off         enable/disable tool calling\n"
           "  /max_iters [N]        tool rounds one request may run before the loop guard stops it (default 100); takes effect at once, even mid-turn\n"
           "  /ui [ansi|notcurses]  the screen: ansi (default) or notcurses (needs notcurses installed at build time); restarts into this session\n"
           "  /ctx [N|Nk|max]       context window: no argument opens a size picker; N/64k/max/default set it\n"
           "  /temp X               set temperature (-1 = server default)\n"
           "  /host URL             set ollama host (default http://127.0.0.1:11434)\n"
           "  /keepalive DUR        how long ollama keeps the model loaded between requests (default 30m; -1 = forever, 0 = unload, default = server's)\n"
           "  /save [file]          save transcript as markdown\n"
           "  /resume [ID|all]      pick an earlier session to continue (sessions are saved after every request; also --continue / --resume)\n"
           "  /history [N]          show the last N queries (default 20; the latest 100 are kept across sessions)\n"
           "  /cd DIR, /pwd         change / show working directory\n"
           "  /quit, /exit          leave (also Ctrl-D)\n\n"
           C_BOLD "Input\n" C_RESET
           "  !cmd                  run a shell command yourself; output is added to the conversation. Its words are completed as\n"
           "                        in a shell: programs from $PATH where a command goes, files and directories elsewhere\n"
           "  @path                 attach a file (or directory listing) to your message — while you type, the files matching the name\n"
           "                        so far are listed under the input field (@term finds src/term.c; @../lib/pa looks below ../lib)\n"
           "  # fact                remember something: appended to " MEMORY_PATH " (pick the section from a menu), no model call\n"
           "  Enter                 send  ·  Alt+Enter / Ctrl+J / trailing \\ : newline\n"
           "  Enter while busy      queue a message for the model (added between tool rounds or after the turn; Ctrl-C hands it back)\n"
           "                        commands that only report or set something run at once instead: /help /status /cost /usage /diff /history /pwd\n"
           "                        /skills /memory /mode /yolo /permissions /tools /web /max_iters /think /effort /advisor /temp /keepalive\n"
           "  Ctrl-C                cancel generation / clear line (twice: quit)  ·  Ctrl-L clear screen\n"
           "  click on a ▸ line      a tool result of more than one line is folded: the line says how long it is and how it begins,\n"
           "                        and a click on it shows the rest (▾), another hides it again\n"
           "  mouse wheel, PgUp/PgDn  scroll back through the conversation, also while the model works (at the prompt ↑/↓ and Home/End\n"
           "                        scroll too; Esc/Enter return); a line under it says where you are, the status bar stays as it is.\n"
           "                        The wheel does nothing over the input field: there ↑/↓ are the keys.\n"
           "                        Selecting text with the mouse needs Shift held down (in tmux too)\n"
           "  ←  on an empty line    the sub-agents of this session and what each did (the main agent is the list's first line)\n"
           "  status bar            bottom row shows the permission mode, model, session tokens and context usage\n"
           "  / @ and !             the commands (and their options), files, or programs and paths matching what you typed are listed\n"
           "                        under the input field, also while the model works: Tab completes, ↓ steps into the list, Enter\n"
           "                        takes the highlighted one, Esc closes it\n"
           "  ↑/↓                   history  ·  Ctrl-R search history\n\n"
           C_BOLD "Tools the model can call\n" C_RESET "  %s\n"
           "  write/edit/bash ask for confirmation: pick with ↑/↓ + enter, or press y (once), a (always this session), p (always in this project), n (deny, with optional reason)\n"
           "  modes: manual asks for everything · accept-edits auto-approves file edits · plan is read-only (model proposes a plan) · auto approves all\n",
           tools_summary_line());
}

/* " · effort high" for the model in use, when one is set: it came along with the model */
static const char *model_effort_note(void) {
    static char b[64];
    const char *e = effort_resolve(&g_model_info, effort_get(g_cfg.model), NULL);
    if (e) snprintf(b, sizeof b, C_DIM " · effort %s" C_RESET, e); else b[0] = 0;
    return b;
}

static void cmd_models(void) {
    cJSON *list = ollama_list_models();
    if (!list) return;
    cJSON *m;
    printf(C_BOLD "%-52s %-8s %s" C_RESET "\n", "model", "size", "capabilities");
    cJSON_ArrayForEach(m, list) {
        const char *n = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring;
        bool cur = g_cfg.model && (!strcmp(n, g_cfg.model));
        printf("%s%-52s" C_RESET " %-8s %s%s\n", cur ? C_GREEN "▸ " : "  ", n,
               cJSON_GetObjectItemCaseSensitive(m, "size")->valuestring,
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools")) ? C_CYAN "tools " C_RESET : C_DIM "chat-only " C_RESET,
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "thinking")) ? C_MAGENTA "thinking" C_RESET : "");
    }
    printf(C_DIM "switch with /model <name>; models without 'tools' cannot use files/shell" C_RESET "\n");
    cJSON_Delete(list);
}

static cJSON *model_list_get(int slot, const provider_def *p, const char **why);
static int provider_slot_of(const provider_def *p) { int slot = 1; while (provider_at(slot - 1) != p) slot++; return slot; }

/* /model's picker: the Ollama server's models, then the hosted ones that can do the work (the Messages
 * providers, named "anthropic:ID"). xAI and OpenAI are advisors only and are not offered here. A list
 * that cannot be had says why, under the menu's place; the menu is empty only when both are. */
static void cmd_model_picker(void) {
    cJSON *list = ollama_list_models();   /* NULL: the server is down, and it has said so */
    const provider_def *hp = NULL, *p;
    for (int i = 0; (p = provider_at(i)); i++) if (p->style == PROVIDER_MESSAGES && !hp) hp = p;
    const char *why = NULL;
    cJSON *hosted = hp ? model_list_get(provider_slot_of(hp), hp, &why) : NULL;
    int nl = list ? cJSON_GetArraySize(list) : 0, nh = hosted ? cJSON_GetArraySize(hosted) : 0, n = nl + nh;
    if (hp && !hosted && why) printf(C_DIM "%s models: %s" C_RESET "\n", hp->label, why);
    if (n == 0) { if (list) printf(C_YELLOW "no models installed — run: ollama pull <model>" C_RESET "\n"); cJSON_Delete(list); return; }
    const char **names = xmalloc(sizeof(char*) * (size_t)n);
    const char **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **own = xmalloc(sizeof(char*) * (size_t)n * 2);   /* what this function allocated: descriptions, and the hosted names */
    int nown = 0, current = -1;
    for (int i = 0; i < nl; i++) {
        cJSON *m = cJSON_GetArrayItem(list, i);
        names[i] = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring;
        bool tools = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools"));
        bool think = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "thinking"));
        const char *size = cJSON_GetObjectItemCaseSensitive(m, "size")->valuestring;
        sbuf d; sb_init(&d);
        sb_printf(&d, "%-7s %s%s", size, tools ? "tools" : "chat-only", think ? " · thinking" : "");
        char *dt = sb_detach(&d); own[nown++] = dt; descs[i] = dt;
        if (g_cfg.model && !strcmp(names[i], g_cfg.model)) current = i;
    }
    for (int j = 0; j < nh; j++) {
        int i = nl + j;
        cJSON *m = cJSON_GetArrayItem(hosted, j);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id"), *name = cJSON_GetObjectItemCaseSensitive(m, "name");
        if (!cJSON_IsString(id)) { names[i] = descs[i] = ""; continue; }
        sbuf nm; sb_init(&nm); sb_printf(&nm, "%s:%s", hp->name, id->valuestring);
        char *nt = sb_detach(&nm); own[nown++] = nt; names[i] = nt;
        sbuf d; sb_init(&d);
        sb_printf(&d, "%s%s%s", hp->label, cJSON_IsString(name) ? " · " : "", cJSON_IsString(name) ? name->valuestring : "");
        char *dt = sb_detach(&d); own[nown++] = dt; descs[i] = dt;
        if (g_cfg.model && !strcmp(names[i], g_cfg.model)) current = i;
    }
    int r = term_select("Select model", names, descs, n, current);
    if (r >= 0) {
        free(g_cfg.model); g_cfg.model = xstrdup(names[r]);
        refresh_model_caps(false); config_save();
        printf(C_GREEN "✓ model set to %s" C_RESET "%s%s\n", g_cfg.model, g_model_tools ? "" : C_DIM " (chat-only: no tool support)" C_RESET, model_effort_note());
    } else printf(C_DIM "model unchanged: %s" C_RESET "\n", g_cfg.model);
    for (int i = 0; i < nown; i++) free(own[i]);
    free(own); free(names); free(descs); cJSON_Delete(list);   /* (the hosted list is the cache's: not freed here) */
}

/* ---------- /effort: how hard a model thinks ----------
 * /think says *when* the model thinks (every call, the first call of a request, never); /effort
 * says how hard, in the levels that model has — see model_info and think_decide(). It is kept
 * per model, and the same command serves the advisor's model (/advisor effort). */
static const char *effort_in_use(const char *model, const model_info *mi) {   /* what is sent; NULL = left to the model */
    return effort_resolve(mi, effort_get(model), NULL);
}

/* "off · low · medium · high" */
static void effort_choices(const model_info *mi, sbuf *b) {
    if (mi->think_off) sb_puts(b, "off");
    if (mi->think_on) sb_printf(b, "%son", b->len ? " · " : "");
    for (int i = 0; i < mi->n_think_levels; i++) sb_printf(b, "%s%s", b->len ? " · " : "", mi->think_levels[i]);
}

static const char *effort_label(const char *model, const model_info *mi) {
    static char b[96];
    const char *e = effort_in_use(model, mi);
    if (!mi->thinking) snprintf(b, sizeof b, "none (the model cannot think)");
    else if (e) snprintf(b, sizeof b, "%s", e);
    else snprintf(b, sizeof b, "default%s%s%s", mi->think_default[0] ? " (" : "", mi->think_default, mi->think_default[0] ? ")" : "");
    return b;
}

static const char *think_label(void) {
    static char b[160];
    const char *base = g_cfg.think < 0 ? "auto (first call of a request only)" : g_cfg.think ? "on (every call)" : "off";
    const char *e = effort_in_use(g_cfg.model, &g_model_info);
    bool own = e && strcmp(e, "off") && strcmp(e, "on") && effort_supported(&g_model_info, e) == 1;
    if (own && g_cfg.think < 0) snprintf(b, sizeof b, "auto — but effort %s goes with every call (see /effort)", e);
    else if (e && g_cfg.think) snprintf(b, sizeof b, "%s · effort %s", base, e);
    else snprintf(b, sizeof b, "%s", base);
    return b;
}

static void effort_report(const char *who, const char *model, const model_info *mi) {
    if (!mi->thinking) { printf("%seffort: " C_DIM "%s cannot think, so there is nothing to set — models that can are marked 'thinking' in /models" C_RESET "\n", who, model); return; }
    sbuf c; sb_init(&c); effort_choices(mi, &c);
    printf("%seffort: " C_BOLD "%s" C_RESET C_DIM " · %s offers: %s%s" C_RESET "\n", who, effort_label(model, mi), model, c.data ? c.data : "",
           mi->think_declared || mi->n_think_levels ? "" : " (no levels of its own, as far as the server says)");
    sb_free(&c);
}

/* Set it, or say why not. Returns true when something was set. */
static bool effort_apply(const char *who, const char *model, const model_info *mi, const char *level) {
    if (!strcmp(level, "default") || !strcmp(level, "reset")) {
        effort_set(model, NULL); config_save();
        printf(C_GREEN "✓ %seffort: %s" C_RESET C_DIM " — left to %s" C_RESET "\n", who, effort_label(model, mi), model);
        return true;
    }
    bool mapped = false;
    const char *sent = mi->thinking ? effort_resolve(mi, level, &mapped) : NULL;
    int ok = sent ? (mapped ? 1 : effort_supported(mi, level)) : 0;
    if (ok == 0) {
        sbuf c; sb_init(&c); effort_choices(mi, &c);
        if (!mi->thinking) printf(C_RED "%s cannot think" C_RESET " — there is no effort to set for it\n", model);
        else if (!strcmp(level, "off") && !mi->think_off) printf(C_RED "%s cannot stop thinking" C_RESET " — its levels: %s\n", model, c.data ? c.data : "");
        else printf(C_RED "%s has no effort '%s'" C_RESET " — it offers: %s · default\n", model, level, c.data ? c.data : "");
        sb_free(&c);
        return false;
    }
    effort_set(model, level);
    bool woke = false;   /* a level is no use to the model doing the work while /think is off; another model's call never reads /think */
    if (strcmp(level, "off") && g_cfg.think == 0 && model_same(model, g_cfg.model)) { g_cfg.think = -1; woke = true; }
    config_save();
    printf(C_GREEN "✓ %seffort: %s" C_RESET C_DIM " for %s%s" C_RESET "\n", who, level, model, woke ? " · /think was off: now auto" : "");
    if (mapped) printf(C_DIM "  sent as %s, its strongest level" C_RESET "\n", sent);
    if (ok < 0) printf(C_DIM "  %s is not known to act on levels: the server takes '%s' as plain on" C_RESET "\n", model, level);
    else if (strcmp(level, "off") && strcmp(level, "on") && g_cfg.think < 0 && !*who)
        printf(C_DIM "  sent with every model call: the level is written into the top of the prompt, so dropping it for the tool rounds (/think auto) would make the server read the conversation again each time" C_RESET "\n");
    return true;
}

static void effort_command(const char *who, const char *model, const model_info *mi, const char *arg) {
    if (arg) { effort_apply(who, model, mi, arg); return; }
    if (!mi->thinking || !g_cfg.interactive || g_while_busy) { effort_report(who, model, mi); return; }
    const char *items[EFFORT_LEVELS_MAX + 3], *descs[EFFORT_LEVELS_MAX + 3];
    char dbuf[EFFORT_LEVELS_MAX + 3][160];
    const char *cur = effort_in_use(model, mi);
    int n = 0, current = 0;
    #define ITEM(name, ...) do { items[n] = (name); snprintf(dbuf[n], sizeof dbuf[n], __VA_ARGS__); \
        if (mi->think_default[0] && !strcmp(mi->think_default, (name))) strncat(dbuf[n], " · what the model does by itself", sizeof dbuf[n] - strlen(dbuf[n]) - 1); \
        if (cur && !strcmp(cur, (name))) { strncat(dbuf[n], " · current", sizeof dbuf[n] - strlen(dbuf[n]) - 1); current = n; } \
        descs[n] = dbuf[n]; n++; } while (0)
    ITEM("default", "leave it to the model%s%s%s", mi->think_default[0] ? " (" : "", mi->think_default, mi->think_default[0] ? ")" : "");
    if (!cur) strncat(dbuf[0], " · current", sizeof dbuf[0] - strlen(dbuf[0]) - 1);
    if (mi->think_off) ITEM("off", "no thinking: fastest");
    if (mi->think_on) ITEM("on", mi->n_think_levels ? "think" : "think — this model has no levels");
    for (int i = 0; i < mi->n_think_levels; i++)
        ITEM(mi->think_levels[i], "%s", i == 0 ? "the least thinking" : i == mi->n_think_levels - 1 ? "the most thinking: slowest" : "");
    #undef ITEM
    char title[256]; snprintf(title, sizeof title, "%s%sffort for %s", who, *who ? "e" : "E", model);
    int r = term_select(title, items, descs, n, current);
    if (r < 0) printf(C_DIM "%seffort unchanged: %s" C_RESET "\n", who, effort_label(model, mi));
    else effort_apply(who, model, mi, items[r]);
}
static void cmd_effort(const char *arg) { effort_command("", g_cfg.model, &g_model_info, arg); }

/* /subagent [MODEL|default] and /subagent effort [LEVEL|default]: the model sub-agents (task) run on, and
 * how hard it thinks. A model is asked of its server first, so a wrong name is refused there and then. */
static void subagent_report(void) {
    const char *m = subagent_model();
    printf("sub-agents run on " C_BOLD "%s" C_RESET "%s\n", m ? m : g_cfg.model ? g_cfg.model : "(no model)",
           m ? "" : C_DIM " — the model doing the work" C_RESET);
    if (m && !(g_subagent_info_for && !strcmp(g_subagent_info_for, m)))
        printf(C_DIM "  its levels are read when a sub-agent first runs on it, or with /subagent %s" C_RESET "\n", m);
    else if (g_cfg.model)
        printf(C_DIM "  effort: %s" C_RESET "\n", effort_label(m ? m : g_cfg.model, m ? &g_subagent_info : &g_model_info));
}
static void cmd_subagent(const char *arg) {
    if (!arg) { subagent_report(); return; }
    if (!strncmp(arg, "effort", 6) && (arg[6] == ' ' || !arg[6])) {
        const char *v = arg + 6; while (*v == ' ') v++;
        if (!*v) { subagent_report(); return; }
        if (!g_cfg.model) { printf(C_DIM "no model is set" C_RESET "\n"); return; }
        if (!subagent_model()) { effort_apply("subagent ", g_cfg.model, &g_model_info, v); return; }   /* the same model: its effort is the one it has */
        if (!subagent_info()) { printf(C_RED "✗ %s" C_RESET "\n", g_subagent_err); return; }
        effort_apply("subagent ", subagent_model(), &g_subagent_info, v);
        return;
    }
    if (!strcmp(arg, "default")) {
        free(g_cfg.subagent); g_cfg.subagent = NULL; free(g_subagent_info_for); g_subagent_info_for = NULL;
        config_save();
        printf(C_GREEN "✓ sub-agents run on the model doing the work" C_RESET "\n");
        return;
    }
    const provider_def *pv = provider_find(arg, NULL);
    if (pv && pv->style != PROVIDER_MESSAGES) { printf(C_RED "✗ %s is an advisor only: the sub-agents need a model that does the work (a local one, or anthropic:)" C_RESET "\n", pv->label); return; }
    char *prev = g_cfg.subagent;
    g_cfg.subagent = xstrdup(arg);
    if (!subagent_info()) { free(g_cfg.subagent); g_cfg.subagent = prev; printf(C_RED "✗ %s" C_RESET "\n", g_subagent_err); return; }
    free(prev); config_save();
    printf(C_GREEN "✓ sub-agents run on %s" C_RESET "%s\n", arg, model_same(arg, g_cfg.model) ? C_DIM " — the model doing the work" C_RESET : "");
}
static void cmd_status(void) {
    printf(C_BOLD "model      " C_RESET "%s%s\n", g_cfg.model, g_model_tools ? "" : C_DIM " (no tool support)" C_RESET);
    if (model_hosted()) {
        const provider_def *pv = provider_find(g_cfg.model, NULL);
        printf(C_BOLD "api        " C_RESET "%s\n", pv ? provider_base_url(pv) : "");
    } else printf(C_BOLD "host       " C_RESET "%s\n", g_cfg.host);
    printf(C_BOLD "cwd        " C_RESET "%s\n", g_cwd);
    printf(C_BOLD "messages   " C_RESET "%d\n", cJSON_GetArraySize(g_messages));
    printf(C_BOLD "%s" C_RESET "last prompt %d tokens", model_hosted() ? "budget     " : "context    ", g_session.last_prompt_tokens);
    if (g_cfg.num_ctx > 0) printf(" of %d (%d%%)", g_cfg.num_ctx, (int)(100.0 * g_session.last_prompt_tokens / g_cfg.num_ctx)); else printf(" (num_ctx: server default)");
    if (g_model_max_ctx > 0) printf(" · model max %d%s", g_model_max_ctx, g_model_max_ctx > g_cfg.num_ctx ? " (/ctx to enlarge)" : "");
    printf("\n" C_BOLD "tokens     " C_RESET "%ld this session (%ld in · %ld generated)\n", g_session.prompt_tokens + g_session.eval_tokens, g_session.prompt_tokens, g_session.eval_tokens);
    printf(C_BOLD "tools      " C_RESET "%s%s\n", (g_cfg.no_tools || !g_model_tools) ? "off" : "on",
           (g_cfg.no_tools || !g_model_tools) ? "" : (g_cfg.web ? C_DIM " · web on" C_RESET : C_DIM " · web off" C_RESET));
    printf(C_BOLD "mode       " C_RESET "%s%s" C_RESET "\n", g_cfg.mode == MODE_AUTO ? C_RED : g_cfg.mode == MODE_PLAN ? C_CYAN : g_cfg.mode == MODE_ACCEPT_EDITS ? C_ORANGE : "", mode_label(g_cfg.mode));
    printf(C_BOLD "think      " C_RESET "%s, %s\n", g_cfg.think < 0 ? "auto (first call of a request only)" : g_cfg.think ? "on (every call)" : "off", g_cfg.show_thinking ? "shown" : "hidden");
    { sbuf c; sb_init(&c); effort_choices(&g_model_info, &c);
      printf(C_BOLD "effort     " C_RESET "%s%s%s" C_RESET "\n", effort_label(g_cfg.model, &g_model_info), c.len ? C_DIM " · /effort: " : "", c.len ? c.data : ""); sb_free(&c); }
    if (g_cfg.advisor) printf(C_BOLD "advisor    " C_RESET "%s" C_DIM " · guidance %s · %d consultation%s this session%s" C_RESET "\n", g_cfg.advisor, advisor_guidance()->name,
                              g_session.advisor_calls, g_session.advisor_calls == 1 ? "" : "s", g_model_tools && !g_cfg.no_tools ? "" : " · idle: the agent has no tools to ask it with");
    printf(C_BOLD "temp       " C_RESET "%s", g_cfg.temperature < 0 ? "default\n" : ""); if (g_cfg.temperature >= 0) printf("%g\n", g_cfg.temperature);
    if (model_hosted()) {
        printf(C_BOLD "keep_alive " C_RESET C_DIM "unused — a hosted model is not kept loaded" C_RESET "\n");
        printf(C_BOLD "draft      " C_RESET C_DIM "unused — a hosted model has no draft model" C_RESET "\n");
    } else {
        printf(C_BOLD "keep_alive " C_RESET "%s\n", g_cfg.keep_alive ? g_cfg.keep_alive : "server default (5m)");
        if (g_cfg.draft >= 0) printf(C_BOLD "draft      " C_RESET "%d (draft_num_predict, %s)\n", g_cfg.draft, g_cfg.draft ? "speculative decoding / MTP" : "speculative decoding off");
        check_model_placement(true);
    }
    if (g_project_instructions) printf(C_BOLD "project    " C_RESET "instructions file loaded\n");
    printf(C_BOLD "memory     " C_RESET "%s%s\n", g_cfg.memory ? "on" : "off", g_memory ? " · " MEMORY_PATH " loaded" : "");
    if (g_session_id[0]) printf(C_BOLD "session    " C_RESET "%s · resume later with: corbienest --resume %s\n", g_session_id, g_session_id);
    else printf(C_BOLD "session    " C_RESET "(nothing saved yet)\n");
}

/* Summarise the conversation and start again from the summary. Returns true when it did.
 *
 * `resuming` says who speaks next. At the prompt (false) the compacted conversation ends with
 * an assistant turn, so whatever the user types next follows one. In the middle of a turn
 * (true) it must end with a user turn instead: the model has a job half done — a tool result
 * it never got to read, a task several rounds in — and a conversation that ends with the
 * assistant asking "how should we continue?" makes it stop and wait for an answer that is
 * never coming, dropping the work. So the summary is handed back as a request to carry on,
 * with `request` (the user's own words, if we still have them) appended. */
static bool compact_conversation(bool resuming, const char *request) {
    if (cJSON_GetArraySize(g_messages) == 0) { printf(C_DIM "nothing to compact" C_RESET "\n"); return false; }
    /* a copy: `request` points into the conversation this is about to delete */
    char *ask = NULL; bool ask_cut = false;
    if (request && *request) {
        size_t len = strlen(request), keep = len > COMPACT_REQUEST_MAX ? COMPACT_REQUEST_MAX : len;
        while (keep && keep < len && ((unsigned char)request[keep] & 0xC0) == 0x80) keep--;   /* UTF-8 boundary */
        ask = xstrndup(request, keep); ask_cut = keep < len;
    }
    memory_flush();   /* learn from the detail before it is summarised away */
    printf(C_DIM "compacting conversation…" C_RESET "\n");
    cJSON *msgs = messages_with_system();
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content",
        "Write a detailed summary of our conversation so far, to be used as context for continuing the work. "
        "Include: the user's goals and requests, what has been done (files read/changed, commands run, results), key findings, "
        "decisions made, and what remains to be done. Be specific about file paths and code details. Output only the summary.");
    cJSON_AddItemToArray(msgs, u);
    chat_stats st; bool aborted;
    ollama_call.think = 0; ollama_call.followup = true; ollama_call.busy = "compacting";   /* a summary needs no thinking; keep it quick — but it is the conversation's own prompt */
    cJSON *reply = model_chat(msgs, NULL, &st, &aborted);
    ollama_call_reset();
    cJSON_Delete(msgs);
    account(&st);
    term_status_refresh();
    if (!reply || aborted) { free(ask); if (reply) cJSON_Delete(reply); printf(C_YELLOW "compact cancelled" C_RESET "\n"); return false; }
    const char *summary = cJSON_GetObjectItemCaseSensitive(reply, "content")->valuestring;
    cJSON_Delete(g_messages); g_messages = cJSON_CreateArray(); g_prev_request_first = -1;
    sbuf b; sb_init(&b);
    sb_printf(&b, "This conversation was compacted. Summary of the previous conversation:\n\n%s\n\nContinue from here.", summary);
    if (resuming) {
        sb_puts(&b, " The work in the summary is unfinished: pick it up where it left off and carry on until it is done. "
                    "Do not start over, do not summarise what happened, and do not ask what to do next — act.");
        if (ask) sb_printf(&b, "\n\nThe request you are working on, as it was made:\n\n%s%s", ask, ask_cut ? "\n[…truncated]" : "");
    }
    add_message("user", b.data);
    if (!resuming) add_message("assistant", "Understood, I have the context from the summary. How should we continue?");
    sb_free(&b); free(ask); cJSON_Delete(reply);
    g_session.last_prompt_tokens = 0;   /* unknown until the next request; also stops auto-compact re-firing */
    printf(C_GREEN "✓ conversation compacted%s" C_RESET "\n", resuming ? " — continuing" : "");
    return true;
}

/* Auto-compact: once the last request used AUTO_COMPACT_PCT% or more of the context
 * window, summarise the conversation before sending anything else. A failed attempt
 * is not retried until the usage figure changes (a new request came through).
 * `resuming`/`request`: see compact_conversation(). */
static bool maybe_auto_compact(bool resuming, const char *request) {
    static int tried_at = -1;
    if (g_cfg.num_ctx <= 0) return false;
    int est = ctx_estimate(), pct = (int)(100.0 * est / g_cfg.num_ctx);
    if (pct < AUTO_COMPACT_PCT || est == tried_at) return false;
    tried_at = est;
    printf(C_YELLOW "⚠ context %d%% full — auto-compacting" C_RESET "\n", pct);
    return compact_conversation(resuming, request);
}

static void cmd_save(const char *arg) {
    char path[PATH_MAX];
    if (arg && *arg) snprintf(path, sizeof path, "%s", arg);
    else { time_t t = time(NULL); strftime(path, sizeof path, "corbienest-%Y%m%d-%H%M%S.md", localtime(&t)); }
    sbuf b; sb_init(&b);
    sb_printf(&b, "# corbienest transcript (%s)\n\n", g_cfg.model);
    cJSON *m; cJSON_ArrayForEach(m, g_messages) {
        const char *role = cJSON_GetObjectItemCaseSensitive(m, "role")->valuestring;
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!strcmp(role, "tool")) { cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name"); sb_printf(&b, "<details><summary>tool result: %s</summary>\n\n```\n%s\n```\n</details>\n\n", cJSON_IsString(tn) ? tn->valuestring : "?", c && cJSON_IsString(c) ? c->valuestring : ""); continue; }
        sb_printf(&b, "## %s\n\n%s\n\n", role, c && cJSON_IsString(c) ? c->valuestring : "");
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        if (tc) { char *s = cJSON_PrintUnformatted(tc); sb_printf(&b, "```json\n%s\n```\n\n", s); free(s); }
    }
    if (write_whole_file(path, b.data, b.len) == 0) printf(C_GREEN "✓ saved %s" C_RESET "\n", path);
    else printf(C_RED "✗ cannot write %s: %s" C_RESET "\n", path, strerror(errno));
    sb_free(&b);
}

/* /history [N]: the most recent queries, oldest first (↑/↓ at the prompt recalls them) */
static void cmd_history(const char *arg) {
    int total = hist_count();
    if (!total) { printf(C_DIM "no history yet" C_RESET "\n"); return; }
    int want = arg ? atoi(arg) : 20;
    if (want <= 0) want = 20;
    if (want > total) want = total;
    printf(C_BOLD "last %d of %d quer%s" C_RESET " " C_DIM "(↑/↓ at the prompt recalls them · the latest 100 are kept)" C_RESET "\n", want, total, total == 1 ? "y" : "ies");
    int w = term_width() - 8; if (w < 20) w = 20;
    for (int i = total - want; i < total; i++) {
        const char *q = hist_get(i);
        size_t n = strcspn(q, "\n"), cut = n > (size_t)w ? (size_t)w : n;
        while (cut > 0 && cut < n && ((unsigned char)q[cut] & 0xC0) == 0x80) cut--;
        printf(C_DIM "%4d" C_RESET "  %.*s%s\n", i + 1, (int)cut, q, cut < n || q[n] ? "…" : "");
    }
}

/* returns 1 to quit, -1 if a skill's model turn was interrupted */
static void host_warning(const char *lead);
/* the command line the program was started with: /ui starts it again with the screen changed */
static char **g_argv = NULL;
static int g_argc = 0;

/* /ui [ansi|notcurses]: the choice is saved, the session with it, and the program starts again on the
 * same session (--resume) with the other screen. Only from the prompt: a turn in flight keeps its screen. */
/* /ui theme: the picker (a modal list of the themes, the one in use marked), or /ui theme NAME. The theme is saved,
 * and the screen takes it at once: the conversation is painted again from the model, the notcurses screen draws next frame. */
static void cmd_ui_theme(const char *name) {
    int n = theme_count(), cur = 0;
    const char *items[32], *descs[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        items[i] = theme_name(i);
        descs[i] = !strcmp(theme_name(i), g_theme->name) ? "in use" : "";
        if (!strcmp(theme_name(i), g_theme->name)) cur = i;
    }
    if (!name) {
        int pick = term_select("Colour theme", items, descs, n, cur);
        if (pick < 0) return;
        name = items[pick];
    }
    if (!theme_set(name)) { printf("no theme named %s (themes: ", name); for (int i = 0; i < n; i++) printf("%s%s", i ? ", " : "", items[i]); printf(")\n"); return; }
    free(g_cfg.theme); g_cfg.theme = !strcmp(name, "default") ? NULL : xstrdup(name);
    config_save();
    term_repaint();
    printf("theme: %s\n", name);
}

static void cmd_ui(const char *arg) {
    const char *now = g_cfg.ui ? "notcurses" : "ansi";
    if (arg && !strncmp(arg, "theme", 5) && (arg[5] == ' ' || !arg[5])) {
        const char *name = arg[5] ? arg + 5 : NULL;
        while (name && *name == ' ') name++;
        cmd_ui_theme(name && *name ? name : NULL);
        return;
    }
    if (!arg) {
        printf("screen: %s%s" C_RESET "%s\n", now, g_ui_nc ? " (running)" : "",
               nc_compiled() ? "" : C_DIM "  · notcurses is not in this build (it was not installed when built)" C_RESET);
        printf("theme: %s\n", g_theme->name);
        printf(C_DIM "  /ui ansi | /ui notcurses | /ui theme [NAME]" C_RESET "\n");
        return;
    }
    if (strcmp(arg, "ansi") && strcmp(arg, "notcurses")) { printf("usage: /ui ansi|notcurses\n"); return; }
    if (!strcmp(arg, "notcurses") && !nc_compiled()) {
        printf(C_YELLOW "the notcurses screen is not in this build: install notcurses (libnotcurses-dev) and run make again" C_RESET "\n");
        return;
    }
    if (!strcmp(arg, now) && (g_ui_nc == (!strcmp(arg, "notcurses")))) { printf("already on the %s screen\n", arg); return; }
    free(g_cfg.ui); g_cfg.ui = !strcmp(arg, "notcurses") ? xstrdup("notcurses") : NULL;
    config_save();
    if (g_mem_pending) memory_flush();
    session_save();
    /* the same command line, with --ui and --resume replaced */
    char **na = xmalloc(sizeof *na * (size_t)(g_argc + 6));
    int n = 0;
    na[n++] = g_argv[0];   /* execv's argv[0] */
    for (int i = 1; i < g_argc; i++) {
        const char *a = g_argv[i];
        if (!strcmp(a, "--ui")) { i++; continue; }
        if (!strcmp(a, "--continue")) continue;
        if (!strcmp(a, "-r") || !strcmp(a, "--resume")) { if (i + 1 < g_argc && g_argv[i + 1][0] != '-') i++; continue; }
        na[n++] = g_argv[i];
    }
    na[n++] = "--ui"; na[n++] = (char *)arg;
    na[n++] = "--resume"; na[n++] = g_session_id;
    na[n] = NULL;
    printf(C_DIM "restarting on the %s screen, in this session…" C_RESET "\n", arg);
    fflush(stdout);
    term_restore();
    execv("/proc/self/exe", na);
    /* only reached when the restart failed: the session is saved, and the program is still running */
    fprintf(stderr, "corbienest: could not restart (%s); still on the old screen — restart it with --resume %s\n", strerror(errno), g_session_id);
    if (g_ui_nc) term_fullscreen(true);   /* the notcurses screen was given back above: take it again */
    free(na);
}

static void cmd_banner(void);

static int handle_slash(char *line) {
    char *cmd = line, *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; if (!*arg) arg = NULL; }
    if (!strcmp(cmd, "/help") || !strcmp(cmd, "/?")) cmd_help();
    else if (!strcmp(cmd, "/quit") || !strcmp(cmd, "/exit") || !strcmp(cmd, "/q")) return 1;
    else if (!strcmp(cmd, "/models")) cmd_models();
    else if (!strcmp(cmd, "/model")) {
        if (!arg) cmd_model_picker();
        else {
            char *prev = g_cfg.model;
            g_cfg.model = xstrdup(arg);
            refresh_model_caps(false);
            if (g_model_bad) {
                printf(C_DIM "  model left as %s" C_RESET "\n", prev ? prev : "(none)");
                free(g_cfg.model); g_cfg.model = prev;
                refresh_model_caps(true);
            } else {
                free(prev); config_save();
                printf(C_GREEN "✓ model set to %s" C_RESET "%s\n", g_cfg.model, model_effort_note());
            }
        }
    }
    else if (!strcmp(cmd, "/clear") || !strcmp(cmd, "/new")) { memory_flush(); cJSON_Delete(g_messages); g_messages = cJSON_CreateArray(); typed_clear(); tools_todos_set(NULL); g_prev_request_first = -1; tools_reset_permissions(); tools_checkpoint_clear(); g_session.last_prompt_tokens = 0; g_session_id[0] = 0; term_clear_screen(); printf(C_GREEN "✓ new conversation" C_RESET "\n"); }
    else if (!strcmp(cmd, "/compact")) { if (cmd_compact()) session_save(); }
    else if (!strcmp(cmd, "/resume")) cmd_resume(arg);
    else if (!strcmp(cmd, "/permissions")) cmd_permissions(arg);
    else if (!strcmp(cmd, "/hooks")) cmd_hooks(arg);
    else if (!strcmp(cmd, "/cost")) cmd_cost();
    else if (!strcmp(cmd, "/usage")) cmd_usage(arg);
    else if (!strcmp(cmd, "/banner")) cmd_banner();
    else if (!strcmp(cmd, "/diff")) cmd_diff(arg);
    else if (!strcmp(cmd, "/rewind")) return cmd_rewind();
    else if (!strcmp(cmd, "/init")) return cmd_init();
    else if (!strcmp(cmd, "/orchestrate")) return orchestrate_run(arg, false);
    else if (!strcmp(cmd, "/plan")) return orchestrate_run(arg, true);
    else if (!strcmp(cmd, "/goal")) return cmd_goal(arg);
    else if (!strcmp(cmd, "/btw")) return cmd_btw(arg);
    else if (!strcmp(cmd, "/status") || !strcmp(cmd, "/cost")) cmd_status();
    else if (!strcmp(cmd, "/system")) {
        if (!arg) printf("extra system prompt: %s\n", g_cfg.system_prompt ? g_cfg.system_prompt : C_DIM "(none)" C_RESET);
        else if (!strcmp(arg, "clear")) { free(g_cfg.system_prompt); g_cfg.system_prompt = NULL; printf(C_GREEN "✓ cleared" C_RESET "\n"); }
        else { free(g_cfg.system_prompt); g_cfg.system_prompt = xstrdup(arg); printf(C_GREEN "✓ set" C_RESET "\n"); }
    }
    else if (!strcmp(cmd, "/think")) {
        if (!arg) printf("think: %s, %s\n", think_label(), g_cfg.show_thinking ? "shown" : "hidden");
        else if (!strcmp(arg, "low") || !strcmp(arg, "medium") || !strcmp(arg, "high") || !strcmp(arg, "max")) { cmd_effort(arg); return 0; }   /* how hard is /effort's */
        else if (!strcmp(arg, "on") || !strcmp(arg, "off") || !strcmp(arg, "auto") || !strcmp(arg, "first")) {
            g_cfg.think = !strcmp(arg, "on") ? 1 : !strcmp(arg, "off") ? 0 : -1;
            /* asking for thinking takes back an "/effort off" for this model, or nothing would change */
            const char *e = effort_get(g_cfg.model);
            if (g_cfg.think && e && !strcmp(e, "off")) { effort_set(g_cfg.model, NULL); printf(C_DIM "  (/effort off for %s is forgotten, or nothing would change)" C_RESET "\n", g_cfg.model); }
            if (!g_cfg.think && g_model_info.thinking && !g_model_info.think_off)
                printf(C_DIM "  %s cannot stop thinking: it is sent its weakest level (%s) instead" C_RESET "\n", g_cfg.model, g_model_info.n_think_levels ? g_model_info.think_levels[0] : "none known");
        }
        else if (!strcmp(arg, "show")) g_cfg.show_thinking = true;
        else if (!strcmp(arg, "hide")) g_cfg.show_thinking = false;
        else { printf("usage: /think on|off|auto|show|hide\n  auto = think once per request, not after every tool result (default) · how hard the model thinks is /effort\n"); return 0; }
        if (arg) { config_save(); printf(C_GREEN "✓ think: %s, %s" C_RESET "\n", think_label(), g_cfg.show_thinking ? "shown" : "hidden"); }
    }
    else if (!strcmp(cmd, "/effort")) cmd_effort(arg);
    else if (!strcmp(cmd, "/advisor")) cmd_advisor(arg);
    else if (!strcmp(cmd, "/subagent")) cmd_subagent(arg);
    else if (!strcmp(cmd, "/yolo")) {
        bool on = !arg ? !YOLO() : !strcmp(arg, "on");
        g_cfg.mode = on ? MODE_AUTO : MODE_MANUAL;
        config_save();
        printf(on ? C_RED "⚠ auto (yolo) mode ON — all tool calls auto-approved" C_RESET "\n" : C_GREEN "✓ auto mode off — back to manual confirmations" C_RESET "\n");
    }
    else if (!strcmp(cmd, "/mode")) {
        if (!arg) {
            printf("mode: " C_BOLD "%s" C_RESET "\n" C_DIM "available: manual, accept-edits, plan, auto (shift+tab cycles)" C_RESET "\n", mode_label(g_cfg.mode));
        } else {
            int m = mode_parse(arg);
            if (m < 0) printf(C_RED "unknown mode '%s'" C_RESET " — use manual, accept-edits, plan or auto\n", arg);
            else { g_cfg.mode = m; config_save(); printf("%s✓ mode: %s" C_RESET "\n", m == MODE_AUTO ? C_RED : C_GREEN, mode_label(m)); }
        }
    }
    else if (!strcmp(cmd, "/tools")) {
        if (!arg) printf("tools: %s\n", g_cfg.no_tools ? "off" : "on"); else { g_cfg.no_tools = !strcmp(arg, "off"); printf(C_GREEN "✓ tools %s" C_RESET "\n", g_cfg.no_tools ? "off" : "on"); }
    }
    else if (!strcmp(cmd, "/web")) {
        const char *engine = g_cfg.search_url ? g_cfg.search_url : SEARCH_URL_DEFAULT;
        if (!arg) printf("web_search/web_fetch: %s\n" C_DIM "engine: %s%s" C_RESET "\n",
                         g_cfg.web ? "on" : "off", engine, g_cfg.search_url ? "" : " (default)");
        else if (!strncmp(arg, "engine", 6)) {
            const char *v = arg + 6; while (*v == ' ') v++;
            if (!*v) printf("engine: %s\n" C_DIM "  /web engine URL (%%s = the query) · /web engine default" C_RESET "\n", engine);
            else {
                free(g_cfg.search_url);
                g_cfg.search_url = strcmp(v, "default") ? xstrdup(v) : NULL;
                config_save();
                printf(C_GREEN "✓ engine: %s" C_RESET "\n", g_cfg.search_url ? g_cfg.search_url : SEARCH_URL_DEFAULT);
            }
        }
        else if (strcmp(arg, "on") && strcmp(arg, "off")) printf("usage: /web on|off|engine URL\n");
        else {
            bool want = !strcmp(arg, "on");
            if (want != g_cfg.web) {   /* the tool list is built once at startup: rebuild it so the model sees the change */
                g_cfg.web = want;
                cJSON_Delete(g_tools); g_tools = tools_definitions(); g_tools_bytes = 0;
                config_save();
            }
            printf(C_GREEN "✓ web_search/web_fetch %s" C_RESET "\n", g_cfg.web ? "on" : "off");
        }
    }
    else if (!strcmp(cmd, "/workers")) {
        int now = g_cfg.workers > 0 ? g_cfg.workers : ORCH_WORKERS_DEFAULT, n = arg ? atoi(arg) : 0;
        if (!arg) printf("workers: %d" C_DIM " — tasks of /orchestrate that may run at the same time, where the plan lets them (1 = one after another)" C_RESET "\n", now);
        else if (n < 1 || n > ORCH_PARALLEL_MAX) printf("usage: /workers N   (1 to %d: tasks of /orchestrate that may run at the same time)\n", ORCH_PARALLEL_MAX);
        else {
            g_cfg.workers = n; config_save();
            printf(C_GREEN "✓ workers = %d" C_RESET C_DIM "%s" C_RESET "\n", n, n > 1 ? " — whether they overlap is the server's to say too: an Ollama with one slot (OLLAMA_NUM_PARALLEL) answers them in turn" : " — one task after another");
        }
    }
    else if (!strcmp(cmd, "/ui")) cmd_ui(arg);
    else if (!strcmp(cmd, "/max_iters") || !strcmp(cmd, "/max-iters")) {
        if (!arg) printf("max_iters: %d tool rounds per request\n", g_cfg.max_iters);
        else {
            int n = atoi(arg);
            if (n < 1) printf("usage: /max_iters N   (N ≥ 1: tool rounds one request may run before the loop guard stops it)\n");
            else { g_cfg.max_iters = n; config_save(); printf(C_GREEN "✓ max_iters = %d" C_RESET C_DIM " tool rounds per request" C_RESET "\n", n); }
        }
    }
    else if (!strcmp(cmd, "/ctx") || !strcmp(cmd, "/context")) cmd_ctx(arg);
    else if (!strcmp(cmd, "/temp")) {
        if (!arg) printf("temperature: %g\n", g_cfg.temperature); else { g_cfg.temperature = atof(arg); config_save(); printf(C_GREEN "✓ temperature = %g" C_RESET "\n", g_cfg.temperature); }
    }
    else if (!strcmp(cmd, "/keepalive") || !strcmp(cmd, "/keep-alive")) {
        if (!arg) printf("keep_alive: %s\n", g_cfg.keep_alive ? g_cfg.keep_alive : "server default (5m)");
        else { free(g_cfg.keep_alive); g_cfg.keep_alive = strcmp(arg, "default") ? xstrdup(arg) : NULL; config_save();
               printf(C_GREEN "✓ keep_alive = %s" C_RESET " (applies from the next request)\n", g_cfg.keep_alive ? g_cfg.keep_alive : "server default"); }
    }
    else if (!strcmp(cmd, "/host")) {
        if (!arg) printf("host: %s\n", g_cfg.host); else { free(g_cfg.host); g_cfg.host = xstrdup(arg); config_save(); free(g_advisor_info_for); g_advisor_info_for = NULL; refresh_model_caps(false); printf(C_GREEN "✓ host = %s" C_RESET "\n", g_cfg.host); host_warning("  "); }
    }
    else if (!strcmp(cmd, "/save")) cmd_save(arg);
    else if (!strcmp(cmd, "/history")) cmd_history(arg);
    else if (!strcmp(cmd, "/pwd")) printf("%s\n", g_cwd);
    else if (!strcmp(cmd, "/cd")) {
        char *d = expand_home(arg ? arg : "~");
        memory_flush();   /* the memory file belongs to the directory we are leaving */
        if (chdir(d) == 0) { if (getcwd(g_cwd, sizeof g_cwd)) {} load_project_instructions(); load_memory(); tools_permissions_load(); tools_hooks_load(); skills_load(); printf(C_GREEN "✓ %s" C_RESET "\n", g_cwd); }
        else printf(C_RED "✗ cd %s: %s" C_RESET "\n", d, strerror(errno));
        free(d);
    }
    else if (!strcmp(cmd, "/skills")) cmd_skills(arg);
    else if (!strcmp(cmd, "/memory")) cmd_memory(arg);
    else {
        const skill_t *sk = skill_find(cmd);
        if (!sk) { printf(C_RED "unknown command %s" C_RESET " — try /help or /skills\n", cmd); return 0; }
        printf(C_DIM "  skill %s: %s" C_RESET "\n", sk->name, *sk->desc ? sk->desc : sk->path);
        char *prompt = skill_expand(sk, arg);
        char *msg = expand_mentions(prompt);
        int first = begin_request();
        { sbuf raw; sb_init(&raw); sb_printf(&raw, "/%s%s%s", sk->name, arg && *arg ? " " : "", arg ? arg : ""); typed_note(raw.data, msg); sb_free(&raw); }
        add_message("user", msg);
        free(msg); free(prompt);
        bool aborted = run_turn();
        session_save();
        memory_note(first, aborted);
        apply_pending();
        if (aborted) return -1;
    }
    return 0;
}

/* ---------- slash commands that do not have to wait for the turn ----------
 * Enter while the model works normally queues a message. A command that only reports
 * state or flips a setting can just as well run there and then — that is what the user
 * expects from /status, and a permission or mode change is only useful if it lands
 * before the next tool call (Shift+Tab already works that way). To qualify a command
 * must not: touch the conversation (/clear, /compact, /rewind, /resume, /save, /system,
 * /init and skills do), start an HTTP request while one is streaming — ollama.c's
 * callbacks are global (/model, /models, /ctx, /host, /memory update), ask the user
 * anything (the pickers), or change the ground under the running turn (/cd). */
/* A setting that changes the prompt or the tool list is not applied under a request in flight: it
 * is run when the next request starts. /think show|hide only change the display, so they run now. */
static bool slash_waits_for_next(const char *cmd, const char *arg) {
    if (!arg) return false;
    if (!strcmp(cmd, "/think")) return strcmp(arg, "show") && strcmp(arg, "hide");
    if (!strcmp(cmd, "/effort")) return true;
    if (!strcmp(cmd, "/advisor")) return !strncmp(arg, "window", 6) && (arg[6] == ' ' || !arg[6]);
    return false;
}

static bool slash_runs_while_busy(const char *cmd, const char *arg) {
    static const char *ok[] = {
        "/help", "/?", "/status", "/cost", "/usage", "/diff", "/history", "/pwd", "/skills",
        "/mode", "/yolo", "/permissions", "/tools", "/workers", "/max_iters", "/max-iters", "/temp",
        "/keepalive", "/keep-alive", "/memory", "/web", NULL };
    /* /web with an argument is not just a report: on|off rebuilds the tool list the running
     * turn is holding, and engine writes the config. Bare /web only prints. */
    if (!strcmp(cmd, "/web") && arg) return false;
    /* /ctx with a size only sets the window for the next request; bare /ctx opens the picker */
    if ((!strcmp(cmd, "/ctx") || !strcmp(cmd, "/context"))) return arg != NULL;
    /* /advisor with an argument asks the server about the model and rebuilds the tool list as
     * well; bare, it only reports. /effort never needs the server: what the model offers was
     * read when it was chosen, and while busy it prints the choices instead of asking. */
    /* /advisor guidance LEVEL and /advisor effort LEVEL run at once too (the first waits for the next request);
     * the other forms of /advisor ask the server or change the advisor: not while busy */
    if (!strcmp(cmd, "/advisor"))
        return !arg || (!strncmp(arg, "guidance", 8) && (arg[8] == ' ' || !arg[8])) || (!strncmp(arg, "effort", 6) && (arg[6] == ' ' || !arg[6]))
            || (!strncmp(arg, "ctx", 3) && (arg[3] == ' ' || !arg[3]));   /* the window of a consultation: read by the next one */
    /* the bare forms report; with an argument /think, /effort and /advisor window wait (slash_waits_for_next) */
    if (!strcmp(cmd, "/think")) return !arg || !strcmp(arg, "show") || !strcmp(arg, "hide");
    if (!strcmp(cmd, "/effort")) return !arg;
    if (!strcmp(cmd, "/subagent")) return !arg;   /* with a model: asked of its server, so it waits for the turn to end (like /model) */
    /* /ui theme NAME only sets the palette and repaints, which a resize does mid-turn too; the bare form opens a
     * picker, and /ui ansi|notcurses restart the program: none of those while busy */
    if (!strcmp(cmd, "/ui")) return arg && !strncmp(arg, "theme ", 6) && arg[6];
    /* /memory update — and "every N" once N requests are pending — runs the extraction call;
     * "idle N" only sets the delay for the next prompt, so it is safe mid-turn */
    if (!strcmp(cmd, "/memory") && arg && strcmp(arg, "on") && strcmp(arg, "off") && strcmp(arg, "clear")
        && strncmp(arg, "idle", 4)) return false;
    for (int i = 0; ok[i]; i++) if (!strcmp(cmd, ok[i])) return true;
    return false;
}

/* term.c hands us the line typed while busy: 1 = handled here, 0 = queue it as a message.
 * Runs from inside term_poll_interrupt(), i.e. in the middle of a stream or a tool. */
static int run_slash_while_busy(const char *line) {
    char *cmd = xstrndup(line, strcspn(line, " "));
    const char *arg = line + strlen(cmd); while (*arg == ' ') arg++;
    bool later = slash_waits_for_next(cmd, *arg ? arg : NULL);
    bool ok = later || slash_runs_while_busy(cmd, *arg ? arg : NULL);
    free(cmd);
    if (!ok) return 0;
    hist_add(line); hist_save();
    term_line_break();   /* the model may be mid-sentence */
    echo_queued(line);
    if (later) {
        g_pending_lines = xrealloc(g_pending_lines, sizeof *g_pending_lines * (size_t)(g_pending_n + 1));
        g_pending_lines[g_pending_n++] = xstrdup(line);
        printf(C_GREEN "✓ %s" C_RESET C_DIM " — from the next request" C_RESET "\n", line);
        term_status_refresh();
        return 1;
    }
    char *copy = xstrdup(line);
    g_while_busy = true;
    handle_slash(copy);
    g_while_busy = false;
    free(copy);
    printf("\n");
    term_status_refresh();
    return 1;
}

/* !cmd : run shell command directly, add to context */
static void handle_bang(const char *cmd) {
    cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", cmd);
    tools_no_confirm = true;   /* user typed it: no confirmation */
    sbuf out; sb_init(&out);
    printf(C_GREEN "●" C_RESET " " C_BOLD "bash" C_RESET "\n");
    tools_execute("bash", a, &out);
    tools_no_confirm = false;
    print_result_preview(out.data ? out.data : "", 40);
    sbuf m; sb_init(&m);
    sb_printf(&m, "I ran this shell command myself:\n$ %s\n\nOutput:\n%s", cmd, out.data ? out.data : "");
    { sbuf raw; sb_init(&raw); sb_printf(&raw, "!%s", cmd); typed_note(raw.data, m.data); sb_free(&raw); }
    add_message("user", m.data);
    add_message("assistant", "Noted the command output.");
    sb_free(&m); sb_free(&out); cJSON_Delete(a);
}

/* the conversation goes to the Ollama host whole: say so when that is another machine and the
 * way there is not encrypted (Ollama has no TLS of its own; a TLS proxy in front of it does) */
static void host_warning(const char *lead) {
    if (!url_cleartext(g_cfg.host) || url_is_local(g_cfg.host)) return;
    char h[256] = ""; url_hostname(g_cfg.host, h, sizeof h);
    printf("%s" C_YELLOW "⚠ plain http to %s: the conversation crosses the network unencrypted" C_RESET C_DIM " (an https:// host keeps it private)" C_RESET "\n", lead, h);
}

static bool ollama_needed(void) {   /* the working model is local, or the advisor is a model on the Ollama server */
    if (!model_set()) return g_cfg.advisor && !provider_find(g_cfg.advisor, NULL);   /* no model yet: only the advisor can need it */
    return !model_hosted() || (g_cfg.advisor && !provider_find(g_cfg.advisor, NULL));
}

/* The crow: 32x8 braille cells (a 64x32 dot silhouette), one picture per wing position, coloured per cell (SGR in the
 * string, so widths are the braille's). Frames 0-2 are the flap; frame 3 is the folded wing. */
#define CROW_ROWS 8
#define CROW_W 32   /* columns */
static const char *CROW[4][CROW_ROWS] = {
    {   /* wings up */
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;169;182;204m⢀⣴⣿⣷⡶⠂[0m⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⣀⣤⣶⣶⣶⣤[0m⠀⠀⠀[38;2;169;182;204m⣴⣿⣿⣿⡿⠁[0m⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "[38;2;107;120;144m⢀⣀⣠⣤⣴⣶⣿⣿⣿⣿⣿⣿⣿⣿⡇[38;2;169;182;204m⣠⣾⣿⣿⣿⣟[38;2;107;120;144m⣀[0m⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀[38;2;107;120;144m⠈⠉⠉⠛⠛⠛⣿⣿⣿⣿⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⣿[38;2;107;120;144m⣿⣿⣿⣆[0m⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⣽⣿⣿⣿[38;2;169;182;204m⣿⣿⣿⣿[38;2;107;120;144m⣿⣿⣿⣿⠟[0m⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠹⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⢿⣿⣶⣦⣤⣀⡀[0m⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⠉⣿⠉⢹⡏[0m⠀⠀⠀[38;2;107;120;144m⠘⠻⠿⠿⣿⣿⣿⣿⡖[0m⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;122;102;80m⣤⣤⣤⣤⣤[38;2;107;120;144m⣿[38;2;122;102;80m⣤[38;2;107;120;144m⣼⣧[38;2;122;102;80m⣤⣤⣤⣤⣤[0m⠀⠀⠀⠀[38;2;107;120;144m⠈⠉[0m⠀⠀[0m",
    },
    {   /* wings mid */
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⣀⣤⣶⣶⣶⣤[0m⠀⠀⠀⠀⠀⠀⠀⠀[38;2;169;182;204m⣀⣠⣄[0m⠀⠀⠀⠀⠀⠀⠀[0m",
        "[38;2;107;120;144m⢀⣀⣠⣤⣴⣶⣿⣿⣿⣿⣿⣿⣿⣿⡇[0m⠀[38;2;107;120;144m⣀[38;2;169;182;204m⣀⣠⣴⣶⣿⣿⣿⣿⣿⠆[0m⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀[38;2;107;120;144m⠈⠉⠉⠛⠛⠛⣿⣿⣿⣿⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠋[0m⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⣽⣿⣿⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⣿⣿⣿[38;2;107;120;144m⠟[0m⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠹⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⢿⣿⣶⣦⣤⣀⡀[0m⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⠉⣿⠉⢹⡏[0m⠀⠀⠀[38;2;107;120;144m⠘⠻⠿⠿⣿⣿⣿⣿⡖[0m⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;122;102;80m⣤⣤⣤⣤⣤[38;2;107;120;144m⣿[38;2;122;102;80m⣤[38;2;107;120;144m⣼⣧[38;2;122;102;80m⣤⣤⣤⣤⣤[0m⠀⠀⠀⠀[38;2;107;120;144m⠈⠉[0m⠀⠀[0m",
    },
    {   /* wings low */
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⣀⣤⣶⣶⣶⣤[0m⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "[38;2;107;120;144m⢀⣀⣠⣤⣴⣶⣿⣿⣿⣿⣿⣿⣿⣿⡇[0m⠀[38;2;107;120;144m⣀⣀⣀⣀⣀⣀[0m⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀[38;2;107;120;144m⠈⠉⠉⠛⠛⠛⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣆[0m⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⣽⣿⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⣿⣿⣿⣿⣟⣀⡀[0m⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠹⣿⣿⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⡿⠿⢿⣿⣿⣿⣯[38;2;107;120;144m⣀⡀[0m⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⠉⣿⠉⢹⡏[0m⠀⠀⠀[38;2;107;120;144m⠘⠻⠿[38;2;169;182;204m⠿[38;2;107;120;144m⣿⣿⣿⣿⡖[0m⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;122;102;80m⣤⣤⣤⣤⣤[38;2;107;120;144m⣿[38;2;122;102;80m⣤[38;2;107;120;144m⣼⣧[38;2;122;102;80m⣤⣤⣤⣤⣤[0m⠀⠀⠀⠀[38;2;107;120;144m⠈⠉[0m⠀⠀[0m",
    },
    {   /* wings folded (holds) */
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⣀⣤⣶⣶⣶⣤[0m⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "[38;2;107;120;144m⢀⣀⣠⣤⣴⣶⣿⣿⣿⣿⣿⣿⣿⣿⡇[0m⠀[38;2;107;120;144m⣀⣀⣀⣀⣀⣀[0m⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀[38;2;107;120;144m⠈⠉⠉⠛⠛⠛⣿⣿⣿⣿⣿[38;2;169;182;204m⣿⣿⣿[38;2;107;120;144m⣿⣿⣿⣿⣿⣿⣆[0m⠀⠀⠀⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⣽⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣶⣶⣄[0m⠀⠀⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠹⣿⣿⣿[38;2;169;182;204m⣿⣿⣿⣿⣿⡿⠿⢿⣿⣿⣿⣯⣁[38;2;107;120;144m⡀[0m⠀⠀⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;107;120;144m⠉⠉⣿⠉⢹⡏[0m⠀⠀⠀[38;2;107;120;144m⠘⠻⠿⠿⣿⣿⣿⣿⡖[0m⠀[0m",
        "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀[38;2;122;102;80m⣤⣤⣤⣤⣤[38;2;107;120;144m⣿[38;2;122;102;80m⣤[38;2;107;120;144m⣼⣧[38;2;122;102;80m⣤⣤⣤⣤⣤[0m⠀⠀⠀⠀[38;2;107;120;144m⠈⠉[0m⠀⠀[0m",
    },
};

/* ---------- the banner: a framed box, centred, its crow flapping and a spark round the border ----------
 * The box is built again on every tick (banner_tick) and kept in the scrollback's lines, so the screen and
 * the modal (term.c: term_banner_draw) shows the same thing on both screens. Items are lines of the box; the crow's
 * rows are items of their own (kind 2) so the picture can change with its wings. */
#define BANNER_MAX_W 84   /* the box is as wide as the terminal less one column, up to this, and centred */
#define BOX_ITEMS 24
#define BOX_ROWS (BOX_ITEMS + 2)
#define CROW_FLAP_TICKS 10                      /* the flap: frames 0,1,2,1,0,1,2,1,0 then the folded wing (3) */
#define SPARK_TICKS 30                          /* the spark's lap: 30 ticks of TICK_MS (100 ms), once every 3 s */
#define SPARK_TAIL 4
static const int CROW_FLAP[CROW_FLAP_TICKS] = { 0, 1, 2, 1, 0, 1, 2, 1, 0, 3 };
#define BOX_EDGE (th_fg(g_theme->crow_dark))   /* the box's outline: a slate blue, the crow's dark (theme.c) */
/* the spark's colours, head first: the theme's (theme.c) */
static unsigned spark_col(int d) { return d == 0 ? g_theme->brand : d == 1 ? g_theme->spark_hi : d == 2 ? g_theme->spark_mid : g_theme->spark_lo; }
/* the crow's art in the theme's colours: the art is written with the default palette's RGB, which are read here as roles */
static void crow_out(sbuf *o, const char *art) {
    static const unsigned from[5] = { 0x4A6A8Au, 0x6B7890u, 0xA9B6CCu, 0x7A6650u, 0xE6FBFFu };
    const unsigned to[5] = { g_theme->crow_dark, g_theme->crow_body, g_theme->crow_light, g_theme->crow_beak, g_theme->brand };
    theme_recolor(art, o, from, to, 5);
}

struct box_item { int kind; int crow; sbuf text; };   /* kind: 0 left, 1 centred, 2 a row of the crow */
static struct { int ml, bw, n, t; struct box_item it[BOX_ITEMS]; } g_box;
static sbuf g_box_out[BOX_ROWS];                     /* the lines of the box as drawn now */

static void box_add(int kind, const char *fmt, ...) {
    if (g_box.n >= BOX_ITEMS) return;
    struct box_item *b = &g_box.it[g_box.n++];
    b->kind = kind; b->crow = -1; sb_init(&b->text);
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    sb_puts(&b->text, buf);
}

/* `s` in `cols` columns (its escapes kept); when it is longer, the first cols-1 columns and an ellipsis */
static void box_cut(sbuf *o, const char *s, int cols) {
    if (plain_width(s) <= cols) { sb_puts(o, s); return; }
    int w = 0; const char *p = s;
    while (*p) {
        if (*p == 27) { const char *q = p; while (*q && *q != 'm') q++; if (*q) q++; sb_append(o, p, (size_t)(q - p)); p = q; continue; }
        size_t len = 1; while (((unsigned char)p[len] & 0xC0) == 0x80) len++;
        if (w + 1 > cols - 1) break;
        sb_append(o, p, len); w++; p += len;
    }
    sb_puts(o, "…" C_RESET);
}

/* the border's cells run round once: index k of the perimeter, from the top left corner, clockwise */
static int box_perim(int bw, int H) { return 2 * (bw - 1) + 2 * (H - 1); }
static int box_k(int bw, int H, int row, int x) {
    if (row == 0) return x;
    if (row == H - 1) return (bw - 1) + (H - 1) + (bw - 1 - x);
    if (x == bw - 1) return (bw - 1) + row;
    return 2 * (bw - 1) + (H - 1) + (H - 1 - row);
}
/* one border cell: the spark's colours on its tail, the edge's elsewhere */
/* one border cell. `*last` is the colour the terminal already has (-1: none, 0: the edge, 1..: the spark's tail):
 * a colour is sent only when it changes, so a run of edge cells costs one escape (over ssh every byte counts) */
static void box_edge(sbuf *o, int k, int spark, int P, const char *glyph, int *last) {
    int d = spark < 0 ? -1 : ((spark - k) % P + P) % P;
    int want = d >= 0 && d < SPARK_TAIL ? 1 + d : 0;
    if (*last != want) {
        if (want) sb_puts(o, th_fg(spark_col(d)));
        else sb_puts(o, BOX_EDGE);
        *last = want;
    }
    sb_puts(o, glyph);
}

/* the box as it is drawn: the crow's `frame`, the spark at perimeter index `spark` (-1: none). Returns its rows. */
static int box_lines(int frame, int spark) {
    int bw = g_box.bw, H = g_box.n + 2, P = box_perim(bw, H), inner = bw - 4;
    for (int row = 0; row < H; row++) {
        sbuf *o = &g_box_out[row]; sb_clear(o);
        int last = -1;   /* the colour the terminal has, along this row */
        if (row == 0 || row == H - 1) {
            int bx = bw - BANNER_X_FROM_RIGHT - BANNER_X_W / 2;   /* the close button's first cell */
            for (int x = 0; x < bw; x++) {
                if (row == 0 && x >= bx && x < bx + BANNER_X_W) {   /* "[×]": bold, the × brightest: the close button */
                    if (x == bx + 1) sb_printf(o, "\x1b[1m%s×", th_fg(g_theme->brand));
                    else sb_printf(o, "\x1b[1m%s%s", th_fg(g_theme->spark_lo), x == bx ? "[" : "]");
                    last = -1;   /* the button's colours: the next edge cell names its own */
                    continue;
                }
                const char *g = x == 0 ? (row == 0 ? "╭" : "╰") : x == bw - 1 ? (row == 0 ? "╮" : "╯") : "─";
                box_edge(o, box_k(bw, H, row, x), spark, P, g, &last);
            }
            sb_puts(o, C_RESET);
            continue;
        }
        const struct box_item *it = &g_box.it[row - 1];
        box_edge(o, box_k(bw, H, row, 0), spark, P, "│", &last);
        sb_puts(o, C_RESET " ");
        sbuf t; sb_init(&t);
        if (it->kind == 2) crow_out(&t, CROW[frame][it->crow]);
        else box_cut(&t, it->text.data ? it->text.data : "", inner);
        int w = plain_width(t.data ? t.data : ""), lp = it->kind == 0 ? 0 : (inner - w) / 2;
        for (int i = 0; i < lp; i++) sb_putc(o, ' ');
        if (t.data) sb_append(o, t.data, t.len);
        for (int i = lp + w; i < inner; i++) sb_putc(o, ' ');
        sb_free(&t);
        sb_puts(o, C_RESET " ");
        last = -1;   /* the text may have set any colour: the right edge names its own */
        box_edge(o, box_k(bw, H, row, bw - 1), spark, P, "│", &last);
        sb_puts(o, C_RESET);
    }
    return H;
}

/* the frame and the spark at tick t (still: the folded wing, no spark) */
static void box_state(int t, bool still, int *frame, int *spark) {
    if (still) { *frame = 3; *spark = -1; return; }
    int c = t % SPARK_TICKS;
    *frame = c < CROW_FLAP_TICKS ? CROW_FLAP[c] : 3;
    int P = box_perim(g_box.bw, g_box.n + 2);
    *spark = (int)(((long)t * P) / SPARK_TICKS) % P;
}

static void banner_tick(bool still) {
    if (!term_banner_open()) return;   /* the animation runs while the modal is up */
    int frame, spark;
    box_state(g_box.t, still, &frame, &spark);
    if (!still) g_box.t++;
    int H = box_lines(frame, spark);
    const char *ls[BOX_ROWS];
    for (int i = 0; i < H; i++) ls[i] = g_box_out[i].data ? g_box_out[i].data : "";
    term_banner_draw(ls, H, g_box.bw);
}

/* the warning about plain http, as text for the box (host_warning prints it for /host) */
static bool host_warning_text(char *out, size_t n) {
    if (!url_cleartext(g_cfg.host) || url_is_local(g_cfg.host)) return false;
    char h[256] = ""; url_hostname(g_cfg.host, h, sizeof h);
    snprintf(out, n, C_YELLOW "⚠ plain http to %s: the conversation crosses the network unencrypted" C_RESET C_DIM " (an https:// host keeps it private)" C_RESET, h);
    return true;
}

static char g_host_note[400] = "";      /* the plain-http warning, for the transcript under the entry line */
static char g_advisor_note[200] = "";   /* a hosted advisor without its key: said in the transcript too */

static void banner_size(void) {
    int w = term_width(); if (w < 40) w = 40;
    g_box.bw = w - 1 < BANNER_MAX_W ? w - 1 : BANNER_MAX_W;   /* one column short of the screen: a full row would wrap */
    g_box.ml = (w - g_box.bw) / 2;
    g_box.n = 0;
}

/* the title "CORBIE NEST" in block letters, three rows (box_title); the letters the title uses */
static const struct { char ch; const char *row[3]; } BLOCK_FONT[] = {
    { 'C', { "▄▀▀", "█  ", "▀▀▀" } }, { 'O', { "▄▀▀▄", "█  █", "▀▀▀▀" } }, { 'R', { "█▀▀▄", "█▀▀▄", "▀  ▀" } },
    { 'B', { "█▀▀▄", "█▀▀▄", "▀▀▀ " } }, { 'I', { "▀█▀", " █ ", "▀▀▀" } }, { 'E', { "█▀▀", "█▀▀", "▀▀▀" } },
    { 'N', { "█▄ █", "█ ▀█", "▀  ▀" } }, { 'S', { "▄▀▀", "▀▀▄", "▄▄▀" } }, { 'T', { "▀█▀", " █ ", " ▀ " } },
    { ' ', { "  ", "  ", "  " } },
};
/* the title in block letters as three box items, a gap between the letters when the box has room for it; a box
 * too narrow for the letters keeps the plain name (false, nothing added) */
static bool box_title(void) {
    static const char *WORD = "CORBIE NEST";
    int inner = g_box.bw - 4;
    for (int gap = 1; gap >= 0; gap--) {
        sbuf r[3]; for (int i = 0; i < 3; i++) sb_init(&r[i]);
        int w = 0;
        for (const char *p = WORD; *p; p++) {
            size_t g = 0;
            while (BLOCK_FONT[g].ch != *p) g++;
            int gw = 0;
            for (int i = 0; i < 3; i++) if (plain_width(BLOCK_FONT[g].row[i]) > gw) gw = plain_width(BLOCK_FONT[g].row[i]);
            for (int i = 0; i < 3; i++) {
                if (p != WORD && gap) sb_putc(&r[i], ' ');
                sb_puts(&r[i], BLOCK_FONT[g].row[i]);
                for (int c = plain_width(BLOCK_FONT[g].row[i]); c < gw; c++) sb_putc(&r[i], ' ');
            }
            w += gw + (p != WORD && gap ? 1 : 0);
        }
        if (w <= inner) {
            for (int i = 0; i < 3; i++) { box_add(1, C_BOLD "%s%s" C_RESET, th_fg(g_theme->brand), r[i].data ? r[i].data : ""); }
            for (int i = 0; i < 3; i++) sb_free(&r[i]);
            return true;
        }
        for (int i = 0; i < 3; i++) sb_free(&r[i]);
    }
    return false;
}

/* the box's items, read now: the model, the host, the advisor, the mode; and the two warnings */
static void banner_items(bool need_ollama, int ok) {
    g_host_note[0] = g_advisor_note[0] = 0;
    box_title();   /* the block letters, when the box is wide enough (the name is on the line below either way) */
    box_add(1, C_BOLD "Corbie Nest" C_RESET " v%s — a coding agent for local and hosted models", CORBIE_VERSION);
    for (int r = 0; r < CROW_ROWS; r++) { box_add(2, ""); g_box.it[g_box.n - 1].crow = r; }
    box_add(0, "");   /* a gap between the crow and the lines below it */
    const char *eff = model_set() ? effort_resolve(&g_model_info, effort_get(g_cfg.model), NULL) : NULL;
    box_add(0, C_DIM "model:" C_RESET " %s%s" C_DIM " · ctx %s%s%s%s%s" C_RESET, model_set() ? g_cfg.model : "(none)", g_model_tools ? "" : C_DIM " (chat-only)" C_RESET,
            fmt_ctx(g_cfg.num_ctx), g_model_max_ctx > 0 ? " of " : "", g_model_max_ctx > 0 ? fmt_ctx(g_model_max_ctx) : "", eff ? " · effort " : "", eff ? eff : "");
    if (g_cfg.advisor) {
        /* a hosted advisor without its key is one that will not answer: say so here, not at the first consultation */
        const provider_def *pv = provider_find(g_cfg.advisor, NULL);
        const char *key = pv ? getenv(pv->key_env) : NULL;
        if (pv && !(key && *key)) snprintf(g_advisor_note, sizeof g_advisor_note, C_YELLOW "%s is not set — export it before starting corbienest" C_RESET, pv->key_env);
        box_add(0, C_DIM "advisor:" C_RESET " %s%s%s", g_cfg.advisor,
                g_cfg.advisor_guidance != GUIDANCE_NORMAL ? C_DIM " · guidance " C_RESET : "", g_cfg.advisor_guidance != GUIDANCE_NORMAL ? advisor_guidance()->name : "");
    }
    if (model_hosted()) {
        const provider_def *pv = provider_find(g_cfg.model, NULL);
        const char *key = pv ? getenv(pv->key_env) : NULL;
        box_add(0, C_DIM "api:  " C_RESET " %s · %s %s", pv ? provider_base_url(pv) : "", pv ? pv->key_env : "", key && *key ? C_GREEN "set" C_RESET : C_RED "not set" C_RESET);
        if (need_ollama) box_add(0, C_DIM "host: " C_RESET " %s %s" C_DIM " (the advisor)" C_RESET, g_cfg.host, ok == 0 ? C_GREEN "● connected" C_RESET : C_RED "● unreachable" C_RESET);
    } else {
        if (model_set()) box_add(0, C_DIM "host: " C_RESET " %s %s", g_cfg.host, ok == 0 ? C_GREEN "● connected" C_RESET : C_RED "● unreachable" C_RESET);   /* no model yet: Ollama was not asked */
        host_warning_text(g_host_note, sizeof g_host_note);
    }
    box_add(0, C_DIM "cwd:  " C_RESET " %s%s", g_cwd, g_project_instructions ? C_DIM " (project instructions loaded)" C_RESET : "");
    if (g_cfg.mode != MODE_MANUAL) box_add(0, C_DIM "mode:  " C_RESET " %s%s" C_RESET, g_cfg.mode == MODE_AUTO ? C_RED : g_cfg.mode == MODE_PLAN ? C_CYAN : C_ORANGE, mode_label(g_cfg.mode));
    box_add(0, C_DIM "ui:   " C_RESET " %s" C_DIM " · /ui to switch" C_RESET, g_ui_nc ? "notcurses" : "ansi");
}

/* the box as a modal: animated while it is up, on a screen a person watches (the prompt's tick). Without a
 * screen to put it on, its lines go to the transcript as they always did. */
static void banner_modal(bool animate) {
    int frame, spark;
    box_state(0, !animate, &frame, &spark);
    int H = box_lines(frame, spark);
    const char *ls[BOX_ROWS];
    for (int i = 0; i < H; i++) ls[i] = g_box_out[i].data ? g_box_out[i].data : "";
    if (term_banner_show(ls, H, g_box.bw)) { if (animate) term_tick_set(banner_tick); return; }
    for (int i = 0; i < H; i++) printf("%s\n", ls[i]);
}

static void banner(void) {
    bool need_ollama = ollama_needed();
    char ver[128] = "";
    int ok = need_ollama ? ollama_ping(ver, sizeof ver) : 0;
    banner_size();
    banner_items(need_ollama, ok);
    /* the transcript keeps one line for it; the box is the modal (/banner brings it back) */
    printf(C_BOLD "Corbie Nest" C_RESET C_DIM " v%s · /banner shows the box again" C_RESET "\n", CORBIE_VERSION);
    if (g_advisor_note[0]) printf("%s\n", g_advisor_note);
    if (g_host_note[0]) printf("%s\n", g_host_note);
    if (!model_set()) printf(C_YELLOW "%s" C_RESET "\n", g_model_err);
    if (need_ollama && ok != 0) {
        if (model_hosted()) printf(C_YELLOW "cannot reach ollama at %s: %s — the advisor's consultations need it" C_RESET "\n" C_DIM "start it with `ollama serve`, or set the host with /host or OLLAMA_HOST" C_RESET "\n", g_cfg.host, ver);
        else printf(C_RED "cannot reach ollama at %s: %s" C_RESET "\n" C_DIM "start it with `ollama serve`, or set the host with /host or OLLAMA_HOST" C_RESET "\n", g_cfg.host, ver);
    }
    bool animate = g_cfg.interactive && (g_ui_nc || term_is_fullscreen());
    banner_modal(animate);
    printf(C_DIM "/help for commands · @file to attach · !cmd for shell · shift+tab to switch mode · Ctrl-D to quit" C_RESET "\n\n");
}

/* /banner: the box again, as the modal (its animation with it) */
static void cmd_banner(void) {
    bool need_ollama = ollama_needed();
    char ver[128] = "";
    int ok = need_ollama ? ollama_ping(ver, sizeof ver) : 0;
    banner_size();
    banner_items(need_ollama, ok);
    bool animate = g_cfg.interactive && (g_ui_nc || term_is_fullscreen());
    banner_modal(animate);
}

/* ---------- suggestions under the input field (term_suggest) ----------
 * What the word at the cursor could become: a command for "/mo", one of its options for
 * "/mode pl", a file for "@te" (see file_complete: the name is searched for below the working
 * directory, or below the directory typed in front of it), a program or a path in a "!line". */
static void sug_add(term_sug **v, int *n, const char *text, const char *desc, bool open) {
    for (int i = 0; i < *n; i++) if (!strcmp((*v)[i].text, text)) return;   /* once is enough */
    *v = xrealloc(*v, sizeof **v * (size_t)(*n + 1));
    (*v)[(*n)++] = (term_sug){ xstrdup(text), desc && *desc ? xstrdup(desc) : NULL, open };
}

/* The model lists behind /advisor's suggestions: slot 0 is the Ollama server's, 1 + i that of
 * hosted provider i. Asked for when a name is first typed and kept for a minute — an answer that
 * did not come too, so a server that is down costs one wait and not one per letter. The hook
 * runs inside the poll of a live request as well, where a second request must not start: there
 * it gets what is already known. */
#define MODEL_LIST_SECS 60
#define MODEL_LISTS 8
#define MODEL_SUG_MAX 60
static struct { cJSON *list; char *from; time_t at; char why[240]; } g_model_lists[MODEL_LISTS];   /* why: what kept a provider's list away */

static cJSON *model_list_get(int slot, const provider_def *p, const char **why) {
    if (why) *why = NULL;
    if (slot >= MODEL_LISTS) return NULL;
    const char *from = p ? provider_base_url(p) : g_cfg.host;
    bool same = g_model_lists[slot].from && !strcmp(g_model_lists[slot].from, from);
    const char *key = p ? getenv(p->key_env) : NULL;   /* a key that is missing is not an answer to keep: it costs nothing to look again */
    if (http_busy() || (same && time(NULL) - g_model_lists[slot].at <= MODEL_LIST_SECS && (!p || g_model_lists[slot].list || (key && *key)))) {
        if (why && same && g_model_lists[slot].why[0]) *why = g_model_lists[slot].why;
        return same ? g_model_lists[slot].list : NULL;
    }
    cJSON_Delete(g_model_lists[slot].list); free(g_model_lists[slot].from);
    g_model_lists[slot].why[0] = 0;
    g_model_lists[slot].list = p ? provider_list_models(p, MODEL_LIST_WAIT_MS, g_model_lists[slot].why, sizeof g_model_lists[slot].why) : ollama_list_models_quiet(MODEL_LIST_WAIT_MS);
    g_model_lists[slot].from = xstrdup(from); g_model_lists[slot].at = time(NULL);
    if (why && g_model_lists[slot].why[0]) *why = g_model_lists[slot].why;
    return g_model_lists[slot].list;
}

static void sug_add(term_sug **v, int *n, const char *text, const char *desc, bool open);

/* What /advisor or /model could be given besides its options: an installed model, or a hosted
 * provider's prefix — and behind the prefix, the models its key can use. /model only offers
 * Anthropic (it can do the work; xAI and OpenAI stay advisors). The providers come before the
 * installed models (those can be many, and a hosted one is what nobody would guess is there);
 * of the models, names that start with what was typed come first, then those that contain it. */
static void suggest_advisor(const char *word, term_sug **items, int *n, bool working) {
    size_t wl = strlen(word);
    const char *colon = strchr(word, ':');
    const provider_def *pv = NULL, *p;
    if (colon) {   /* "xai:" is a provider as much as "xai:grok" is */
        sbuf probe; sb_init(&probe); sb_append(&probe, word, (size_t)(colon - word) + 1); sb_putc(&probe, 'x');
        pv = provider_find(probe.data, NULL);
        sb_free(&probe);
        if (working && pv && pv->style != PROVIDER_MESSAGES) pv = NULL;   /* xAI and OpenAI are advisors; /model does not offer them */
    }
    cJSON *m;
    if (pv) {
        int slot = 1; while (provider_at(slot - 1) != pv) slot++;
        const char *why;
        cJSON *list = model_list_get(slot, pv, &why);
        const char *rest = colon + 1; size_t rl = strlen(rest);
        if (!list && why) {   /* an empty list would look like a prefix nothing is known about: say what is in the way, on a row that changes nothing when taken */
            char *pre = xstrndup(word, (size_t)(colon - word) + 1);
            sug_add(items, n, pre, why, true);
            free(pre);
            return;
        }
        for (int pass = 0; pass < 2; pass++) cJSON_ArrayForEach(m, list) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id"), *name = cJSON_GetObjectItemCaseSensitive(m, "name");
            bool starts = !strncasecmp(id->valuestring, rest, rl);
            if (*n >= MODEL_SUG_MAX || (pass == 0 ? !starts : starts || rl < 2 || !strcasestr(id->valuestring, rest))) continue;
            sbuf t; sb_init(&t); sb_append(&t, word, (size_t)(colon - word) + 1); sb_puts(&t, id->valuestring);
            sug_add(items, n, t.data, cJSON_IsString(name) ? name->valuestring : NULL, false);
            sb_free(&t);
        }
        return;
    }
    for (int i = 0; (p = provider_at(i)); i++) {   /* by either name: "grok", "chatgpt" and "claude" are what people type */
        if (working && p->style != PROVIDER_MESSAGES) continue;
        char pre[48], desc[160]; snprintf(pre, sizeof pre, "%s:", p->name);
        if (strncasecmp(pre, word, wl) && (!p->alias || strncasecmp(p->alias, word, wl))) continue;
        const char *key = getenv(p->key_env);
        snprintf(desc, sizeof desc, working ? "%s (%s) — the model that does the work · %s %s" : "%s (%s), a hosted API · %s %s", p->label, p->known_as, p->key_env, key && *key ? "set" : "not set");
        sug_add(items, n, pre, desc, true);
    }
    if (working) for (int i = 0; (p = provider_at(i)); i++) {   /* the hosted models that do the work: "anthropic:ID", found by what is typed of it */
        if (p->style != PROVIDER_MESSAGES) continue;
        const char *why;
        cJSON *hl = model_list_get(provider_slot_of(p), p, &why);
        for (int pass = 0; pass < 2; pass++) cJSON_ArrayForEach(m, hl) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id"), *name = cJSON_GetObjectItemCaseSensitive(m, "name");
            if (!cJSON_IsString(id)) continue;
            sbuf t; sb_init(&t); sb_printf(&t, "%s:%s", p->name, id->valuestring);
            bool starts = !strncasecmp(t.data, word, wl);
            if (*n < MODEL_SUG_MAX && (pass == 0 ? starts : !starts && wl >= 2 && strcasestr(t.data, word))) {
                char desc[160]; snprintf(desc, sizeof desc, "%s%s%s", p->label, cJSON_IsString(name) ? " · " : "", cJSON_IsString(name) ? name->valuestring : "");
                sug_add(items, n, t.data, desc, false);
            }
            sb_free(&t);
        }
    }
    cJSON *list = model_list_get(0, NULL, NULL);
    for (int pass = 0; pass < 2; pass++) cJSON_ArrayForEach(m, list) {
        const char *name = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring, *size = cJSON_GetObjectItemCaseSensitive(m, "size")->valuestring;
        bool starts = !strncasecmp(name, word, wl);
        if (*n >= MODEL_SUG_MAX || (pass == 0 ? !starts : starts || wl < 2 || !strcasestr(name, word))) continue;
        char desc[96]; snprintf(desc, sizeof desc, "%s%s%s", size, *size && model_same(name, g_cfg.model) ? " · " : "", model_same(name, g_cfg.model) ? "the model doing the work" : "");
        sug_add(items, n, name, desc, false);
    }
}

static int suggest_files(const char *typed, bool dirs_only, term_sug **items) {
    file_match *m;
    int n = file_complete(typed, dirs_only, 50, &m), k = 0;
    for (int i = 0; i < n; i++) sug_add(items, &k, m[i].path, NULL, m[i].dir);
    file_matches_free(m, n);
    return k;
}

/* A "!line" is the shell's, so its words are completed the way a shell would: where a command
 * goes, the programs in $PATH that start with what was typed; anywhere else the files and
 * directories that do — found like an "@file", wherever below the working directory they are,
 * but by the start of their name only: most words of a command are not files, and a list that
 * came up for every one of them would be in the way. First of all come the "!lines" sent
 * before that go on from what is typed (the newest first), so a bare "!" shows what was run. */
static int suggest_shell(const char *buf, size_t cur, size_t *from, term_sug **items) {
    bool command;
    long ws = shell_word(buf, cur, &command);
    if (ws < 0) return 0;
    int n = 0;
    *from = (size_t)ws;
    char *word = xstrndup(buf + ws, cur - (size_t)ws);
    size_t wl = 0;
    for (const char *p = word; *p; p++) { if (*p == '\\' && p[1]) p++; word[wl++] = *p; }   /* as the shell reads it */
    word[wl] = 0;
    for (int i = hist_count() - 1, most = wl ? 3 : 8; i >= 0 && n < most; i--) {
        const char *h = hist_get(i);
        if (h[0] == '!' && strlen(h) > cur && !strncmp(h, buf, cur) && !strchr(h, '\n')) sug_add(items, &n, h + ws, NULL, true);
    }
    bool prog = command && !strchr(word, '/');   /* a program by its name; with a '/' in it, it is a path like any other */
    const char *name = strrchr(word, '/'); name = name ? name + 1 : word;
    file_match *m = NULL;
    int k = !wl || word[0] == '-' ? 0 : prog ? command_complete(word, 50, &m) : file_complete(word, false, 50, &m);
    for (int i = 0; i < k; i++) {
        if (!prog) {   /* the last part of the path has to start with what was typed */
            const char *base = m[i].path + strlen(m[i].path) - (m[i].dir ? 1 : 0);
            while (base > m[i].path && base[-1] != '/') base--;
            if (strncasecmp(base, name, strlen(name))) continue;
        }
        char *text = shell_escape(m[i].path);
        sug_add(items, &n, text, NULL, m[i].dir);
        free(text);
    }
    file_matches_free(m, k);
    free(word);
    return n;
}

static int suggest(const char *buf, size_t cur, size_t *from, term_sug **items) {
    int n = 0;
    if (buf[0] == '!') return suggest_shell(buf, cur, from, items);
    long at = mention_start(buf, cur);
    if (at >= 0) {
        char *typed = xstrndup(buf + at, cur - (size_t)at);
        *from = (size_t)at;
        n = suggest_files(typed, false, items);
        free(typed);
        return n;
    }
    if (buf[0] != '/' || memchr(buf, '\n', cur)) return 0;
    size_t ws = cur;   /* where the word at the cursor starts */
    while (ws > 0 && buf[ws - 1] != ' ') ws--;
    char *word = xstrndup(buf + ws, cur - ws);
    size_t wl = strlen(word);
    *from = ws;
    if (ws == 0) {   /* the command: the names that start with what was typed, then those that contain it */
        int nb = (int)(sizeof SLASH_CMDS / sizeof *SLASH_CMDS), ns = skills_count();
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < nb + ns; i++) {
                sbuf sk; sb_init(&sk);
                if (i >= nb) sb_printf(&sk, "/%s", skill_get(i - nb)->name);
                const char *name = i < nb ? SLASH_CMDS[i].name : sk.data;
                bool starts = !strncmp(name, word, wl);
                if (pass == 0 ? starts : !starts && wl > 2 && strcasestr(name + 1, word + 1))
                    sug_add(items, &n, name, i < nb ? SLASH_CMDS[i].desc : skill_get(i - nb)->desc, false);
                sb_free(&sk);
            }
        free(word);
        return n;
    }
    size_t bl = ws;   /* an argument: the options of the command line so far */
    while (bl > 0 && buf[bl - 1] == ' ') bl--;
    char *before = xstrndup(buf, bl);
    if (!strcmp(before, "/cd")) n = suggest_files(word, true, items);
    else if (!strcmp(before, "/ui theme")) {   /* the themes by name, with what each is */
        for (int i = 0; i < theme_count(); i++)
            if (!strncmp(theme_name(i), word, wl)) sug_add(items, &n, theme_name(i), !strcmp(theme_name(i), g_theme->name) ? "in use" : "a colour theme", false);
    }
    else if (!strcmp(before, "/effort")) {
        const model_info *mi = &g_model_info;
        const char *opt[EFFORT_LEVELS_MAX + 3], *why[EFFORT_LEVELS_MAX + 3]; int no = 0;
        opt[no] = "default"; why[no++] = "leave it to the model";
        if (mi->think_off) { opt[no] = "off"; why[no++] = "no thinking: fastest"; }
        if (mi->think_on) { opt[no] = "on"; why[no++] = "think"; }
        for (int i = 0; i < mi->n_think_levels; i++) { opt[no] = mi->think_levels[i]; why[no++] = i == 0 ? "the least thinking" : i == mi->n_think_levels - 1 ? "the most thinking: slowest" : ""; }
        for (int i = 0; i < no; i++) if (!strncmp(opt[i], word, wl)) sug_add(items, &n, opt[i], why[i], false);
    }
    else for (size_t i = 0; i < sizeof SLASH_ARGS / sizeof *SLASH_ARGS; i++) {
        if (strcmp(SLASH_ARGS[i].after, before)) continue;
        for (const char *o = SLASH_ARGS[i].opts; *o; ) {
            size_t ol = strcspn(o, "|"), nl = strcspn(o, ":|");
            char *name = xstrndup(o, nl), *desc = xstrndup(o + (nl < ol ? nl + 1 : nl), nl < ol ? ol - nl - 1 : 0);
            if (!strncmp(name, word, wl)) sug_add(items, &n, name, desc, false);
            free(name); free(desc);
            o += ol; if (*o == '|') o++;
        }
        break;
    }
    if (!strcmp(before, "/advisor")) suggest_advisor(word, items, &n, false);
    else if (!strcmp(before, "/model") || !strcmp(before, "/subagent")) suggest_advisor(word, items, &n, true);
    free(before); free(word);
    return n;
}

/* Handle one line of user input (from the editor or the message queue).
 * Returns 1 to quit, -1 if the model turn was interrupted, 0 otherwise. */
/* The implementation options the model ended its last reply with: a fenced ```options block, one line per
 * option ("- [recommended] Title — what it does"). They are offered as a picker, with the recommended one
 * under the cursor, and the chosen one comes back as the next message: malloc'd, or NULL when there are none
 * or the user chose none. */
static char *options_choose(void) {
    static int offered_at = -1;   /* the conversation's length when its last reply's options were offered: a slash command, which adds nothing, does not offer them again */
    if (!g_cfg.interactive || !g_messages) return NULL;
    if (cJSON_GetArraySize(g_messages) == offered_at) return NULL;
    offered_at = cJSON_GetArraySize(g_messages);
    cJSON *last = NULL;
    for (int i = cJSON_GetArraySize(g_messages) - 1; i >= 0 && !last; i--) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(g_messages, i), "role");
        if (cJSON_IsString(role) && !strcmp(role->valuestring, "assistant")) last = cJSON_GetArrayItem(g_messages, i);
    }
    cJSON *content = last ? cJSON_GetObjectItemCaseSensitive(last, "content") : NULL;
    if (!cJSON_IsString(content)) return NULL;
    const char *head = strstr(content->valuestring, "```options");
    const char *body = head ? strchr(head, '\n') : NULL;
    if (!body) return NULL;
    body++;
    const char *end = strstr(body, "```");
    size_t blen = end ? (size_t)(end - body) : strlen(body);
    char *blk = xstrndup(body, blen);
    char *labels[12], *descs[12]; int n = 0, rec = -1;
    for (char *ln = strtok(blk, "\n"); ln && n < 12; ln = strtok(NULL, "\n")) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (!(ln[0] == '-' || ln[0] == '*') || ln[1] != ' ') continue;
        ln += 2; while (*ln == ' ') ln++;
        if (!strncmp(ln, "[recommended]", 13)) { rec = n; ln += 13; while (*ln == ' ') ln++; }
        const char *dash = strstr(ln, " — ");   /* the dash between title and what it does (an em dash, or a hyphen) */
        size_t sep = strlen(" — ");
        if (!dash) { dash = strstr(ln, " - "); sep = 3; }
        labels[n] = dash ? xstrndup(ln, (size_t)(dash - ln)) : xstrdup(ln);
        descs[n] = dash ? xstrdup(dash + sep) : xstrdup("");
        n++;
    }
    char *chosen = NULL;
    if (n >= 2) {
        /* the last row is always "chat about this": it sends nothing, and the editor is left to the user's own words */
        const char *items[13], *shown[13]; char *dbuf[13];
        for (int i = 0; i < n; i++) {
            items[i] = labels[i];
            sbuf d; sb_init(&d); sb_puts(&d, descs[i]);
            if (i == rec) sb_puts(&d, *descs[i] ? " · recommended" : "recommended");
            dbuf[i] = sb_detach(&d); shown[i] = dbuf[i];
        }
        items[n] = "chat about this"; shown[n] = "talk it over with the model before choosing"; dbuf[n] = NULL;
        int r = term_select("Implementation options", items, shown, n + 1, rec >= 0 ? rec : 0);
        if (r == n) printf(C_DIM "  type your question or message below" C_RESET "\n");
        else if (r >= 0) {
            sbuf c; sb_init(&c); sb_printf(&c, "Go with this: %s", labels[r]);
            if (*descs[r]) sb_printf(&c, " — %s", descs[r]);
            chosen = sb_detach(&c);
        }
        for (int i = 0; i < n; i++) free(dbuf[i]);
    }
    for (int i = 0; i < n; i++) { free(labels[i]); free(descs[i]); }
    free(blk);
    return chosen;
}

static int process_input(char *line) {
    char *s = line; while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    size_t n = strlen(s); while (n && (s[n-1] == ' ' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
    if (!*s) return 0;
    if (strcmp(s, "/rewind")) { hist_add(s); hist_save(); }   /* persist as we go, so a crash or kill loses nothing */
    if (!strcasecmp(s, "exit")) return 1;   /* a typed "exit" leaves, as /quit does */
    if (*s == '/') { int q = handle_slash(s); if (q == 1) return 1; printf("\n"); term_status_refresh(); return q; }
    if (*s == '!') { handle_bang(s + 1); printf("\n"); return 0; }
    if (*s == '#' && s[1] && s[1] != '#') {   /* "# fact" -> memory (a lone "#" or "##…" is sent as text) */
        const char *f = s + 1; while (*f == ' ') f++;
        if (*f) { memory_quick_add(f); printf("\n"); return 0; }
    }
    char *msg = expand_mentions(s);
    int first = begin_request();
    typed_note(s, msg);
    add_message("user", msg);
    free(msg);
    tools_exit_clear();
    bool aborted = run_turn();
    session_save();
    memory_note(first, aborted);
    bool leave = tools_exit_asked() && !aborted;   /* the model judged the user is leaving: the turn is over, so go */
    tools_exit_clear();
    apply_pending();
    if (leave) return 1;
    printf("\n");
    fflush(stdout);
    return aborted ? -1 : 0;
}

/* ---------- --benchmark: tokens per second ----------
 * For each context size (the /ctx list up to the model's maximum, or just -c N): a warm-up call
 * (Ollama reloads the model for the new num_ctx — its load time and GPU placement are what
 * change with the size), then `runs` timed generations of the same prompt, capped at
 * BENCH_TOKENS tokens, with no tools or system prompt. Prompt-evaluation and generation rates
 * come from Ollama's own counters in the final chunk. Nothing is saved: no session, no memory. */
#define BENCH_TOKENS 256
static const char *BENCH_PROMPT =
    "Write a long, detailed essay on the history of programming languages, from Fortran and "
    "Lisp through C, Smalltalk, Python and Java to Rust. Write flowing prose, no lists or headings.";

static cJSON *bench_call(const char *prompt, int num_predict, chat_stats *st, bool *aborted) {
    cJSON *msgs = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", prompt);
    cJSON_AddItemToArray(msgs, m);
    ollama_call.num_predict = num_predict;
    ollama_quiet = true;
    cJSON *reply = ollama_chat(msgs, NULL, st, aborted);
    ollama_quiet = false;
    ollama_call_reset();
    cJSON_Delete(msgs);
    return reply;
}

/* progress note on the current line; the result line then overwrites it (interactive only) */
static void bench_note(const char *what) {
    if (!g_cfg.interactive) return;
    printf("\r\x1b[2K" C_DIM "  %s…" C_RESET, what); fflush(stdout);
}
static void bench_note_clear(void) { if (g_cfg.interactive) fputs("\r\x1b[2K", stdout); }
static double tps(long tokens, double secs) { return secs > 0 ? (double)tokens / secs : 0; }

/* one context size; returns 0 ok, 1 failed, 130 interrupted; fills *row (JSON) when wanted */
static int bench_ctx(int num_ctx, int runs, const char *prompt, cJSON *jrow) {
    g_cfg.num_ctx = num_ctx;
    char what[64], label[16];
    snprintf(label, sizeof label, "%s", fmt_ctx(num_ctx));
    chat_stats st; bool aborted = false;
    snprintf(what, sizeof what, "ctx %s · loading", label);
    bench_note(what);
    cJSON *r = bench_call(prompt, 8, &st, &aborted);
    bench_note_clear();
    if (jrow) cJSON_AddNumberToObject(jrow, "num_ctx", num_ctx);
    if (!r) { printf(C_BOLD "  %-6s " C_RESET C_RED "%s" C_RESET "\n", label, aborted ? "interrupted" : "failed"); if (jrow) cJSON_AddStringToObject(jrow, "error", aborted ? "interrupted" : "failed"); return aborted ? 130 : 1; }
    cJSON_Delete(r);
    double load = st.load_seconds, size, vram; int placed = ollama_model_placement(g_cfg.model, &size, &vram);
    if (jrow) {
        cJSON_AddNumberToObject(jrow, "load_seconds", load);
        if (placed == 0) { cJSON_AddNumberToObject(jrow, "size_bytes", size); cJSON_AddNumberToObject(jrow, "size_vram_bytes", vram); }
    }
    cJSON *jruns = jrow ? cJSON_AddArrayToObject(jrow, "runs") : NULL;
    long tok_sum = 0, ptok_sum = 0; double gen_sum = 0, prompt_sum = 0, ttft_sum = 0, gen_min = 1e30, gen_max = 0;
    int done = 0;
    for (int i = 0; i < runs; i++) {
        snprintf(what, sizeof what, "ctx %s · run %d/%d", label, i + 1, runs);
        bench_note(what);
        r = bench_call(prompt, BENCH_TOKENS, &st, &aborted);
        bench_note_clear();
        if (!r) break;
        cJSON_Delete(r);
        double gen = tps(st.eval_tokens, st.eval_seconds), pe = tps(st.prompt_tokens, st.prompt_seconds);
        tok_sum += st.eval_tokens; gen_sum += st.eval_seconds; ptok_sum += st.prompt_tokens; prompt_sum += st.prompt_seconds;
        ttft_sum += st.load_seconds + st.prompt_seconds;
        if (gen < gen_min) gen_min = gen;
        if (gen > gen_max) gen_max = gen;
        done++;
        if (jruns) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "prompt_tokens", st.prompt_tokens);
            cJSON_AddNumberToObject(o, "prompt_seconds", st.prompt_seconds);
            cJSON_AddNumberToObject(o, "eval_tokens", st.eval_tokens);
            cJSON_AddNumberToObject(o, "eval_seconds", st.eval_seconds);
            cJSON_AddNumberToObject(o, "total_seconds", st.total_seconds);
            cJSON_AddNumberToObject(o, "generation_tps", gen);
            cJSON_AddNumberToObject(o, "prompt_tps", pe);
            cJSON_AddItemToArray(jruns, o);
        }
    }
    /* row: ctx · load · placement · prompt eval · generation · ttft */
    char pl[40] = ""; bool spill = placed == 0 && size > 0 && vram < size;
    if (placed == 0 && size > 0) {
        if (!spill) snprintf(pl, sizeof pl, "100%% GPU (%.1f GB)", size / 1e9);
        else snprintf(pl, sizeof pl, "%d%% GPU (%.1f/%.1f GB)", (int)(100.0 * vram / size + 0.5), vram / 1e9, size / 1e9);
    }
    char ld[16]; if (load > 0.05) snprintf(ld, sizeof ld, "%.1fs", load); else snprintf(ld, sizeof ld, "loaded");
    printf(C_BOLD "  %-6s " C_RESET "%-8s %s%-22s" C_RESET "  ", label, ld, spill ? C_YELLOW : "", pl);
    if (done) {
        printf("%5.0f tok/s   " C_ORANGE "%6.1f tok/s" C_RESET, tps(ptok_sum, prompt_sum), tps(tok_sum, gen_sum));
        if (done > 1) printf(C_DIM " (%.1f–%.1f)" C_RESET, gen_min, gen_max); else printf("            ");
        printf("   %.2fs", ttft_sum / done);
    }
    if (done < runs) printf("  " C_RED "%s after %d run%s" C_RESET, aborted ? "interrupted" : "failed", done, done == 1 ? "" : "s");
    printf("\n"); fflush(stdout);
    if (jrow) {
        cJSON_AddNumberToObject(jrow, "generation_tps", tps(tok_sum, gen_sum));
        cJSON_AddNumberToObject(jrow, "prompt_tps", tps(ptok_sum, prompt_sum));
        cJSON_AddNumberToObject(jrow, "ttft_seconds", done ? ttft_sum / done : 0);
        if (done < runs) cJSON_AddStringToObject(jrow, "error", aborted ? "interrupted" : "failed");
    }
    return aborted ? 130 : done == runs ? 0 : 1;
}

static int run_benchmark(int runs, const char *prompt, bool ctx_given, bool json_out, int json_fd) {
    if (!prompt) prompt = BENCH_PROMPT;
    if (runs < 1) runs = 1;
    int sizes[16], n = 0;
    if (ctx_given || g_cfg.num_ctx <= 0) sizes[n++] = g_cfg.num_ctx;   /* one size (0 = the server's default) */
    else n = ctx_size_list(sizes, 16, 0, false);
    int draft = ollama_model_draft(g_cfg.model);
    printf(C_BOLD "benchmark" C_RESET "  %s · %d run%s × %d tokens per context size · ", g_cfg.model, runs, runs == 1 ? "" : "s", BENCH_TOKENS);
    if (g_cfg.draft >= 0) printf("draft %d (--draft)", g_cfg.draft);
    else if (draft > 0) printf("draft %d (model default, MTP/speculative)", draft);
    else printf("no draft/MTP");
    printf(" · %s\n", g_cfg.host);
    printf(C_DIM "  %-6s %-8s %-22s  %-14s%-27s%s" C_RESET "\n", "ctx", "load", "placement", "  prompt eval", "   generation", "first token");
    fflush(stdout);
    cJSON *jrows = json_out ? cJSON_CreateArray() : NULL;
    int rc = 0;
    for (int i = 0; i < n; i++) {
        cJSON *row = jrows ? cJSON_CreateObject() : NULL;
        rc = bench_ctx(sizes[i], runs, prompt, row);
        if (row) cJSON_AddItemToArray(jrows, row);
        if (rc) { if (rc != 130 && i + 1 < n) printf(C_DIM "  larger sizes skipped" C_RESET "\n"); break; }   /* larger sizes will not fare better */
    }
    term_restore();
    fflush(stdout);
    if (jrows) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "model", g_cfg.model);
        cJSON_AddNumberToObject(o, "draft_num_predict", g_cfg.draft >= 0 ? g_cfg.draft : draft);
        cJSON_AddNumberToObject(o, "runs_per_size", runs);
        cJSON_AddNumberToObject(o, "tokens_per_run", BENCH_TOKENS);
        cJSON_AddItemToObject(o, "sizes", jrows);
        cJSON_AddBoolToObject(o, "interrupted", rc == 130);
        char *txt = cJSON_PrintUnformatted(o);
        if (json_fd >= 0) { if (write(json_fd, txt, strlen(txt)) < 0 || write(json_fd, "\n", 1) < 0) {} }
        free(txt); cJSON_Delete(o);
    }
    return rc;
}

static void usage(void) {
    printf("usage: corbienest [options] [-p PROMPT]\n"
           "  -m, --model NAME     model that does the work (default: saved / first tool-capable local model);\n"
           "                       anthropic:MODEL or claude:MODEL uses the Anthropic API (ANTHROPIC_API_KEY)\n"
           "  -H, --host URL       ollama host (default $OLLAMA_HOST or http://127.0.0.1:11434)\n"
           "  -c, --ctx N          context window (num_ctx), e.g. 32768, 64k, 128k\n"
           "      --keep-alive DUR keep the model loaded between requests (default 30m; -1 forever, 0 unload, default = server's)\n"
           "  -s, --system TEXT    extra system instructions\n"
           "  -p, --prompt TEXT    non-interactive: run one prompt and exit (implies tools need --yolo)\n"
           "      --output-format text|json   with -p: print the reply as text (default) or one JSON object\n"
           "                       {result, session_id, model, prompt_tokens, eval_tokens, model_calls, tool_calls, advisor_calls, duration_s}\n"
           "  -y, --yolo           auto-approve tool calls (same as --mode auto)\n"
           "      --mode NAME      permission mode: manual, accept-edits, plan, auto\n"
           "  -T, --no-tools       disable tool calling\n"
           "      --continue       resume the latest session started in this directory\n"
           "  -r, --resume [ID]    resume a session: by ID, or pick one from a menu\n"
           "      --no-memory      don't update " MEMORY_PATH " after requests\n"
           "      --no-web         don't offer web_search/web_fetch (the model cannot look documentation up)\n"
           "      --think          think on every model call (default: only the first call of a request); --no-think to disable; --show-thinking to display\n"
           "      --effort LEVEL   how hard the model thinks, one of the levels it has (see /effort): off, on, low, medium, high, …; default = the model's own\n"
           "      --advisor MODEL  a stronger model the agent may consult through the advisor tool (see /advisor); off = none;\n"
           "                       xai:MODEL, openai:MODEL, anthropic:MODEL for a hosted one (key from XAI_API_KEY, OPENAI_API_KEY, ANTHROPIC_API_KEY)\n"
           "      --advisor-guidance LEVEL   how much the agent leans on it: light, normal (default), strong, max (see /advisor guidance)\n"
           "      --advisor-window MINUTES   a request that runs long gets its consultations again after that long (default 15; off = never)\n"
           "      --draft N        draft_num_predict: speculative-decoding/MTP draft tokens per step (0 = off; default: the model's own)\n"
           "      --ui ansi|notcurses  the screen for this run (default: ui= in the config, else ansi); notcurses needs a build made with notcurses installed\n"
           "      --benchmark [N]  measure tokens per second at each context size the model supports (or just -c N):\n"
           "                       N timed runs (default 3) of a fixed prompt (or -p PROMPT) per size, then exit\n"
           "  -h, --help           this help\n");
}

/* No model in the config and none on the Ollama server: the first provider that speaks Messages (Anthropic)
 * with its key in the environment becomes the working model, for this run only (it is not saved). Only the
 * Messages providers can do the work; the others are advisors. Returns false when there is no key. */
static bool hosted_fallback(bool quiet) {
    const provider_def *p = NULL;
    for (int i = 0; (p = provider_at(i)) != NULL; i++) {
        if (p->style != PROVIDER_MESSAGES) continue;
        const char *key = getenv(p->key_env);
        if (key && *key) break;
        p = NULL;
    }
    if (!p) {
        snprintf(g_model_err, sizeof g_model_err, "no model yet: ollama has none at %s — ollama pull <model>, set ANTHROPIC_API_KEY, or /model", g_cfg.host);
        return false;
    }
    char m[256]; snprintf(m, sizeof m, "%s:%s", p->name, p->example);
    free(g_cfg.model); g_cfg.model = xstrdup(m);
    refresh_model_caps(true);
    if (!g_model_bad) {
        if (!quiet) printf(C_DIM "no model in the config and none on ollama: using %s for this run (/model to choose another)" C_RESET "\n", m);
        return true;
    }
    /* the provider refused it: no model yet, and the reason says so (copied first: g_model_err is both) */
    char why[160]; snprintf(why, sizeof why, "%.150s", g_model_err);
    snprintf(g_model_err, sizeof g_model_err, "no model yet: %s could not be used (%s) — /model to choose one", m, why);
    free(g_cfg.model); g_cfg.model = NULL;
    g_model_bad = false;
    return false;
}

int main(int argc, char **argv) {
    g_argc = argc; g_argv = argv;
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    signal(SIGPIPE, SIG_IGN);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.temperature = -1; g_cfg.think = -1; g_cfg.draft = -1; g_cfg.max_iters = 100; g_cfg.num_ctx = 32768; g_cfg.color = true; g_cfg.memory = true; g_cfg.web = true; g_cfg.memory_every = 5; g_cfg.memory_idle = 15; g_cfg.advisor_guidance = GUIDANCE_NORMAL; g_cfg.advisor_window = ADVISOR_WINDOW_DEFAULT;
    g_cfg.keep_alive = xstrdup("30m");   /* ollama's own default unloads the model after 5 idle minutes */
    g_cfg.interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    config_load();
    const char *env_host = getenv("OLLAMA_HOST");
    if (env_host && *env_host && !g_cfg.host) g_cfg.host = xstrdup(env_host);
    if (!g_cfg.host) g_cfg.host = xstrdup("http://127.0.0.1:11434");
    const char *env_model = getenv("CORBIENEST_MODEL");
    if (env_model && *env_model) { free(g_cfg.model); g_cfg.model = xstrdup(env_model); }

    const char *oneshot = NULL, *resume_id = NULL, *cli_effort = NULL, *ui_arg = NULL; bool resume_latest = false, json_out = false, ctx_given = false; int bench_runs = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEEDARG() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (char*)NULL))
        if (!strcmp(a, "-m") || !strcmp(a, "--model")) { free(g_cfg.model); g_cfg.model = xstrdup(NEEDARG()); }
        else if (!strcmp(a, "-H") || !strcmp(a, "--host")) { free(g_cfg.host); g_cfg.host = xstrdup(NEEDARG()); }
        else if (!strcmp(a, "-c") || !strcmp(a, "--ctx")) { const char *cv = NEEDARG(); int n = parse_ctx_arg(cv); if (n < 0) { fprintf(stderr, "bad context size %s (use e.g. 32768, 64k, default)\n", cv); return 2; } g_cfg.num_ctx = n; ctx_given = true; }
        else if (!strcmp(a, "-s") || !strcmp(a, "--system")) { free(g_cfg.system_prompt); g_cfg.system_prompt = xstrdup(NEEDARG()); }
        else if (!strcmp(a, "-p") || !strcmp(a, "--prompt")) oneshot = NEEDARG();
        else if (!strcmp(a, "-y") || !strcmp(a, "--yolo")) g_cfg.mode = MODE_AUTO;
        else if (!strcmp(a, "--mode")) { const char *mn = NEEDARG(); int m = mode_parse(mn); if (m < 0) { fprintf(stderr, "unknown mode %s\n", mn); return 2; } g_cfg.mode = m; }
        else if (!strcmp(a, "-T") || !strcmp(a, "--no-tools")) g_cfg.no_tools = true;
        else if (!strcmp(a, "--no-memory")) g_cfg.memory = false;
        else if (!strcmp(a, "--no-web")) g_cfg.web = false;
        else if (!strcmp(a, "--continue")) resume_latest = true;
        else if (!strcmp(a, "--output-format")) { const char *f = NEEDARG(); if (!strcmp(f, "json")) json_out = true; else if (strcmp(f, "text")) { fprintf(stderr, "unknown output format %s (text|json)\n", f); return 2; } }
        else if (!strcmp(a, "-r") || !strcmp(a, "--resume")) { resume_id = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : ""; }
        else if (!strcmp(a, "--keep-alive")) { const char *kv = NEEDARG(); free(g_cfg.keep_alive); g_cfg.keep_alive = strcmp(kv, "default") ? xstrdup(kv) : NULL; }
        else if (!strcmp(a, "--think")) g_cfg.think = 1;
        else if (!strcmp(a, "--no-think")) g_cfg.think = 0;
        else if (!strcmp(a, "--show-thinking")) g_cfg.show_thinking = true;
        else if (!strcmp(a, "--effort")) cli_effort = NEEDARG();
        else if (!strcmp(a, "--advisor")) { const char *av = NEEDARG(); free(g_cfg.advisor); g_cfg.advisor = strcmp(av, "off") && strcmp(av, "none") ? xstrdup(av) : NULL; }
        else if (!strcmp(a, "--advisor-window")) { const char *wv = NEEDARG(); char *we = NULL; double m = !strcmp(wv, "off") ? 0 : strtod(wv, &we); if ((we && (*we || we == wv)) || m < 0 || m > 24 * 60) { fprintf(stderr, "bad advisor window %s (minutes, or off)\n", wv); return 2; } g_cfg.advisor_window = (int)(m * 60 + 0.5); }
        else if (!strcmp(a, "--advisor-guidance")) { const char *gv = NEEDARG(); int n = advisor_guidance_parse(gv); if (n < 0) { fprintf(stderr, "bad advisor guidance %s (light, normal, strong or max)\n", gv); return 2; } g_cfg.advisor_guidance = n; }
        else if (!strcmp(a, "--draft")) { const char *dv = NEEDARG(); if (strspn(dv, "0123456789") != strlen(dv) || !*dv) { fprintf(stderr, "bad draft count %s (a number of tokens, 0 = off)\n", dv); return 2; } g_cfg.draft = atoi(dv); }
        else if (!strcmp(a, "--benchmark")) { bench_runs = 3; if (i + 1 < argc && argv[i+1][0] >= '1' && argv[i+1][0] <= '9' && strspn(argv[i+1], "0123456789") == strlen(argv[i+1])) bench_runs = atoi(argv[++i]); }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-v") || !strcmp(a, "--version")) { printf("corbienest %s%s\n", CORBIE_VERSION, nc_compiled() ? " (notcurses screen)" : ""); return 0; }
        else if (!strcmp(a, "--ui")) { ui_arg = NEEDARG(); if (strcmp(ui_arg, "ansi") && strcmp(ui_arg, "notcurses")) { fprintf(stderr, "unknown screen %s (ansi|notcurses)\n", ui_arg); return 2; } }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    if (!getcwd(g_cwd, sizeof g_cwd)) snprintf(g_cwd, sizeof g_cwd, ".");
    g_session.started = time(NULL);
    /* the screen: --ui, else ui= in the config. Only an interactive session gets it: a pipe, -p and
     * --benchmark have no screen to draw, and the notcurses one would draw over their output */
    const char *want_ui = ui_arg ? ui_arg : g_cfg.ui ? "notcurses" : "ansi";
    if (!strcmp(want_ui, "notcurses") && g_cfg.interactive && !oneshot && !bench_runs && !json_out) {
        if (nc_compiled()) g_ui_nc = true;
        else fprintf(stderr, "corbienest: this build has no notcurses screen (notcurses was not installed when it was built); using the ANSI one\n");
    }
    term_init();
    g_messages = cJSON_CreateArray(); typed_clear();
    g_tools = tools_definitions();
    tools_subagent = run_subagent;
    tools_advisor = run_advisor;
    term_run_while_busy = run_slash_while_busy;
    load_project_instructions();
    load_memory();
    tools_permissions_load();
    tools_hooks_load();
    skills_load();
    if (g_cfg.interactive && !oneshot && !bench_runs) tools_hook_event("start");   /* the start hooks: at a terminal only */
    bool quiet_caps = oneshot != NULL || bench_runs;
    if (g_cfg.model && !*g_cfg.model) { free(g_cfg.model); g_cfg.model = NULL; }   /* an empty model= in the config is no model */
    refresh_model_caps(quiet_caps);
    if (!g_cfg.model && !hosted_fallback(quiet_caps)) {
        /* nothing to work with yet: a one-shot run cannot do anything without a model; the screen starts and says so */
        if (quiet_caps) { fprintf(stderr, "corbienest: %s\n", g_model_err); term_restore(); return 1; }
        g_cfg.model = xstrdup("");
    }
    if (g_model_bad) { fprintf(stderr, "corbienest: %s\n", g_model_err); term_restore(); return 1; }
    if (cli_effort) {   /* only now is it known what the model offers */
        if (!strcmp(cli_effort, "default")) effort_set(g_cfg.model, NULL);
        else if (!effort_resolve(&g_model_info, cli_effort, NULL)) {
            sbuf c; sb_init(&c); effort_choices(&g_model_info, &c);
            if (!g_model_info.thinking) fprintf(stderr, "corbienest: --effort %s: %s cannot think, so it has no effort to set\n", cli_effort, g_cfg.model);
            else fprintf(stderr, "corbienest: --effort %s: %s offers %s, or default\n", cli_effort, g_cfg.model, c.data ? c.data : "nothing");
            sb_free(&c); term_restore(); return 2;
        } else { effort_set(g_cfg.model, cli_effort); if (strcmp(cli_effort, "off") && g_cfg.think == 0) g_cfg.think = -1; }
    }
    if (bench_runs) {
        if (provider_find(g_cfg.model, NULL)) { fprintf(stderr, "corbienest: --benchmark measures an Ollama model; %s is a hosted one\n", g_cfg.model); term_restore(); return 2; }
        int json_fd = -1;
        if (json_out) {   /* the report is dropped; only the JSON goes to the real stdout */
            fflush(stdout);
            json_fd = dup(STDOUT_FILENO);
            if (!freopen("/dev/null", "w", stdout)) { fprintf(stderr, "corbienest: cannot redirect stdout\n"); return 1; }
        }
        return run_benchmark(bench_runs, oneshot, ctx_given, json_out, json_fd);
    }
    if (g_cfg.interactive && !oneshot) term_fullscreen(true);   /* alternate screen + bottom status bar */
    if (resume_latest) {
        int n; session_info *v = sessions_list(true, &n);
        if (n) session_load(v[0].id); else printf(C_DIM "no earlier session in this directory — starting fresh" C_RESET "\n");
        sessions_free(v, n);
    } else if (resume_id && *resume_id) {
        if (!session_load(resume_id)) { fprintf(stderr, "corbienest: no session %s\n", resume_id); term_restore(); return 1; }
    } else if (resume_id) {
        if (oneshot) { fprintf(stderr, "corbienest: --resume needs a session ID with -p\n"); return 2; }
        cmd_resume(NULL);
    }

    if (json_out && !oneshot) { fprintf(stderr, "corbienest: --output-format json needs -p PROMPT\n"); return 2; }
    int json_fd = -1;
    if (json_out) {   /* everything the run prints goes to /dev/null; the JSON goes to the real stdout */
        fflush(stdout);
        json_fd = dup(STDOUT_FILENO);
        if (!freopen("/dev/null", "w", stdout)) { fprintf(stderr, "corbienest: cannot redirect stdout\n"); return 1; }
        g_cfg.color = false;
    }

    if (oneshot) {
        char *msg;
        const skill_t *sk = NULL;
        if (oneshot[0] == '/') { char *nm = xstrndup(oneshot, strcspn(oneshot, " ")); sk = skill_find(nm); free(nm); }
        if (sk) { const char *a = strchr(oneshot, ' '); char *pr = skill_expand(sk, a ? a + 1 : ""); msg = expand_mentions(pr); free(pr); }
        else msg = expand_mentions(oneshot);
        int first = begin_request();
        typed_note(oneshot, msg);
        add_message("user", msg); free(msg);
        time_t t0 = time(NULL);
        bool aborted = run_turn();
        session_save();
        memory_note(first, aborted);
        term_restore();
        if (json_out) {
            cJSON *o = cJSON_CreateObject();
            const char *last = "";
            for (int i = cJSON_GetArraySize(g_messages) - 1; i >= 0; i--) {
                cJSON *m = cJSON_GetArrayItem(g_messages, i);
                cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
                if (cJSON_IsString(r) && !strcmp(r->valuestring, "assistant")) { if (cJSON_IsString(c)) last = c->valuestring; break; }
            }
            cJSON_AddStringToObject(o, "result", last);
            cJSON_AddBoolToObject(o, "interrupted", aborted);
            cJSON_AddBoolToObject(o, "error", g_turn_failed);
            cJSON_AddStringToObject(o, "session_id", g_session_id);
            cJSON_AddStringToObject(o, "model", g_cfg.model);
            cJSON_AddNumberToObject(o, "prompt_tokens", (double)g_session.prompt_tokens);
            cJSON_AddNumberToObject(o, "eval_tokens", (double)g_session.eval_tokens);
            cJSON_AddNumberToObject(o, "model_calls", g_session.calls);
            cJSON_AddNumberToObject(o, "tool_calls", g_session.tool_calls);
            cJSON_AddNumberToObject(o, "advisor_calls", g_session.advisor_calls);
            cJSON_AddNumberToObject(o, "duration_s", difftime(time(NULL), t0));
            cJSON_AddNumberToObject(o, "num_messages", cJSON_GetArraySize(g_messages) - first);
            char *txt = cJSON_PrintUnformatted(o);
            if (json_fd >= 0) { if (write(json_fd, txt, strlen(txt)) < 0 || write(json_fd, "\n", 1) < 0) {} }
            free(txt); cJSON_Delete(o);
        }
        return aborted ? 130 : g_turn_failed ? 1 : 0;   /* -p: an error (the model did not answer) is a failed run */
    }

    banner();
    if (term_trace_path())   /* it holds the conversation: nobody should be recording one without seeing so */
        printf(C_YELLOW "trace:" C_RESET C_DIM " everything drawn and typed in this session is written to %s" C_RESET "\n\n", term_trace_path());
    hist_load();
    term_suggest = suggest;
    term_idle_hook = memory_idle_hook;
    term_agents_menu = agents_menu;
    term_agents_strip = strip_lines;
    term_agents_open = strip_open;
    for (;;) {
        memory_arm_idle();   /* a finished request folds itself into memory while the prompt sits idle */
        char *line = term_readline(C_BOLD C_ORANGE "› " C_RESET);
        if (!line) break;
        int rc = process_input(line);
        free(line);
        if (rc == 1) break;
        /* Messages queued while the model was busy: after an interrupt they go back to the
         * editor for the user to review; otherwise they are sent one after another. */
        while (rc != -1 && term_queue_count()) {
            char *q = term_queue_pop();
            echo_queued(q);
            rc = process_input(q);
            free(q);
        }
        /* the options the model offered at the end of its reply: the one chosen is the next message */
        while (rc == 0 && !term_queue_count()) {
            char *pick = options_choose();
            if (!pick) break;
            rc = process_input(pick);
            free(pick);
        }
        if (rc == 1) break;
        if (rc == -1) term_queue_to_editor();
    }
    hist_save();
    g_mem_exiting = true;
    memory_flush();   /* whatever the idle flush did not get to runs before we leave (Ctrl-C skips it) */
    session_save();
    term_finish();    /* leaves the alternate screen: say goodbye on the normal one */
    printf(C_DIM "bye" C_RESET " · corbienest used %ld tokens this session (%ld in · %ld out) with %s\n",
           g_session.prompt_tokens + g_session.eval_tokens, g_session.prompt_tokens, g_session.eval_tokens, g_cfg.model);
    if (g_session_id[0]) printf(C_DIM "  continue it later with: corbienest --continue   (or --resume %s)" C_RESET "\n", g_session_id);
    return 0;
}
