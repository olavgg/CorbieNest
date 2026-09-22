/* corbienest - a Claude-Code-style TUI for local Ollama models, in C. */
#ifndef CORBIE_COMMON_H
#define CORBIE_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>
#include <cjson/cJSON.h>

#define CORBIE_VERSION "0.1.0"

/* ---------- ANSI colours ---------- */
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_ITALIC  "\x1b[3m"
#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_BLUE    "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN    "\x1b[36m"
#define C_GRAY    "\x1b[90m"
#define C_ORANGE  "\x1b[38;5;208m"

/* ---------- growable string buffer ---------- */
typedef struct {
    char *data;
    size_t len, cap;
} sbuf;

void  sb_init(sbuf *b);
void  sb_free(sbuf *b);
void  sb_clear(sbuf *b);
void  sb_append(sbuf *b, const char *s, size_t n);
void  sb_puts(sbuf *b, const char *s);
void  sb_putc(sbuf *b, char c);
void  sb_printf(sbuf *b, const char *fmt, ...);
char *sb_detach(sbuf *b);            /* returns malloc'd string, resets buffer */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void  die(const char *fmt, ...);

/* file helpers */
char *read_whole_file(const char *path, size_t *len_out, size_t cap);   /* NULL on error */
int   write_whole_file(const char *path, const char *data, size_t len);
/* Same, but the file is replaced in one step: written to a temporary next to it, then
 * rename(2)d over it. Another process reading the file gets either the whole old one or the
 * whole new one, never half of either, and a crash mid-write cannot truncate what was there.
 * Use it for every file corbienest keeps its own state in — several sessions may share it. */
int   write_whole_file_atomic(const char *path, const char *data, size_t len);
int   mkdir_p(const char *path);
char *expand_home(const char *path);   /* "~/x" -> "/home/u/x", malloc'd */
int   is_dir(const char *path);
int   is_file(const char *path);

/* URLs and HTML, for the web_fetch tool (in util.c so the unit tests can reach them) */
bool  url_ok(const char *url);                                  /* http(s), sane, not cloud metadata */
bool  url_host(const char *url, char *out, size_t n);           /* "host[:port]", lowercased */
char *html_to_text(const char *html, size_t len, const char *base);  /* readable text, malloc'd; base = "scheme://host" for rooted links */
char *url_encode(const char *s);                                /* percent-encoding for a query value */
char *url_decode(const char *s, size_t n);                      /* the reverse, malloc'd */
/* A search engine's result page as "N. title / url / snippet" lines; *count gets how many */
char *search_results_text(const char *html, size_t len, const char *engine_url, int max, int *count);
/* A conversation (cJSON array of messages) as one quoted text of at most ~`budget` bytes, for a
 * model that is shown it rather than being part of it — the advisor. Long messages lose their
 * middle; when it still does not fit the newest messages win, a line says how many older ones
 * were left out, and message `keep` (the request being worked on; -1 = none) always stays. malloc'd. */
char *transcript_text(cJSON *msgs, int keep, size_t budget);
void  sb_put_cut(sbuf *b, const char *s, size_t cap);   /* s, without its middle when longer than cap bytes (UTF-8 safe; says how much went) */

/* The advisor (/advisor): where a consultation runs and what it is shown — see util.c. */
#define ADVISOR_CTX_AUTO     16384        /* its window unless /advisor ctx says otherwise (and never more than the main one) */
#define ADVISOR_CTX_CLOUD    32768        /* what is planned for with a cloud model, which has its own (large) window */
#define ADVISOR_CTX_UNSET    4096         /* what to plan for when num_ctx is left to the server */
#define ADVISOR_CTX_API      131072       /* ... and with a hosted API's model whose window it did not say */
#define ADVISOR_CTX_MIN      4096
#define ADVISOR_REPLY_MAX    8192         /* tokens it may generate, thinking included */
#define ADVISOR_REPLY_API    16000        /* the same for a hosted API: paid by the token used, not by the cap, and a cut-off answer is the waste */
#define ADVISOR_BRIEF_MIN    4096         /* bytes of prompt it is never planned below, however small the window */
#define ADVISOR_RULES_MAX    6000
#define ADVISOR_QUESTION_MAX 4000
/* How much the agent leans on the advisor (/advisor guidance). A weak model is a poor judge of
 * when it needs help, so the upper levels have corbienest ask as well: a review before a request
 * that changed files ends, and (max) a check of the request's first change before it is made. */
