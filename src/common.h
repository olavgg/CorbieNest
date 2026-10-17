/* corbienest - a Claude-Code-style TUI for local Ollama models, in C. */
#ifndef CORBIE_COMMON_H
#define CORBIE_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>
#include <cjson/cJSON.h>

#define CORBIE_VERSION "0.3.0"

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

/* ---------- themes ----------
 * A theme is the palette both screens draw from. The basic codes above are tokens: the ANSI screen maps them to the
 * theme's RGB as the bytes leave (theme_map_bytes), the notcurses screen maps them as it parses (nc_sgr_parse).
 * theme.c holds the themes; "theme=NAME" in the config picks one. */
typedef struct {
    const char *name;
    unsigned text, muted, rule, menu;           /* the text, the dimmed text, the rules, a menu's ground */
    unsigned window;                            /* the window's ground and the text's default colour; 0 = the terminal's own */
    unsigned bar_a, bar_b;                      /* the status bar: from bar_a to bar_b */
    unsigned field_text;                        /* what is typed in the field (the field has the window's own background) */
    unsigned edge_ready, edge_idle;             /* the field's edges while the prompt waits, and while a turn runs */
    unsigned loc;                               /* the location line */
    unsigned select;                            /* the row the keys are on, in the sub-agents strip */
    unsigned user_bg, user_text;                /* the ground and the text of a line the user sent, in the conversation */
    unsigned brand, spark_hi, spark_mid, spark_lo;   /* the banner: its title and the spark's head, then its tail */
    unsigned crow_dark, crow_body, crow_light, crow_beak;   /* the banner's crow: outline, body, highlight, beak */
    unsigned accent, ok, bad, warn, info, blue, magenta, gray;   /* the orange, and the basic codes 32, 31, 33, 36, 34, 35, 90 */
} theme_t;
typedef struct {
    char pend[32]; int np;                      /* an escape sequence the last write ended inside, not yet written */
} theme_map;
extern const theme_t *g_theme;
bool theme_set(const char *name);               /* "theme=NAME": false, and the theme stays, when there is none */
int  theme_count(void);                         /* the themes there are, for the picker and the completions */
const char *theme_name(int i);
bool theme_fg_code(int code, unsigned *rgb);    /* the theme's colour of a basic fg code (30-37, 90-97) */
void theme_paint_window(bool on);              /* the full-screen ANSI screen paints the theme's window ground */
const char *theme_base_sgr(void);              /* the SGR for the window's ground and text ("" when none) */
/* Theme roles as SGR parameters: a line stored with one is coloured by the theme when it is drawn, so a switch
 * recolours the conversation already on screen (the ANSI screen maps it in theme_map_bytes, notcurses in nc_sgr_parse). */
#define THEME_ROLE_USER_TEXT 1001   /* the text of a line the user sent (theme user_text) */
#define THEME_ROLE_USER_BG   1002   /* the ground of a line the user sent (theme user_bg) */
bool theme_role(int code, unsigned *rgb, bool *bg);   /* the colour a role code stands for, and whether it is a background; false if it is no role */
void theme_recolor(const char *s, sbuf *out, const unsigned *from, const unsigned *to, int count);   /* the 38;2 colours in s that are from[i] become to[i] */
void theme_map_bytes(theme_map *m, const char *in, size_t n, sbuf *out);   /* the bytes with their themed codes mapped */
bool theme_map_pending(const theme_map *m);     /* the last write ended inside an escape sequence: its bytes are not out yet */
const char *th_fg(unsigned rgb);                /* "\x1b[38;2;R;G;Bm" for an RGB colour (a short-lived string) */
const char *th_bg(unsigned rgb);                /* the background form */

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

/* Finding the file a half-typed name means (the suggestions for "@file", and "@name" as sent).
 * mention_start(): the offset just after the '@' of the mention the cursor (byte `cur`) is in —
 * an '@' that starts a word — or -1. file_complete(): up to `max` paths for what was typed after
 * it, best first: `typed` may carry a directory ("../lib/pa", "~/", "src/") — the search is below
 * that one, the working directory otherwise — and its last part is matched against file names:
 * those starting with it, those containing it, those with its letters in order. Paths come back
 * the way they would be typed, a directory with a trailing '/'. file_find_named(): the one file
 * below the working directory with that name (or path ending), malloc'd; NULL when there is none
 * or more than one — *count says how many, and `names` (if given) lists the first few. */
typedef struct { char *path; bool dir; } file_match;
long  mention_start(const char *buf, size_t cur);
/* The "@path" mentions in buf[0..len) that name a file or a directory that is there, as
 * [start, end) byte ranges from the '@' on: what the input field colours. At most max. */
int   mention_spans(const char *buf, size_t len, size_t (*spans)[2], int max);
int   file_complete(const char *typed, bool dirs_only, int max, file_match **out);
void  file_matches_free(file_match *m, int n);
char *file_find_named(const char *name, int *count, sbuf *names);

