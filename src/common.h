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
int   mkdir_p(const char *path);
char *expand_home(const char *path);   /* "~/x" -> "/home/u/x", malloc'd */
int   is_dir(const char *path);
int   is_file(const char *path);

/* ---------- global config ---------- */
typedef struct {
    char *host;          /* e.g. http://127.0.0.1:11434 */
    char *model;
    char *system_prompt; /* extra system prompt from CLI/config */
    int   num_ctx;       /* 0 = leave to server default */
    int   draft;         /* draft_num_predict: speculative/MTP draft tokens per step; -1 = model default, 0 = off */
    double temperature;  /* <0 = unset */
    int   think;         /* -1 auto (server default on the first call of a request, off for tool rounds), 0 off, 1 on for every call */
    char *think_level;   /* "low"|"medium"|"high" for models with thinking levels (gpt-oss); NULL = plain on/off */
    bool  show_thinking; /* print thinking tokens */
    int   mode;          /* permission mode, see MODE_* */
    bool  no_tools;      /* don't send tools at all */
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
extern int (*http_interrupt_check)(void);   /* called when that fd is readable; nonzero = abort */
extern http_idle_cb http_idle;
extern void *http_idle_ud;

/* Performs request. If line_cb != NULL body is delivered line by line to it,
 * otherwise appended to `out` (may be NULL to discard). Returns 0 ok, <0 error. */
int http_request(const char *base_url, const char *method, const char *path,
                 const char *body, sbuf *out, http_line_cb line_cb, void *ud,
                 http_result *res);

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

/* Per-call overrides for ollama_chat(); set before a call and reset with ollama_call_reset()
 * (like ollama_quiet). Callers that don't touch them get the session defaults. */
typedef struct {
    int         think;        /* -1 = use g_cfg.think, 0 = force off (only sent if the model can think), 1 = on */
    int         num_predict;  /* >0 = cap on generated tokens */
    const char *busy;         /* status-bar label while generating (default "generating") */
} ollama_call_opts;
extern ollama_call_opts ollama_call;
void ollama_call_reset(void);
extern bool g_model_think;  /* current model advertises the "thinking" capability (set by main.c) */

/* Streams a chat completion. `messages` is a cJSON array (borrowed).
 * On success returns a new cJSON assistant message object (caller owns).
 * On error/abort returns NULL and sets *aborted / prints error. */
cJSON *ollama_chat(cJSON *messages, cJSON *tools, chat_stats *stats, bool *aborted);
extern bool ollama_quiet;   /* when set, ollama_chat() does not print the streamed reply (background calls) */
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

#endif