enum { GUIDANCE_LIGHT, GUIDANCE_NORMAL, GUIDANCE_STRONG, GUIDANCE_MAX, GUIDANCE_COUNT };
typedef struct {
    const char *name;
    int    uses;               /* consultations the agent may ask for in one request */
    int    words;              /* how long the advice may be */
    size_t brief_max;          /* bytes it is shown at most, whatever the window: reading it is what the user waits for */
    bool   review;             /* it reviews a request that changed files before the request ends */
    bool   check_first_edit;   /* it checks the first change of a request before the change is made */
    const char *desc;          /* for the picker */
} advisor_guidance_def;
extern const advisor_guidance_def ADVISOR_GUIDANCE[GUIDANCE_COUNT];
const advisor_guidance_def *advisor_guidance(void);   /* the one g_cfg.advisor_guidance says */
int   advisor_guidance_parse(const char *s);          /* -1 = no such level */
typedef struct {
    bool same, cloud;          /* it is the main model / not on this machine (an Ollama cloud model or a hosted API); neither = another local model */
    bool api;                  /* a hosted API (provider.c): cloud as well */
    int  window;               /* the context window to plan for */
    int  send_ctx;             /* for ollama_call.num_ctx: >0 that size, 0 as the main model's, <0 leave the key out */
    const char *keep_alive;    /* for ollama_call.keep_alive: NULL = the main model's, "" = leave the key out */
    int  reply;                /* num_predict / max_tokens */
    size_t budget;             /* bytes the whole prompt may take */
} advisor_plan;
void  advisor_plan_for(const char *advisor, int trained_ctx, advisor_plan *p);   /* reads g_cfg.model, .num_ctx, .advisor_ctx, .advisor_guidance */
/* The one user message of a consultation; `words` is how long the answer may be. malloc'd. */
char *advisor_brief(cJSON *msgs, int keep, size_t budget, const char *env, bool plan_mode, const char *rules, const char *question, int words);
#define ADVISOR_REVIEW_OK "LGTM"   /* what the advisor answers a review or a check with when nothing needs to change */
bool  advisor_approves(const char *advice);   /* the answer is ADVISOR_REVIEW_OK and not much more */
const char *strip_think_block(const char *s);   /* past a leading <think>…</think> */
bool  model_same(const char *a, const char *b);   /* equal, a trailing ":latest" aside */
bool  model_is_cloud(const char *name);           /* NAME:cloud / NAME-cloud */

/* ---------- global config ---------- */
typedef struct { char *model, *level; } effort_entry;   /* level: "off", "on", or one of the model's named levels */

typedef struct {
    char *host;          /* e.g. http://127.0.0.1:11434 */
    char *model;
    char *system_prompt; /* extra system prompt from CLI/config */
    int   num_ctx;       /* 0 = leave to server default */
    int   draft;         /* draft_num_predict: speculative/MTP draft tokens per step; -1 = model default, 0 = off */
    double temperature;  /* <0 = unset */
    int   think;         /* -1 auto (server default on the first call of a request, off for tool rounds), 0 off, 1 on for every call */
    effort_entry *efforts; /* how hard each model thinks (/effort), see effort_get(); saved as effort.<model>=<level> */
    int   n_efforts;
    char *advisor;       /* the stronger model the agent may consult through the advisor tool (/advisor); NULL = none */
    int   advisor_ctx;   /* num_ctx of an advisor call; 0 = auto (see advisor_plan_for()) */
    int   advisor_guidance; /* how much the agent leans on it, GUIDANCE_* (/advisor guidance) */
    bool  show_thinking; /* print thinking tokens */
    int   mode;          /* permission mode, see MODE_* */
    bool  no_tools;      /* don't send tools at all */
    bool  web;           /* offer the web_fetch/web_search tools (/web, --no-web) */
    char *search_url;    /* search engine, %s = the url-encoded query (/web engine) */
    bool  color;
    int   max_iters;     /* tool loop guard */
    bool  interactive;   /* stdin is a tty */
    bool  memory;        /* keep .corbienest/memory.md up to date (see memory_every) */
    int   memory_every;  /* run the memory-extraction call after this many requests (also at exit, /clear, /compact, /cd) */
    int   memory_idle;   /* seconds idle at the prompt after which a pending extraction runs anyway (0 = only at memory_every / exit) */
    char *keep_alive;    /* ollama keep_alive for the model ("30m", "-1" = forever, "0" = unload); NULL = server default */
} config_t;