/* The same for a "!line", which is the shell's. shell_word(): where the word the cursor (byte
 * `cur`) is in starts, and whether a command goes there (*command) rather than an argument; -1
 * inside quotes — that is text. command_complete(): the programs in $PATH whose name starts
 * with `typed`, the shortest first, each once, at most `max` (the name is in .path).
 * shell_escape(): a path as it has to be written there, malloc'd — a backslash before what the
 * shell would take apart. */
long  shell_word(const char *buf, size_t cur, bool *command);
int   command_complete(const char *typed, int max, file_match **out);
char *shell_escape(const char *s);

/* A pasted block the editor shows as a token, "[Pasted text #K +N lines]" (term.c): K numbers the
 * paste within the prompt, N is its line count. paste_tokens() finds the tokens in a line of
 * input (sp: start and end bytes, ks: K, ns: N, each optional), paste_expand() puts the texts
 * back for the ones that are still whole: K in 1..n and N equal to lines[K-1]. */
size_t paste_tokens(const char *s, size_t len, size_t (*sp)[2], int *ks, int *ns, size_t max);
char  *paste_expand(const char *s, size_t len, const char *const *texts, const int *lines, int n);

/* A sub-agent's log (main.c keeps one per run, shown by ← at the prompt): plain text appended to
 * `log`, which is cut from its front, at a line, once it is past `cap` bytes. */
void  agent_log_add(sbuf *log, const char *text, size_t cap);

/* URLs and HTML, for the web_fetch tool (in util.c so the unit tests can reach them) */
bool  url_ok(const char *url);                                  /* http(s), sane, not cloud metadata */
bool  url_host(const char *url, char *out, size_t n);           /* "host[:port]", lowercased */
bool  url_hostname(const char *url, char *out, size_t n);       /* just the host, of a URL or a host setting ("gpu-box:11434"), lowercased */
bool  url_is_local(const char *url);                            /* the host is this machine (localhost, 127.x, ::1, 0.0.0.0, none) */
bool  url_cleartext(const char *url);                           /* not https (no scheme = http) */
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
/* UTF-8 for a request body (the server refuses a body that is not valid JSON): */
bool   utf8_ok(const char *s, size_t len);
bool   tail_incomplete(const char *buf, size_t n);   /* ends inside an escape sequence or a UTF-8 character */
char  *utf8_fix(const char *s);                  /* malloc'd copy, each invalid byte as U+FFFD */
size_t utf8_cut(const char *s, size_t len, size_t cap);   /* the longest prefix of s up to cap bytes that ends on a character */
int    cmd_head(const char *cmd, int width, char *out, size_t n);   /* a shell command's first line for a fold's head; returns the lines after it */
/* Every tool call of an assistant message gets its tool result right after it (a stopped request leaves
 * calls without one, and the server refuses the next request); invalid UTF-8 in content is fixed. Returns
 * how many were fixed. The conversation's own repair, run when a request starts. */
int    messages_repair(cJSON *msgs);

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
 * that changed a lot ends (any change at max), and (max) a check of the request's first change before it is made. */
enum { GUIDANCE_LIGHT, GUIDANCE_NORMAL, GUIDANCE_STRONG, GUIDANCE_MAX, GUIDANCE_COUNT };
typedef struct {
    const char *name;
    int    uses;               /* consultations the agent may ask for in one request */
    int    words;              /* how long the advice may be */
    size_t brief_max;          /* bytes it is shown at most, whatever the window: reading it is what the user waits for */
    bool   review;             /* it reviews a request that changed a lot before the request ends */
    bool   review_any;         /* that review is for any change; without it, only for a substantial one (ADVISOR_REVIEW_LINES / _FILES) */
    bool   check_first_edit;   /* it checks the first change of a request before the change is made */
    bool   step_in;            /* it is brought in, unasked, when the agent's tool calls keep failing (ADVISOR_FAIL_ROUNDS) */
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
#define ADVISOR_WINDOW_DEFAULT (15 * 60)   /* a long request gets its consultations again every quarter of an hour */
#define ADVISOR_FAIL_ROUNDS 2    /* rounds of tool calls in a row with a failure in them before the advisor steps in (step_in) */
#define ADVISOR_REVIEW_LINES 40  /* changed lines from which a request is reviewed at guidance strong (review, not review_any) */
#define ADVISOR_REVIEW_FILES 3   /* or files that differ from which it is reviewed at strong */
int   diff_changed_lines(const char *d);                  /* lines of a unified diff that add or remove something (not the --- +++ headers) */
bool  tool_result_failed(const char *name, const char *result, bool error);   /* an error result, or a shell command that did not exit with 0 */
#define ADVISOR_REVIEW_OK "LGTM"   /* what the advisor answers a review or a check with when nothing needs to change */
bool  advisor_approves(const char *advice);   /* the answer is ADVISOR_REVIEW_OK and not much more */
const char *strip_think_block(const char *s);   /* past a leading <think>…</think> */
bool  model_same(const char *a, const char *b);   /* equal, a trailing ":latest" aside */
bool  model_is_cloud(const char *name);           /* NAME:cloud / NAME-cloud */

/* ---------- global config ---------- */
typedef struct { char *model; double in, out; } price_entry;   /* USD per million tokens, set with /usage price */
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
    price_entry *prices;   /* what a model costs where the built-in list does not know or is out of date; saved as price.<model>=<in>/<out> */
    int   n_prices;
    char *advisor;       /* the stronger model the agent may consult through the advisor tool (/advisor); NULL = none */
    int   advisor_ctx;   /* num_ctx of an advisor call; 0 = auto (see advisor_plan_for()) */
    char *subagent;      /* the model sub-agents (task) run on; NULL or the main model = the model doing the work (/subagent) */
    int   advisor_guidance; /* how much the agent leans on it, GUIDANCE_* (/advisor guidance) */
    int   advisor_window;   /* seconds after which a request's count of consultations starts again (/advisor window, given in minutes); 0 = never */
    int   workers;       /* tasks of /orchestrate that may run at the same time (/workers); 0 = ORCH_WORKERS_DEFAULT */
    char *build;         /* /plan: the command that builds the project after each layer (build=); NULL = detected from the project's build file */
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
    char *ui;            /* "ansi" (default) or "notcurses" (needs a build made with notcurses installed); NULL = ansi */
    char *theme;         /* "theme=NAME": the colour theme (theme.c); NULL = the default one */
} config_t;