extern config_t g_cfg;

/* per-session token accounting (shown in the status bar and /status) */
typedef struct {
    long prompt_tokens;      /* sum of prompt (input) tokens over all calls */
    long eval_tokens;        /* sum of generated (output) tokens over all calls */
    int  last_prompt_tokens; /* prompt size of the most recent call (context usage) */
    int  calls;              /* model calls made (turns, tool rounds, compaction, memory) */
    int  turns;              /* user requests answered */
    int  tool_calls;         /* tools executed */
    double model_seconds;    /* wall time spent waiting on the model (from Ollama's total_duration) */
    double eval_seconds;     /* generation time (Ollama's eval_duration) */
    double think_seconds;    /* of which: thinking (before the first visible token) */
    long   think_chunks;
    int    advisor_calls;    /* consultations of the advisor model (their tokens and time are in the totals above as well) */
    long   advisor_tokens;
    double advisor_seconds;
    long   advisor_eval_tokens;   /* generated by the advisor, and how long that took: another model's speed, */
    double advisor_eval_seconds;  /* kept out of the tok/s that /cost gives for the main one */
    time_t started;          /* session start (for /cost) */
} session_stats;
extern session_stats g_session;

/* permission modes (cycle with shift+tab, or /mode) */
enum { MODE_MANUAL = 0, MODE_ACCEPT_EDITS, MODE_PLAN, MODE_AUTO, MODE_COUNT };
#define YOLO() (g_cfg.mode == MODE_AUTO)
const char *mode_name(int mode);          /* "manual", "accept-edits", "plan", "auto" */
const char *mode_label(int mode);         /* short human description */
int         mode_parse(const char *s);    /* -1 if unknown */

const char *config_dir(void);        /* ~/.config/corbienest (created) */
void config_load(void);
void config_save(void);

/* ---------- http.h ---------- */
/* Line callback for streamed NDJSON. Return nonzero to abort the request. */
typedef int (*http_line_cb)(const char *line, size_t len, void *ud);
typedef void (*http_idle_cb)(void *ud);   /* called ~every 100ms while waiting */

typedef struct {
    int   status;         /* HTTP status, 0 if none */
    bool  aborted;        /* aborted by callback / interrupt key */
    char  err[512];       /* error text if return < 0 */
} http_result;

extern int http_interrupt_fd;   /* fd to watch for user input while waiting; -1 to disable */
extern int http_idle_timeout_ms; /* a request fails when the server sends nothing for this long (default 10 minutes) */
extern int (*http_interrupt_check)(void);   /* called when that fd is readable; nonzero = abort */
extern http_idle_cb http_idle;
extern void *http_idle_ud;
extern const char *const *http_headers;   /* extra request headers ("Name: value"), NULL-terminated; set around one call */

/* Performs request (libcurl: http:// and https://). If line_cb != NULL body is delivered line by
 * line to it, otherwise appended to `out` (may be NULL to discard). Returns 0 ok, <0 error; an
 * interrupt or a callback abort returns 0 with res->aborted set. */
int http_request(const char *base_url, const char *method, const char *path,
                 const char *body, sbuf *out, http_line_cb line_cb, void *ud,
                 http_result *res);
char *http_url(const char *base, const char *path);   /* base + path; "host" and "http://host" get Ollama's port 11434 (malloc'd) */

/* ---------- term.h ---------- */
void term_init(void);
void term_restore(void);
void term_raw(bool on);
int  term_width(void);
void term_size(int *rows, int *cols);
void term_clear_screen(void);

/* Full-screen mode: alternate screen with the bottom row reserved for a status
 * bar (permission mode · model · session tokens · context usage), like Claude Code.
 * term_status_refresh() redraws the bar; call it after mode/model/token changes.
 * term_status_live() shows output tokens of an in-flight generation (0 = none). */
void term_fullscreen(bool on);
void term_status_refresh(void);
void term_status_live(long out_tokens);
void fmt_tokens(long n, char *out, size_t sz);   /* 950, 1.2k, 45k, 1.1M */
/* Activity indicator in the bar: term_busy("label") shows an animated spinner with the
 * label until term_busy(NULL); call term_busy_tick() regularly (≥10 Hz) while waiting. */
void term_busy(const char *label);
void term_busy_tick(void);

/* prompt history: the latest 100 queries, persisted in ~/.config/corbienest/history */
void hist_load(void);
void hist_save(void);
void hist_add(const char *line);
int  hist_count(void);
const char *hist_get(int i);   /* 0 = oldest kept … hist_count()-1 = most recent */

/* Interactive line editor. Returns malloc'd string, or NULL on EOF (Ctrl-D). */
char *term_readline(const char *prompt);

/* Read a single key while in raw mode. Returns the byte or -1. */
int term_getkey(void);
/* Non-blocking: drain pending stdin into the type-ahead buffer; returns 1 if Ctrl-C/Esc was pressed.
 * Enter while busy turns the pending text into a queued message (see below). */
int term_poll_interrupt(void);
/* Background work at an idle prompt. When term_idle_ms > 0, the editor runs term_idle_hook()
 * once per prompt after that many milliseconds without a keystroke on an empty line, with the
 * terminal put back the way it is during a turn (input field released, cursor where the
 * transcript ended) so the hook can print normally. main.c uses it to fold a finished request
 * into the memory file while the user reads the reply, instead of on the way out. Set
 * term_idle_ms before each term_readline(); typing anything cancels it for that prompt. */
extern void (*term_idle_hook)(void);
extern int term_idle_ms;
/* Messages queued with Enter while the model was generating or a tool was running
 * (like Claude Code). main.c delivers them between tool rounds / after the turn. */
int         term_queue_count(void);
const char *term_queue_peek(void);        /* oldest queued message, or NULL */
char       *term_queue_pop(void);         /* malloc'd, or NULL */
void        term_queue_push(const char *msg);
void        term_queue_clear(void);
void        term_queue_to_editor(void);   /* after an interrupt: hand queued text back to the editor */
char       *term_queue_pop_plain(void);   /* oldest plain message, stepping over queued /commands and !lines */
/* "The user said something since main.c last looked." A message queued after term_queue_mark()
 * stops the work in flight so it reaches the model at once instead of after the task: the tool
 * calls of a round still to run, a shell command, a sub-agent. A message that was already
 * waiting when that work started does not — it is delivered at the normal point. Queued
 * /commands and !lines belong to the REPL and stop nothing. */
int         term_queue_new(void);
void        term_queue_mark(void);        /* everything queued so far is accounted for */
/* Slash commands that only look at state or flip a setting do not have to wait for the turn
 * to end: when Enter is pressed while busy, term.c offers the line to this hook first and
 * only queues it as a message when the hook returns 0. Set by main.c. */
extern int (*term_run_while_busy)(const char *line);
void        term_line_break(void);        /* start a fresh line if output is mid-line (the model may be mid-sentence) */
void        term_editor_prefill(const char *text);   /* text appears in the editor at the next prompt (e.g. after /rewind) */
char       *term_keys_to_text(const unsigned char *keys, size_t n);   /* raw keystrokes -> trimmed text (malloc'd) */
/* Simple prompt for a single line. malloc'd or NULL */
char *term_ask_line(const char *prompt);
/* Interactive yes / always (session) / [always (project)] / no question rendered as a
 * small menu. Returns 1 = yes, 2 = yes-always-this-session, 3 = yes-always-in-project
 * (only offered when project_label != NULL), 0 = no. On "no" the user may type a
 * reason, returned malloc'd in *reason (or NULL). Keys typed before the
 * question appeared are never taken as the answer. */
int term_confirm(const char *question, const char *always_label, const char *project_label, char **reason);

void term_set_slash_commands(const char **cmds, int n);   /* tab completion */
/* Interactive list picker: returns chosen index or -1 if cancelled. */
int term_select(const char *title, const char **items, const char **descs, int n, int current);

/* ---------- markdown-ish streaming printer ---------- */
typedef struct {
    bool in_fence;
    bool in_code;
    bool in_bold;
    bool at_line_start;
    int  pending_ticks;
    bool pending_star;
    int  fence_ticks;   /* ticks in the run at line start */
} md_state;

void md_init(md_state *m);
void md_feed(md_state *m, const char *s, size_t n);
void md_finish(md_state *m);

/* ---------- tools.h ---------- */
typedef enum { TOOL_OK = 0, TOOL_DENIED = 1, TOOL_ERROR = 2 } tool_status;