extern config_t g_cfg;

/* per-session token accounting (shown in the status bar and /status) */
typedef struct {
    long prompt_tokens;      /* sum of prompt (input) tokens over all calls, cached or not */
    long cache_read;         /* the part of prompt_tokens a hosted provider read from its prompt cache (billed at a fraction) */
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

/* The rate limits an Anthropic response reports for itself (anthropic-ratelimit-* headers): what
 * is left of the limit at the moment of the response. Counts are per minute, as the API gives them. */
typedef struct {
    bool      have;       /* the response carried any of them */
    long long req_lim, req_rem, in_lim, in_rem, out_lim, out_rem;
    char      reset[40];  /* when the input-token limit is back in full (RFC 3339, as sent) */
} ratelimit_info;

typedef struct {
    int   status;         /* HTTP status, 0 if none */
    bool  aborted;        /* aborted by callback / interrupt key */
    char  err[512];       /* error text if return < 0 */
    ratelimit_info rl;    /* the response's rate-limit headers, when it had them */
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
/* A POST that a thread of its own performs — the model call of one of several workers. Started
 * and collected on the main thread; see http.c for what the thread may touch (its own handle). */
typedef struct http_job http_job;
http_job *http_job_start(const char *base_url, const char *path, const char *body, const char *const *headers);   /* headers NULL = none; NULL return = it could not be started */
bool      http_job_done(http_job *j);
void      http_job_cancel(http_job *j);                               /* it ends soon after, as aborted */
int       http_job_finish(http_job *j, sbuf *out, http_result *res);  /* waits for it and frees it; returns as http_request() does */
bool http_busy(void);   /* a request is in flight: do not start another (their settings are globals) */
char *http_url(const char *base, const char *path);   /* base + path; "host" and "http://host" get Ollama's port 11434 (malloc'd) */

/* ---------- term.h ---------- */
void term_init(void);
void term_restore(void);
void term_finish(void);       /* the program is ending: restores the terminal, and output after it is not the screen's */
void term_raw(bool on);
int  term_width(void);
void term_size(int *rows, int *cols);
/* The file CORBIENEST_TRACE names, while everything drawn and typed is being written to it for
 * tools/trace_replay.py (see "trace" in term.c); NULL when it is not. */
const char *term_trace_path(void);
void term_clear_screen(void);

/* Full-screen mode: alternate screen with the bottom row reserved for a status
 * bar (permission mode · model · session tokens · context usage), like Claude Code.
 * term_status_refresh() redraws the bar; call it after mode/model/token changes.
 * term_status_live() shows output tokens of an in-flight generation (0 = none). */
void term_fullscreen(bool on);
bool term_is_fullscreen(void);                        /* the alternate screen with its bottom chrome is up (ANSI) */
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
bool hist_contains(const char *line);   /* a line is somewhere in the history (not only the latest) */
int  hist_count(void);
const char *hist_get(int i);   /* 0 = oldest kept … hist_count()-1 = most recent */

/* Interactive line editor. Returns malloc'd string, or NULL on EOF (Ctrl-D). */
char *term_readline(const char *prompt);
char *term_user_band(const char *lead, const char *text);
void  nc_echo_line(const char *prompt, const char *s);   /* notcurses: the line sent goes into the conversation (term.c) */   /* a line the user sent, on the user's ground (theme user_bg), padded to the width; malloc'd */

/* Read a single key while in raw mode. Returns the byte or -1. */
int term_getkey(void);
/* Non-blocking: drain pending stdin into the type-ahead buffer; returns 1 if Ctrl-C/Esc was pressed.
 * Enter while busy turns the pending text into a queued message (see below). */
int term_poll_interrupt(void);
int term_interrupt_fd(void);                /* the fd a busy wait watches for keys: stdin, or notcurses' readiness pipe */
/* Background work at an idle prompt. When term_idle_ms > 0, the editor runs term_idle_hook()
 * once per prompt after that many milliseconds without a keystroke on an empty line, with the
 * terminal put back the way it is during a turn (input field released, cursor where the
 * transcript ended) so the hook can print normally. main.c uses it to fold a finished request
 * into the memory file while the user reads the reply, instead of on the way out. Set
 * term_idle_ms before each term_readline(); typing anything cancels it for that prompt. */
extern void (*term_idle_hook)(void);
extern int term_idle_ms;
/* ← on an empty prompt line calls term_agents_menu() with the terminal as the idle hook gets it
 * (main.c: the sub-agents of this session). term_show_block() shows a text as an open fold and
 * moves the view to its head, for the hook to show what it chose: the rest of the block is plain. */
extern void (*term_agents_menu)(void);
/* the agents strip (term.c's strip_draw): the running sub-agents' lines, and the open of the one picked (main.c) */
extern int  (*term_agents_strip)(sbuf *o, int cols, int max);
extern void (*term_agents_open)(int idx);
int  term_strip_update(const char **text);   /* the strip's rows, refreshed (notcurses draws it) */
enum { STRIP_OTHER = 0, STRIP_UP, STRIP_DOWN, STRIP_ESC, STRIP_ENTER };
int  term_strip_sel(void);                    /* the row the keys are on (-1: the field) */
bool term_strip_key(int key, bool may_enter);   /* a key on the strip (STRIP_*); true when the strip took it */
void term_show_block(const char *head, const char *body);
/* Messages queued with Enter while the model was generating or a tool was running
 * (like Claude Code). main.c delivers them between tool rounds / after the turn. */
int         term_queue_count(void);
const char *term_queue_peek(void);        /* oldest queued message, or NULL */
char       *term_queue_pop(void);         /* malloc'd, or NULL */
void        term_queue_push(const char *msg);
void        term_queue_clear(void);
void        term_queue_to_editor(void);   /* after an interrupt: hand queued text back to the editor */
char       *term_queue_pop_plain(void);   /* oldest plain message, stepping over queued /commands and !lines */
/* Slash commands that only look at state or flip a setting do not have to wait for the turn
 * to end: when Enter is pressed while busy, term.c offers the line to this hook first and
 * only queues it as a message when the hook returns 0. Set by main.c. */
extern int (*term_run_while_busy)(const char *line);
/* A long result, folded: `head` is the one line that stays on screen and has TERM_FOLD_CLOSED in
 * it; what is printed between the two calls is its body, out of sight until the head is clicked.
 * False = no folding here (not in full-screen mode): print a preview instead. */
#define TERM_FOLD_CLOSED "▸"
#define TERM_FOLD_OPEN   "▾"
bool        term_fold_begin(const char *head);
/* rate limits: the anthropic-ratelimit-* headers of a response (util.c); per model, the last seen */
void        ratelimit_header(ratelimit_info *rl, const char *line, size_t n);
void        ratelimit_set(const char *model, const ratelimit_info *rl);
bool        ratelimit_get(const char *model, ratelimit_info *out);
/* a text in a centred window over the conversation, while the model may keep working underneath
 * (Esc closes it). false when there is no full-screen conversation to draw it on: print it instead */
bool        term_overlay_show(const char *text);
void        term_overlay_close(void);
void        term_fold_end(void);
int         term_fold_toggle(int line);   /* the fold whose head is scrollback line `line`: shown or hidden; the body lines flipped, -1 if no head there */
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

/* Suggestions under the input field while a "/command", an "@file" or a "!line" is being typed,
 * and what Tab completes. main.c supplies them: for the text `buf` with the cursor at byte `cur`
 * the hook returns how many there are (0 = none) in *items — malloc'd, like the strings in it;
 * term.c frees them — and sets *from to where the word they would replace starts. `open` marks
 * one that more is going to follow (a directory): no space is put after it and the list stays
 * up. The hook is also asked about what is typed while the model works, i.e. from inside the
 * poll of a live request: it may read whatever it likes and must change nothing. */
typedef struct { char *text; char *desc; bool open; } term_sug;
extern int (*term_suggest)(const char *buf, size_t cur, size_t *from, term_sug **items);
/* Interactive list picker: returns chosen index or -1 if cancelled. */
int term_select(const char *title, const char **items, const char **descs, int n, int current);
void term_repaint(void);                       /* the ANSI conversation painted again from the model (after a theme change) */

/* What term_nc.c draws from, and what term.c exports for it (the ANSI UI uses none of this) */
extern bool g_ui_nc;                                  /* the notcurses frontend is the screen */
extern bool term_confirm_quiet;                        /* the next term_confirm does not echo its answer (a bash call's line says it) */
void        term_status_text(sbuf *o, int cols);      /* the bar, as the ANSI UI draws it (escapes included) */
void        term_wrap(const char *s, size_t len, int width, int col0, size_t target, int want, size_t *start_out, int *row_out, int *col_out);   /* the field's text wrapped at word boundaries (term.c) */
bool        term_is_ready(void);                      /* the top-level prompt is waiting: the field says [ready] in green */
int         term_mode_text(sbuf *o, int cols);        /* the mode for the field's lower rule within cols (its hint dropped when it does not fit); returns its width, 0 if none */
int         term_sb_nlines(void);                     /* the conversation: the scrollback model's lines */
void        term_tick_set(void (*fn)(bool still));     /* the banner: called every TICK_MS while the top-level prompt waits; still=true once, when it returns */
bool        term_banner_show(const char **lines, int n, int bw);   /* the banner's modal (its lines, the box's width): up; false when there is no screen for it */
void        term_banner_draw(const char **lines, int n, int bw);   /* the modal's lines again (a tick) */
void        term_banner_hide(void);
bool        term_banner_open(void);
#define BANNER_X_FROM_RIGHT 3   /* the close button's centre: this many cells in from the box's right edge (the corner is 1): 2 cells left of the corner */
#define BANNER_X_W 3            /* the close button is "[×]": its three cells are the click target */
int         term_tick_wait(void);                      /* ms until the next tick (-1: no tick is running) */
void        term_tick_run(void);                       /* runs the tick if it is due (notcurses' loop calls it) */
int         term_sb_epoch(void);                      /* changes whenever the lines are renumbered (dropped, cleared): a row map made before is stale */
const char *term_sb_line(int i);
bool        term_sb_hidden(int i);                    /* a closed fold's body */
void        term_sgr_track(sbuf *st, const char *d, size_t len);   /* the colours in effect after d: what a wrapped row continues in */
int         term_sb_col(void);                        /* column the output is at (0 = a fresh line) */

/* ---------- term_nc.c: the notcurses frontend ----------
 * Always compiled; with WITH_NOTCURSES (notcurses found by make) they drive notcurses, without it they are
 * stubs and g_ui_nc stays false. nc_sgr_parse() is pure and works in both builds (test_unit). */
typedef struct { unsigned fg, bg; unsigned styles; } nc_style;   /* fg/bg: 0xRRGGBB or NC_DEFAULT; styles: NC_ST_* */
#define NC_DEFAULT 0xFFFFFFFFu
#define NC_ST_BOLD    1u
#define NC_ST_DIM     2u
#define NC_ST_ITALIC  4u
#define NC_ST_UNDER   8u
#define NC_ST_REVERSE 16u
typedef struct { char egc[8]; unsigned char w; nc_style st; } nc_cell;   /* one character as drawn: UTF-8, columns, style */
/* Parses the escape sequence at s (length n): a CSI is consumed whole (its SGR applied to st when it
 * is one); returns the bytes consumed, or 0 when s does not start a CSI. */
size_t nc_sgr_parse(const char *s, size_t n, nc_style *st);
unsigned nc_color256(int n);                           /* the xterm 256-colour palette, 0xRRGGBB */
/* the characters of s (SGR applied on the way, carried in st when given), written to out up to max
 * (out may be NULL to count); returns how many there are */
int    nc_cells(const char *s, size_t n, nc_cell *out, int max, nc_style *st);
int    nc_line_rows(const nc_cell *c, int n, int width);   /* the rows a line takes when wrapped at width */
int    nc_text_width(const char *s);                       /* its width in columns */
bool   nc_compiled(void);                             /* this build has the notcurses frontend */
bool   nc_start(void);                                /* takes the screen; false (and a message on stderr) if it cannot */
void   nc_stop(void);                                 /* gives it back; idempotent */
bool   nc_size(int *rows, int *cols);                 /* the screen's size, when the frontend runs */
void   nc_render(void);                               /* draws the pane, field, bar and location line now */
void   nc_render_soon(void);                          /* the same, at most every few tens of ms */
void   nc_output(void);                               /* a write reached the conversation */
void   nc_clear(void);                                /* the conversation starts over at the top */
void   nc_prefill(const char *text);                  /* text for the field at the next prompt */
void   nc_leave_browse(void);                         /* a history browse ends: the line typed before it is back in the field */
char  *nc_readline(const char *prompt, bool in_field);
int    nc_poll_interrupt(void);                       /* keys read while the model works: 1 = stop */
int    nc_input_fd(void);                             /* the fd that signals keys to read (-1: none) */
int    nc_confirm(const char *question, const char *always_label, const char *project_label, char **reason);
int    nc_select(const char *title, const char **items, const char **descs, int n, int current);
void   nc_banner_set(const char **lines, int n, int bw, bool on);   /* the banner's modal: its lines and width, shown or not */
bool   nc_banner_open(void);                          /* the banner's modal is up */
void   nc_overlay_set(const char *text);              /* the overlay (/usage and the like) as a modal over the pane; NULL closes it */
bool   nc_overlay_open(void);

/* Basic syntax highlighting of one line of code, by the fence's language (or NULL/unknown: unchanged). Only
 * escapes are added; each token returns to `base`. Keywords, strings, comments, numbers. */
void highlight_line(const char *lang, const char *line, sbuf *out, const char *base);

/* ---------- markdown-ish streaming printer ---------- */
typedef struct {
    bool in_fence;
    bool in_code;
    bool in_bold;
    bool at_line_start;
    int  pending_ticks;
    bool pending_star;
    int  fence_ticks;   /* ticks in the run at line start */
    bool in_info;       /* in a fence's info string (the language, after the opening ```) */
    sbuf info;          /* that info string */
    sbuf line;          /* the code line being collected, printed highlighted when it ends */
    char lang[24];      /* the fence's language (first word of its info string, lower case) */
    sbuf word;          /* the prose word being collected: printed when its end comes, so a word is never split across lines */
    int  col;           /* the columns the prose has taken on this line (the word wrap's count) */
} md_state;

void md_init(md_state *m);
void md_feed(md_state *m, const char *s, size_t n);
void md_finish(md_state *m);

/* ---------- tools.h ---------- */
typedef enum { TOOL_OK = 0, TOOL_DENIED = 1, TOOL_ERROR = 2 } tool_status;

extern const char *SEARCH_URL_DEFAULT;   /* web_search engine template, %s = the query */
cJSON *tools_definitions(void);   /* array of tool defs (Ollama/OpenAI format), caller owns */
/* Executes tool. Returns status; writes result text into out. */
enum { VERDICT_NONE = 0, VERDICT_ALLOW = 1, VERDICT_ASK = 2, VERDICT_DENY = 3 };   /* what a project rule says about a target */
bool glob_match(const char *pat, const char *s);   /* * and ** match any text (a / too), ? one character; the whole string */
int  perm_rule_verdict(const char *rule, const char *tool, const char *target);   /* one saved rule on a target of "bash", "edit" or "fetch" */
bool  hook_match(const char *line, const char *event, const char *tool, const char **cmd);   /* a hook line of this event (and tool, for pre/post): its command */
void  tools_hooks_load(void);          /* the project's hooks (the config directory's hooks/ file) */
int   tools_hooks_count(void);
const char *tools_hooks_get(int i);
bool  tools_hooks_add(const char *line);
bool  tools_hooks_remove(int i);
void  tools_hooks_clear(void);
void  tools_bg_init(void);            /* background jobs: atexit and SIGTERM/SIGHUP stop them */
void  tools_bg_kill_all(void);       /* stops the running background jobs now (before a re-exec too) */
void  tools_hook_event(const char *event);   /* "start" or "stop": runs the hooks of that event and shows what they print */
cJSON *tools_todos_get(void);          /* the task list the model keeps with todo_write, or NULL */
void   tools_todos_set(cJSON *list);   /* replaces it (takes the list); NULL clears it */
void   tools_todo_render(sbuf *out);   /* the list as text: a header with the count, then a line a task */
tool_status tools_execute(const char *name, cJSON *args, sbuf *out);
void tools_reset_permissions(void);       /* forget the "always this session" answers */
bool tools_exit_asked(void);              /* the model called the exit tool: end the program after this turn */
void tools_exit_clear(void);
/* Persistent per-project permission rules (kept in the config directory, one per line:
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
void tools_checkpoint_step(int step);              /* a part of it that is looked at by itself (a task of /orchestrate); 0 = none */
void tools_checkpoint_path(const char *path);       /* records a file's state before the program changes it outside a tool (/goal's apply) */
bool tools_enter(const char *root);                /* a tool call runs in root (a /goal candidate's worktree): chdir, writes stay inside it; false if it cannot */
void tools_leave(void);                            /* back to the directory tools_enter() left */
int  tools_shell_in(const char *dir, const char *cmd, int timeout_s, sbuf *out, bool *interrupted, const char *busy, const char *verb);   /* a shell command in dir (NULL = here), its exit code; -1 if it could not run */
int  tools_checkpoint_diff(int turn, int step, sbuf *out);   /* what that step did to the files it wrote, as unified diffs; returns files that differ */
int  tools_request_changes(int turn, int *lines);            /* what the request from `turn` changed on disk, against its first write to each file; files that differ, changed lines in *lines */
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
    char *model;          /* the frontmatter's "model:": the model a worker in this role runs on (NULL: the one in use) */
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
    int    prompt_tokens;    /* all input, cached or not: what the context holds */
    int    cache_read;       /* the part of it the provider read from its prompt cache (a hit, billed at a fraction) */
    int    cache_write;      /* the part it wrote to its cache (billed above the input price) */
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
    int  max_output;       /* a hosted model's output cap (max_tokens), 0 if it did not say */
} model_info;
void   model_info_parse(const char *json, model_info *mi);      /* the /api/show body (exposed for tests) */
void   model_think_profile(model_info *mi);   /* fill think_* from family/renderer unless the server declared them; call again after changing `thinking` */
int    ollama_model_show(const char *model, model_info *mi);    /* 0 ok, -1 unknown model / request failed (*mi is still zeroed) */
extern model_info g_model_info;   /* the model in use (set by main.c) */

/* ---------- /orchestrate: the pure half ----------
 * The plan the orchestrating model writes is text — "TASK 1: title", the instructions under it,
 * the next "TASK 2: …" — because that is what every model can write, markdown around it or not.
 * orch_parse_plan() reads it: the tasks in order (at most ORCH_MAX_TASKS, numbered as they come,
 * whatever numbers the model gave them), malloc'd; 0 when the text holds no task. */
#define ORCH_MAX_TASKS 24   /* the "after" set is a 32-bit mask: at most 32 */
#define ORCH_WORKERS_DEFAULT 3   /* tasks that may run at the same time, unless /workers says otherwise */
#define ORCH_WORKERS_MAX 8     /* /goal: candidates, and models for them */
#define ORCH_PARALLEL_MAX 20   /* /orchestrate: tasks that may run at the same time (/workers, workers= in the config) */
typedef struct {
    char *title, *body;
    unsigned after;   /* the tasks (bit i = task i, earlier ones only) that must be accepted before this one starts: an
                       * "AFTER: 1, 3" or "AFTER: none" line of the plan — and without one, the task before it */
    char *role;       /* "ROLE: name" of the plan: the skill that does this task (malloc'd; NULL = the default worker) */
    char **owns; int nowns;   /* "OWNS: a, b": the paths it may write, literal files or directory prefixes (malloc'd) */
    int layer;         /* /plan: "LAYER: n" of the plan, at least 1 and never lower than the task before (1 when the plan says none) */
} orch_task;
#define ORCH_OWNS_MAX 16
int  orch_parse_plan(const char *text, orch_task **tasks);
bool owns_overlap_one(const char *a, const char *b);   /* two OWNS: paths name the same file, or one holds the other */
bool owns_overlap(char **a, int na, char **b, int nb); /* any path of one list overlaps any path of the other */
int  orch_parse_plan_ex(const char *text, orch_task **tasks, char **contract);   /* contract: the "CONTRACT:" block before the first task, malloc'd or NULL */
void orch_tasks_free(orch_task *t, int n);

/* ---------- /goal: the pure half ----------
 * `/goal "METRIC" [min|max] [-n ROUNDS] [-k CANDIDATES] TASK`. The metric is a shell command whose
 * last output line is a number; higher is better unless "min" says otherwise. A candidate ends its
 * report with a line "GOAL: REACHED" or "GOAL: IMPOSSIBLE <why>" (goal_verdict()). */
typedef struct {
    char *metric;      /* the command that prints the score (malloc'd) */
    bool  lower;       /* min: a lower score is better */
    int   rounds;      /* rounds of candidates, at most GOAL_ROUNDS_MAX */
    int   cands;       /* candidates per round, at most ORCH_WORKERS_MAX */
    char *task;        /* what the goal is (malloc'd) */
    char **models; int nmodels;   /* -m a,b,c: candidate k runs models[k % nmodels]; none = the model in use for all (malloc'd) */
} goal_spec;
#define GOAL_ROUNDS_MAX 10
enum { GOAL_NONE, GOAL_REACHED, GOAL_IMPOSSIBLE };
bool goal_parse(const char *arg, goal_spec *g, char *err, size_t errn);   /* on false, err says why; g is then empty */
void goal_spec_free(goal_spec *g);
bool goal_number(const char *out, double *v);                              /* the last non-empty line, as a number */
bool goal_better(double v, double best, bool lower);
int  goal_verdict(const char *report, char *why, size_t n);                /* the last "GOAL:" line of a report */

/* The git branch of a directory: 1 when it is in a repository, with the branch name (or the commit of a
 * detached HEAD) in out; 0 when it is not. Reads the .git of it or of the nearest directory above. */
int   git_branch(const char *dir, char *out, size_t n);

/* ---------- prices: what /usage estimates the cost with ----------
 * USD per million input and output tokens. model_price() says what a model costs: 2 = a price
 * set with /usage price, 1 = the provider's list price as built in (hosted models only, by the
 * name or a dated snapshot of it), 0 = not known. */
int  model_price(const char *model, double *in, double *out);
double model_cache_read_ratio(const char *model);    /* a cache read costs this × the input price (Anthropic: 0.1, 0.05, 0.025; others 1) */
double model_cache_write_ratio(const char *model);   /* a cache write costs this × the input price (Anthropic 1.25; others 1) */
double usage_cost_usd(const char *model, long in, long out, long cache_read, long cache_write, int *known);   /* in = all input, cached included */
void price_set(const char *model, double in, double out);   /* in < 0 = forget it (does not save the config) */

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
/* Error text of the most recent ollama_chat() ("" when it succeeded or was interrupted), so the
 * caller can react to a specific server error instead of only seeing a NULL reply. */
extern char ollama_error[512];
/* Fetch model names. Returns cJSON array of strings (caller owns) or NULL. */
char  *ollama_chat_request(cJSON *messages, cJSON *tools);   /* the body of a call that is not streamed (ollama_call applies); malloc'd */
cJSON *ollama_chat_reply(const char *json, int status, bool tools, chat_stats *st, char *err, size_t n);   /* its answer as the assistant message; NULL + err */
cJSON *ollama_list_models(void);
cJSON *ollama_list_models_quiet(int wait_ms);   /* the same, silently: for the suggestions */
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

/* ---------- provider.c: hosted model APIs ----------
 * A hosted model, named PROVIDER:MODEL — "xai:grok-4.7", "openai:gpt-5.2", "anthropic:claude-opus-5"
 * (grok:, chatgpt: and claude: work too) — can be the advisor, or the model that does the work.
 * The key comes from the environment and is never saved. A turn (provider_turn) speaks the same
 * conversation ollama_chat() does: the messages, the tools, a streamed reply, and an assistant
 * message of the same shape back. A consultation (provider_chat) stays one toolless question. */
typedef enum { PROVIDER_CHAT_COMPLETIONS, PROVIDER_MESSAGES } provider_style;
typedef struct {
    const char *name, *alias, *label;      /* "xai", "grok", "xAI" */
    const char *key_env, *url_env, *url;   /* XAI_API_KEY, XAI_BASE_URL, the base URL otherwise */
    provider_style style;                  /* Chat Completions (xAI, OpenAI) or Anthropic's Messages API */
    const char *example, *known_as;        /* a model of theirs and what people call them ("Grok"), for /advisor's explanation and suggestions */
} provider_def;
const provider_def *provider_at(int i);   /* the providers in turn, NULL past the last */
const provider_def *provider_find(const char *advisor, const char **model);   /* NULL = a model on the Ollama server */
const char *provider_base_url(const provider_def *p);
const char *provider_chat_path(const provider_def *p);   /* "/chat/completions" or "/v1/messages" */
/* The headers of one call (the key). False when it cannot be made: not a hosted model, no key,
 * or a base URL that would send the key in the clear. The strings live in *keep. */
bool  provider_call_headers(const char *model, const char *hdrs[4], sbuf *keep, char *err, size_t n);
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
 * stopped it (Esc or Ctrl-C), otherwise err says why. */
char *provider_chat(const char *advisor, const model_info *mi, const provider_request *rq, chat_stats *st, bool *aborted, char *err, size_t n);
/* the pieces of it, exposed for tests */
char *provider_request_body(const char *advisor, const model_info *mi, const provider_request *rq);
char *provider_parse_reply(const char *advisor, const char *json, int status, chat_stats *st, char *err, size_t n);
void  provider_parse_model(const char *advisor, const char *json, model_info *mi);
cJSON *provider_parse_models(const char *json);                    /* a model list as [{"id","name"}], NULL if it is not one */
cJSON *provider_list_models(const provider_def *p, int wait_ms, char *err, size_t n);   /* what the key can use, asked silently; NULL without a key or an answer, and err says which */
bool  provider_fallbacks(const char *advisor);   /* an Anthropic model with server-side refusal fallbacks (Opus 5, Fable 5.x) */
/* A turn of the model that does the work, when that model is Anthropic. `messages` and `tools`
 * are the conversation ollama_chat() speaks; the body is the Messages request (malloc'd).
 * `effort` is a resolved level ("high") or NULL, `thinking` asks for it, temperature < 0 leaves
 * it out. The reply is the assistant message ollama_chat() would have built — text in `content`,
 * calls in `tool_calls`, and the raw blocks in `provider_content` so the next turn can send the
 * thinking back. NULL + err on a failed request; a refusal is a message, not a failure. */
char  *provider_turn_body(const char *model, const model_info *mi, cJSON *messages, cJSON *tools, int max_tokens, const char *effort, bool thinking, double temperature, bool stream);
cJSON *provider_turn_reply(const char *model, const char *json, int status, chat_stats *st, char *err, size_t n);
char  *provider_turn_request(cJSON *messages, cJSON *tools);   /* that body for the working model, from ollama_call / g_cfg, not streamed */
cJSON *provider_turn(cJSON *messages, cJSON *tools, chat_stats *stats, bool *aborted);   /* streamed; same contract as ollama_chat() */

#endif