extern const char *SEARCH_URL_DEFAULT;   /* web_search engine template, %s = the query */
cJSON *tools_definitions(void);   /* array of tool defs (Ollama/OpenAI format), caller owns */
/* Executes tool. Returns status; writes result text into out. */
tool_status tools_execute(const char *name, cJSON *args, sbuf *out);
void tools_reset_permissions(void);       /* forget the "always this session" answers */
/* Persistent per-project permission rules (.corbienest/permissions, one per line:
 * "bash <prefix words>" or "edit"), like Claude Code's project allow-list. */
void        tools_permissions_load(void);          /* (re)read the file for the current directory */
int         tools_permissions_count(void);
const char *tools_permissions_get(int i);
bool        tools_permissions_add(const char *rule);      /* returns false if it exists already; saves */
bool        tools_permissions_remove(int i);              /* saves */
void        tools_permissions_clear(void);                /* removes the file */
/* Checkpoints for /rewind: file states before write_file/edit_file, tagged with the request
 * ("turn" = index into the conversation) they happened in. */
void tools_checkpoint_turn(int turn);              /* main.c: a request starts */
int  tools_checkpoint_files(int turn, sbuf *names);/* files changed in that request or later */
int  tools_checkpoint_restore(int turn);           /* put them back; returns files restored */
void tools_checkpoint_clear(void);
/* The `task` tool runs a sub-agent; main.c provides the implementation (it owns the agent
 * loop). Writes the report into out; returns 0 ok, nonzero error. */
typedef int (*tools_subagent_fn)(const char *description, const char *prompt, sbuf *out);
extern tools_subagent_fn tools_subagent;
/* The `advisor` tool puts a question to a stronger model (/advisor); main.c provides it too, since
 * what the advisor is shown is the conversation. Writes the advice (or why there is none) into
 * out; returns 0 ok, nonzero error. Offered to the model only while g_cfg.advisor is set. */
typedef int (*tools_advisor_fn)(const char *question, sbuf *out);
extern tools_advisor_fn tools_advisor;   /* how many one request may make: advisor_guidance()->uses */
extern bool tools_no_confirm;   /* while true, tools run without asking (user-typed "!cmd") */
const char *tools_summary_line(void);   /* short list for help */

/* ---------- skills.h ---------- */
typedef struct {
    char *name, *desc, *path, *dir, *body;
    const char *source;   /* "project" or "user" */
} skill_t;
int   skills_load(void);                 /* (re)scan skill directories, returns count */
int   skills_count(void);
const skill_t *skill_get(int i);
const skill_t *skill_find(const char *name);   /* with or without leading '/' */
char *skill_expand(const skill_t *s, const char *args);   /* prompt text, malloc'd */
char *skills_prompt_section(void);       /* system prompt section, malloc'd or NULL */
int   skill_scaffold(const char *name, char *path_out, size_t n);   /* 0 ok, 1 exists, -1 error */

/* ---------- ollama.h ---------- */
typedef struct {
    int    prompt_tokens;
    int    eval_tokens;
    double eval_seconds;
    double prompt_seconds;   /* prompt evaluation ("prefill") time; large = the KV cache missed */
    double think_seconds;    /* time from the first thinking chunk to the first content/tool chunk */
    int    think_chunks;     /* streamed thinking chunks (≈ tokens) */
    double total_seconds;
    double load_seconds;     /* model load time (0 when it was already loaded) */
    char   done_reason[16];  /* "stop", "length" (num_predict hit), "" if unknown */
} chat_stats;

/* What /api/show says about one model. /api/tags lists capabilities too, but from the manifest
 * as it was pulled: a model whose template learned tools or thinking since then still shows up
 * without them there, so for the model in use this is the one to believe.
 *
 * The think_* fields are what "think" may be set to for this model — its effort levels. Ollama
 * from 0.34.3 says so itself (thinking.values in /api/show: booleans and/or level names);
 * older servers only say *that* a model thinks, so for them model_think_profile() knows the
 * few families that act on a level and treats every other thinking model as on/off. */
#define EFFORT_LEVELS_MAX 8
#define EFFORT_NAME_MAX   16
typedef struct {
    int  context_length;   /* trained context length, 0 if unknown */
    int  draft;            /* the model's own draft_num_predict, -1 when it has none */
    char family[48];       /* details.family ("gptoss", "qwen35", …), "" if unknown */
    char renderer[32];     /* the Modelfile's RENDERER ("qwen3.8"), "" if none: tells qwen3.8 from its family */
    bool caps_known;       /* the server sent capabilities[] (older ones do not) */
    bool tools, thinking;  /* only meaningful when caps_known */
    bool think_off, think_on;   /* takes false / true as choices of their own (gpt-oss cannot stop thinking) */
    char think_levels[EFFORT_LEVELS_MAX][EFFORT_NAME_MAX];   /* named levels, weakest first */
    int  n_think_levels;
    char think_default[EFFORT_NAME_MAX];   /* what it does when "think" is left out: "off", "on", a level; "" unknown */
    bool think_declared;   /* the list came from the server rather than from model_think_profile() */
    bool think_off_top;    /* its "off" is written into the top of the prompt like a level (glimmer: "Reasoning strength: none."), so it may not flip mid-prompt either */
    bool think_adaptive, think_budget;   /* a hosted Anthropic model: takes thinking {type: adaptive} / {type: enabled, budget_tokens} (provider.c) */
} model_info;
void   model_info_parse(const char *json, model_info *mi);      /* the /api/show body (exposed for tests) */
void   model_think_profile(model_info *mi);   /* fill think_* from family/renderer unless the server declared them; call again after changing `thinking` */
int    ollama_model_show(const char *model, model_info *mi);    /* 0 ok, -1 unknown model / request failed (*mi is still zeroed) */
extern model_info g_model_info;   /* the model in use (set by main.c) */

/* ---------- effort: how hard a model thinks (/effort) ----------
 * Kept per model, because the levels are the model's: gpt-oss has low/medium/high and cannot
 * stop thinking, qwen3.8 has off/low/medium/high, most others are on or off. "off" and "on" are
 * the booleans; anything else is a level name, sent as it is. No entry = leave it to the model. */
const char *effort_get(const char *model);                     /* NULL = nothing saved for it */
void        effort_set(const char *model, const char *level);  /* NULL = forget it (does not save the config) */
bool        effort_name_ok(const char *s);                     /* fit to store and to send: a-z 0-9 - _ */
/* Can this model be set to `level`? 1 yes, 0 no, -1 it cannot be known: the model thinks but
 * nothing says in what levels, and such a server takes low/medium/high/max as plain "on". */
int         effort_supported(const model_info *mi, const char *level);
/* What to send for a saved level: itself, or — for high/xhigh/max, which all mean "as hard as it
 * goes" — the strongest level the model does have (*mapped). NULL = nothing it can be set to. */
const char *effort_resolve(const model_info *mi, const char *want, bool *mapped);
/* What is sent as "think" with one call. `when` is g_cfg.think; `quiet`: the call needs no
 * thinking (a summary, the memory update); `followup`: it continues the prompt of the call
 * before it (a tool round, a compaction). That matters because a level is written into the
 * *top* of the prompt by the models that have them: changing it between two calls makes the
 * server read the whole conversation again, so a level is never dropped mid-prompt, and a
 * model that cannot stop thinking is never told to. *level is set for THINK_LEVEL. */
typedef enum { THINK_OMIT, THINK_FALSE, THINK_TRUE, THINK_LEVEL } think_kind;
think_kind  think_decide(int when, bool quiet, bool followup, const char *effort, const model_info *mi, const char **level);

/* Per-call overrides for ollama_chat(); set before a call and reset with ollama_call_reset()
 * (like ollama_quiet). Callers that don't touch them get the session defaults. */
typedef struct {
    int         think;        /* -1 = as configured, 0 = this call needs no thinking (see think_decide: quiet) */
    int         num_predict;  /* >0 = cap on generated tokens */
    const char *busy;         /* status-bar label while generating (default "generating") */
    bool        followup;     /* continues the prompt of the call before it (see think_decide) */
    bool        stop_on_message; /* give up when the user queues a message (*aborted, ollama_stopped_for_message) */
    /* a call to another model than g_cfg.model — the advisor — brings what goes with it: */
    const char *model;        /* NULL = g_cfg.model */
    const model_info *info;   /* that model's /api/show (NULL with model = it cannot think, as far as we know) */
    int         num_ctx;      /* >0 = its own context window instead of g_cfg.num_ctx, <0 = leave the key out */
    const char *keep_alive;   /* non-NULL = instead of g_cfg.keep_alive ("" = leave the key out) */
    int         idle_ms;      /* >0 = how long the server may stay silent (loading, reading the prompt) instead of http_idle_timeout_ms */
} ollama_call_opts;
extern ollama_call_opts ollama_call;
void ollama_call_reset(void);

/* Streams a chat completion. `messages` is a cJSON array (borrowed).
 * On success returns a new cJSON assistant message object (caller owns).
 * On error/abort returns NULL and sets *aborted / prints error. */
cJSON *ollama_chat(cJSON *messages, cJSON *tools, chat_stats *stats, bool *aborted);
extern bool ollama_quiet;   /* when set, ollama_chat() does not print the streamed reply (background calls) */
extern bool ollama_stopped_for_message;   /* the last call was given up for a queued message, not by Esc/Ctrl-C */
int ollama_poll_or_message(void);         /* http_interrupt_check for such a call: Esc/Ctrl-C, or a newly queued message */
/* Error text of the most recent ollama_chat() ("" when it succeeded or was interrupted), so the
 * caller can react to a specific server error instead of only seeing a NULL reply. */
extern char ollama_error[512];
/* Fetch model names. Returns cJSON array of strings (caller owns) or NULL. */
cJSON *ollama_list_models(void);
int    ollama_ping(char *ver, size_t verlen);
/* Context length the model was trained for (from /api/show), 0 if unknown. */
int    ollama_model_context_length(const char *model);
/* The model's own draft_num_predict (speculative decoding / MTP draft head), from /api/show
 * parameters; -1 when the model has none. */
int    ollama_model_draft(const char *model);
/* Where a loaded model lives (from /api/ps): total bytes and bytes in GPU memory.
 * Returns 0 and fills both when the model is loaded, -1 if not loaded/unknown. */
int    ollama_model_placement(const char *model, double *size, double *size_vram);
/* Recover tool calls that a model emitted as text (exposed for tests). */
cJSON *parse_text_tool_calls(const char *content);

/* ---------- provider.c: hosted model APIs, for the advisor ----------
 * Besides a model on the Ollama server the advisor can be a hosted one, named PROVIDER:MODEL:
 * "xai:grok-4.7", "openai:gpt-5.2", "anthropic:claude-opus-5" (grok: and claude: work too). The
 * key comes from the environment and is never saved. */
typedef enum { PROVIDER_CHAT_COMPLETIONS, PROVIDER_MESSAGES } provider_style;
typedef struct {
    const char *name, *alias, *label;      /* "xai", "grok", "xAI" */
    const char *key_env, *url_env, *url;   /* XAI_API_KEY, XAI_BASE_URL, the base URL otherwise */
    provider_style style;                  /* Chat Completions (xAI, OpenAI) or Anthropic's Messages API */
} provider_def;
const provider_def *provider_find(const char *advisor, const char **model);   /* NULL = a model on the Ollama server */
const char *provider_base_url(const provider_def *p);
typedef struct {
    const char *system, *user;   /* the two messages of a consultation */
    int         max_tokens;      /* the answer and its thinking together */
    const char *effort;          /* the saved level (effort_get), resolved against the model_info */
    const char *busy;            /* status-bar label */
    int         idle_ms;         /* >0 = how long the server may stay silent */
} provider_request;
/* What the model can be set to (effort levels, window), from the API's model endpoint. 0 known,
 * 1 not checked (unreachable; *mi holds what the provider's models take between them), -1 not
 * usable (no key, key refused, no such model). err says why for 1 and -1. */
int   provider_model_info(const char *advisor, model_info *mi, char *err, size_t n);
/* One consultation. The answer (malloc'd, "" when there is none) or NULL: *aborted when the user
 * stopped it (ollama_stopped_for_message: by queuing a message), otherwise err says why. */
char *provider_chat(const char *advisor, const model_info *mi, const provider_request *rq, chat_stats *st, bool *aborted, char *err, size_t n);
/* the pieces of it, exposed for tests */
char *provider_request_body(const char *advisor, const model_info *mi, const provider_request *rq);
char *provider_parse_reply(const char *advisor, const char *json, int status, chat_stats *st, char *err, size_t n);
void  provider_parse_model(const char *advisor, const char *json, model_info *mi);
bool  provider_fallbacks(const char *advisor);   /* an Anthropic model with server-side refusal fallbacks (Opus 5, Fable 5.x) */

#endif
