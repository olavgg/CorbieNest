/* Terminal handling: raw mode, key decoding, a small multi-line line editor
 * (linenoise-style, UTF-8 aware) with history + slash-command completion,
 * an interactive list picker, and a streaming markdown-ish printer. */
#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_orig;
static bool g_have_orig = false, g_raw = false;
bool g_ui_nc = false;   /* the notcurses frontend is the screen (term_nc.c): every term_* below that differs asks it */
bool term_confirm_quiet = false;   /* see common.h */
static int vis_width(const char *s);   /* display width of a UTF-8 string, ignoring SGR escapes */
static volatile sig_atomic_t g_winch = 0;
static void sb_hook_stdout(void);       /* scrollback capture (see "scrollback" below) */
static void sb_pause(bool on);
static bool g_sb_pause = false;         /* ... is off: what is written is not conversation output */
static void sb_note(const char *text);
static size_t u8_next(const char *s, size_t pos, size_t len);
static size_t vis_offset(const char *s, size_t len, int col);
static int vis_width_n(const char *s, size_t len);

static void on_winch(int s) { (void)s; g_winch = 1; }

/* ---------- trace (CORBIENEST_TRACE=FILE) ----------
 * A drawing problem that shows in one terminal and not in another cannot be found by reading
 * the code, and "it looks wrong when I scroll" is nothing to go by. With CORBIENEST_TRACE set,
 * everything written to the terminal and everything read from it goes to that file as it
 * happens, with the time and the layout it was drawn for: tools/trace_replay.py plays the file
 * back into any terminal of the same size, and what the keyboard and the mouse really sent can
 * be read off it. One record per line, "<ms since the start> <kind> <payload>":
 *   O  bytes written to the terminal
 *   H  conversation output held back while scrolled back (recorded, not painted)
 *   I  bytes read from the terminal: keys, mouse reports
 *   L  the layout changed: the size, the scroll region, the rows of the chrome
 *   V  the scrollback viewer moved or was left          B  the status bar's activity label
 * Bytes stand for themselves, except control characters and the backslash: \e \n \r \t \\ \xHH.
 * The file holds the conversation and all that was typed, so it is created readable by its owner
 * alone, and it is theirs to pass on or not. */
static FILE *g_trace = NULL;
static const char *g_trace_path = NULL;
static struct timeval g_trace_t0;

static void trace_stamp(char kind) {
    struct timeval t; gettimeofday(&t, NULL);
    fprintf(g_trace, "%.3f %c ", (double)(t.tv_sec - g_trace_t0.tv_sec) * 1e3 + (double)(t.tv_usec - g_trace_t0.tv_usec) / 1e3, kind);
}
static void trace_bytes(char kind, const void *data, size_t n) {
    if (!g_trace) return;
    trace_stamp(kind);
    for (const unsigned char *p = data, *e = p + n; p < e; p++) {
        if (*p == 27) fputs("\\e", g_trace);
        else if (*p == '\n') fputs("\\n", g_trace);
        else if (*p == '\r') fputs("\\r", g_trace);
        else if (*p == '\t') fputs("\\t", g_trace);
        else if (*p == '\\') fputs("\\\\", g_trace);
        else if (*p < 32 || *p == 127) fprintf(g_trace, "\\x%02x", *p);
        else fputc(*p, g_trace);
    }
    fputc('\n', g_trace); fflush(g_trace);   /* (as it happens: the trace of a hang or a crash ends where that did) */
}
static void trace_note(char kind, const char *fmt, ...) {
    if (!g_trace) return;
    trace_stamp(kind);
    va_list ap; va_start(ap, fmt); vfprintf(g_trace, fmt, ap); va_end(ap);
    fputc('\n', g_trace); fflush(g_trace);
}
static void trace_open(void) {
    const char *path = getenv("CORBIENEST_TRACE");
    if (!path || !*path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);   /* (not handed down to the commands the tools run) */
    if (fd < 0 || !(g_trace = fdopen(fd, "w"))) {
        fprintf(stderr, "corbienest: cannot write the trace to %s: %s\n", path, strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    g_trace_path = path;
    gettimeofday(&g_trace_t0, NULL);
    int rows, cols; term_size(&rows, &cols);
    const char *term = getenv("TERM");
    trace_note('#', "corbienest %s trace 1 · TERM=%s · %d rows x %d columns", CORBIE_VERSION, term ? term : "", rows, cols);
}
const char *term_trace_path(void) { return g_trace_path; }

/* read() from the terminal: every place that takes input goes through here, so the trace has it */
static ssize_t in_read(void *buf, size_t n) {
    ssize_t k = read(STDIN_FILENO, buf, n);
    if (k > 0) trace_bytes('I', buf, (size_t)k);
    return k;
}

void term_init(void) {
    trace_open();
    if (g_ui_nc) { atexit(term_restore); return; }   /* notcurses sets up its own signals and terminal */
    if (tcgetattr(STDIN_FILENO, &g_orig) == 0) g_have_orig = true;
    signal(SIGWINCH, on_winch);
    atexit(term_restore);
}

void term_raw(bool on) {
    if (g_ui_nc || !g_have_orig) return;   /* notcurses keeps the terminal in its own mode */
    if (on && !g_raw) {
        struct termios t = g_orig;
        t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        /* keep OPOST/ONLCR so "\n" still becomes CRLF */
        t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
        g_raw = true;
    } else if (!on && g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
        g_raw = false;
    }
}

void term_restore(void) {
    if (g_ui_nc) { nc_stop(); return; }
    term_fullscreen(false);
    if (g_raw) {
        fputs("\x1b[?2004l" C_RESET, stdout);   /* disable bracketed paste */
        fflush(stdout);
        term_raw(false);
    }
}

void term_size(int *rows, int *cols) {
    if (g_ui_nc && nc_size(rows, cols)) return;
    struct winsize ws;
    int r = 24, c = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) { if (ws.ws_row > 0) r = ws.ws_row; if (ws.ws_col > 0) c = ws.ws_col; }
    if (rows) *rows = r;
    if (cols) *cols = c;
}
int term_width(void) { int c; term_size(NULL, &c); return c; }

/* ---------- key input ---------- */
enum {
    K_NONE = 0, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL,
    K_PGUP, K_PGDN, K_ALT_ENTER, K_ESC, K_PASTE_START, K_PASTE_END, K_ALT_B, K_ALT_F, K_ALT_BS,
    K_CTRL_LEFT, K_CTRL_RIGHT, K_SHIFT_TAB,
    K_WHEEL_UP, K_WHEEL_DOWN,   /* the mouse wheel, over the conversation */
    K_BANNER_X,                 /* the close button of the banner's modal: a click on it */
    K_MOUSE,                    /* any other mouse report: nothing acts on it */
    K_MOUSE_X10                 /* the old three-byte report: its bytes are still to come */
};

/* ---------- full screen + input field + status bar ---------- */
/* In full-screen mode the app owns the alternate screen. The bottom rows are the app's
 * chrome — the input field (a rule, the prompt, a rule) and the status bar on the last
 * row — and the rows above them form the scrolling region for the conversation. Every
 * redraw that uses "\x1b[J" (erase to end of screen) also wipes the chrome, so the
 * editor / menus append it again after each redraw (see chrome_append). */
static bool g_fs = false;
static int  g_fs_rows = 0, g_fs_cols = 0;   /* size the scroll region was set up for */
static int  g_fs_reserved = 0;              /* ... and how many bottom rows it left out */
static int  g_conv_row = 1, g_conv_col = 1; /* where conversation output continues while the editor owns the cursor */
static long g_live_out = 0;                 /* output tokens of the generation in flight */
static struct timeval g_live_t;             /* last live redraw (throttle) */
static const char *g_busy = NULL;           /* activity label shown (animated) in the bar; NULL = idle */
static int  g_busy_frame = 0;
static struct timeval g_busy_t;             /* last spinner advance (throttle) */
static int g_queue_n;                       /* messages queued while busy (see term_queue_*) */
static int  g_view_top = -1;                /* scrolled back: the first row shown (see "scrolling back"); -1 = the live view */
static bool g_view_stale = false;           /* ... and its window has to be painted again (the geometry changed) */
static char *g_ov_text = NULL;              /* the overlay's text (term_overlay_show): a centred window over the conversation; NULL = none */
/* the banner's modal (term_banner_show): the box's lines, drawn as they are, with the close button at (g_bn_xr, g_bn_xc) */
static bool g_banner_on = false;
static sbuf *g_bn = NULL;
static int g_bn_n = 0, g_bn_bw = 0, g_bn_xr = -1, g_bn_xc0 = -1, g_bn_xc1 = -1;
static char g_view_hint[200];               /* ... and what the line under that window says meanwhile */
static bool field_shown(void);              /* the terminal is tall enough for the input field (else only the bar is kept) */

static char *ta_pending_text(void);         /* text typed while busy, not yet submitted (malloc'd or NULL) */

void fmt_tokens(long n, char *out, size_t sz) {
    if (n < 1000) snprintf(out, sz, "%ld", n);
    else if (n < 10000) snprintf(out, sz, "%.1fk", n / 1000.0);
    else if (n < 1000000) snprintf(out, sz, "%ldk", n / 1000);
    else snprintf(out, sz, "%.1fM", n / 1e6);
}

/* Appends the status bar text, at most `cols - 1` visible columns wide. Segments are
 * dropped from the right when the terminal is narrow. */
/* the mode, as the field's rule shows it: an icon, its name, and how to switch it */
static void mode_text(sbuf *o, bool hint) {
    const char *icon, *col, *text;
    switch (g_cfg.mode) {
        case MODE_ACCEPT_EDITS: icon = "⏵⏵"; col = C_ORANGE; text = "accept edits on"; break;
        case MODE_PLAN:         icon = "⏸ "; col = C_CYAN;   text = "plan mode on";    break;
        case MODE_AUTO:         icon = "⏵⏵"; col = C_RED;    text = "auto mode on";    break;
        default:                icon = "⏵ "; col = C_DIM;    text = "manual mode";     break;
    }
    sb_printf(o, "%s%s %s" C_RESET, col, icon, text);
    if (hint) sb_printf(o, "\x1b[38;5;240m (shift+tab to cycle)" C_RESET);   /* a dark grey, more muted than the dim text beside it */
}
/* the mode for the field's lower rule: with its hint when the row has room for it, without when it has not */
int term_mode_text(sbuf *o, int cols) {
    for (int hint = 1; hint >= 0; hint--) {
        sbuf m; sb_init(&m); mode_text(&m, hint);
        int mw = vis_width(m.data);
        if (mw + 2 <= cols) { sb_append(o, m.data, m.len); sb_free(&m); return mw; }
        sb_free(&m);
    }
    return 0;
}

/* the model's name as the bar shows it: the provider's prefix and a ":latest" tag are dropped
 * ("anthropic:claude-haiku-5-5" is "claude-haiku-5-5"; "qwen3:latest" is "qwen3") */
static void model_short(char *out, size_t n, const char *m) {
    const char *p = m;
    if (strchr(m, ':') && provider_find(m, NULL)) p = strchr(m, ':') + 1;
    snprintf(out, n, "%s", p);
    size_t l = strlen(out);
    if (l > 7 && !strcmp(out + l - 7, ":latest")) out[l - 7] = 0;
}

/* the folder (the path is cut from the left) and the git branch, in the muted rose, within maxw columns */
static void loc_text(sbuf *o, int maxw) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    char br[128] = "";
    char tail[160] = "";
    if (git_branch(cwd, br, sizeof br) == 1) snprintf(tail, sizeof tail, "  · branch %s", br);
    size_t w = maxw > 1 ? (size_t)maxw : 1, pl = strlen(cwd), tl = strlen(tail);
    if (tl > w) tail[w] = 0, tl = w;
    char shown[4200];
    if (pl + tl > w) {
        size_t keep = w > tl + 1 ? w - tl - 1 : 0;   /* one column for the … */
        if (keep > pl) keep = pl;
        const char *q = cwd + (pl - keep);
        while (*q && (*q & 0xC0) == 0x80) q++;   /* not in the middle of a character */
        snprintf(shown, sizeof shown, "…%s", q);
    } else snprintf(shown, sizeof shown, "%s", cwd);
    sb_printf(o, "%s%s%s" C_RESET, th_fg(g_theme->loc), shown, tail);   /* the location line: a muted rose, a tint not an accent */
}

static void bar_build(sbuf *o, int cols) {
    if (g_view_top >= 0 && !field_shown()) {   /* scrolled back on a terminal too short for a line that says so (view_note): the bar does */
        sbuf t; sb_init(&t);
        sb_puts(&t, g_view_hint);
        if (vis_width(t.data) > cols - 1) { t.len = vis_offset(t.data, t.len, cols - 2); t.data[t.len] = 0; sb_puts(&t, "…"); }
        sb_printf(o, C_DIM "%s" C_RESET, t.data); sb_free(&t); return;
    }
    char mshort[200];
    const char *model = "(no model)";
    if (g_cfg.model) { model_short(mshort, sizeof mshort, g_cfg.model); model = mshort; }
    char mbuf[384];   /* an effort that was set goes with the model's name: it is a large part of how fast it answers */
    const char *eff = g_cfg.model ? effort_resolve(&g_model_info, effort_get(g_cfg.model), NULL) : NULL;
    if (eff) { snprintf(mbuf, sizeof mbuf, "%s · %s", model, eff); model = mbuf; }
    char tin[32], tout[32], ttot[32];
    long out_now = g_session.eval_tokens + g_live_out;
    /* the input the model read is the total: the part it had from the prompt cache is shown on its own (⟳), so ↑ is what was billed as input */
    long cached = g_session.cache_read > 0 ? g_session.cache_read : 0;
    fmt_tokens(g_session.prompt_tokens - cached, tin, sizeof tin);
    fmt_tokens(out_now, tout, sizeof tout);
    fmt_tokens(g_session.prompt_tokens + out_now, ttot, sizeof ttot);
    char ctx[32] = "";
    if (g_cfg.num_ctx > 0 && g_session.last_prompt_tokens > 0)
        snprintf(ctx, sizeof ctx, "ctx %d%%", (int)(100.0 * g_session.last_prompt_tokens / g_cfg.num_ctx));
    /* segments in priority order: model, tokens, ctx; then the queue. The folder and branch are at the right end. */
    char seg_tok[96], seg_tok_long[192];
    snprintf(seg_tok, sizeof seg_tok, "%s tokens", ttot);
    if (cached) { char tc[32]; fmt_tokens(cached, tc, sizeof tc); snprintf(seg_tok_long, sizeof seg_tok_long, "%s tokens (↑%s ⟳ %s ↓%s)", ttot, tin, tc, tout); }
    else snprintf(seg_tok_long, sizeof seg_tok_long, "%s tokens (↑%s ↓%s)", ttot, tin, tout);
    #define SEP_W 3   /* " │ " */
    /* the folder and branch take the right end; the segments have what is left */
    sbuf loc; sb_init(&loc);
    loc_text(&loc, (cols - 1) * 2 / 3);   /* two thirds of the row at most: the branch stays whole where it can */
    int lw = vis_width(loc.data ? loc.data : "");
    int room = cols - 1 - (lw ? lw + 1 : 0);
    int used = 0;
    sb_puts(o, " "); used = 1;
    /* model (truncate if needed) */
    {
        int mw = vis_width(model), avail = room - used - SEP_W;
        if (avail >= 6) {
            sb_puts(o, " ");
            if (mw <= avail) { sb_puts(o, model); used += 1 + mw; }
            else {   /* keep the tail, e.g. ":7b" is more useful than the registry prefix */
                int cut = mw - (avail - 1);
                const char *p = model; while (cut > 0 && *p) { if (((unsigned char)p[1] & 0xC0) != 0x80) cut--; p++; }
                while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++;
                sb_printf(o, "…%s", p); used += 1 + avail;
            }
        }
    }
    /* tokens */
    {
        int lw2 = vis_width(seg_tok_long), sw = vis_width(seg_tok), avail = room - used - SEP_W;
        const char *pick = lw2 <= avail ? seg_tok_long : sw <= avail ? seg_tok : NULL;
        if (pick) {
            sb_puts(o, C_DIM " │ " C_RESET);
            if (g_live_out) sb_printf(o, C_ORANGE "%s" C_RESET, pick); else sb_puts(o, pick);
            used += SEP_W + vis_width(pick);
        }
    }
    if (ctx[0]) {
        int cw = vis_width(ctx), avail = room - used - SEP_W;
        if (cw <= avail) {
            int pct = atoi(ctx + 4);
            sb_puts(o, C_DIM " │ " C_RESET);
            sb_printf(o, "%s%s" C_RESET, pct >= 85 ? C_YELLOW : "", ctx);
            used += SEP_W + cw;
        }
    }
    /* queued messages (typed + Enter while the model was busy) */
    if (g_queue_n) {
        char q[32]; snprintf(q, sizeof q, "%d queued", g_queue_n);
        int qw = vis_width(q), avail = room - used - SEP_W;
        if (qw <= avail) { sb_puts(o, C_DIM " │ " C_RESET); sb_printf(o, C_ORANGE "%s" C_RESET, q); used += SEP_W + qw; }
    }
    if (lw) {
        for (int i = used; i < cols - 1 - lw; i++) sb_putc(o, ' ');
        sb_append(o, loc.data, loc.len);
    }
    sb_free(&loc);
    #undef SEP_W
}

/* ---------- the input field ----------
 * The bottom of the screen always shows the prompt, framed by a rule above and below,
 * with the status bar underneath — so it is visible that input is accepted even while
 * the model works (Claude Code does the same):
 *
 *     ──────────────────────────────────────────
 *     ❯ what you are typing
 *     ──────────────────────────────────────────
 *      ⏵  manual mode │ qwen2.5-coder:7b │ 4.3k tokens
 *
 * The editor (readline_impl) draws into it and owns the cursor there (g_field_focus);
 * while the model works the field shows the type-ahead instead, so keystrokes are
 * visible before Enter turns them into a queued message. The field grows with the text
 * up to FIELD_MAX_ROWS rows (then it scrolls, keeping the cursor row visible), and the
 * scroll region shrinks to match (see layout_sync).
 * All of this is drawn with absolute cursor moves inside sb_pause(): it is not
 * conversation content, and the DECSC slot belongs to the editor (see conv_save). */
#define FIELD_MAX_ROWS 10
static const char *FIELD_PROMPT = C_BOLD C_ORANGE "❯ " C_RESET;
static const char *g_field_prompt = NULL;   /* NULL = FIELD_PROMPT (the editor overrides it for Ctrl-R) */
static const char *field_prompt(void) { return g_field_prompt ? g_field_prompt : FIELD_PROMPT; }
static int  g_field_rows = 1;          /* input rows the field currently shows */
static bool g_field_focus = false;     /* the editor owns the terminal cursor */
/* The cursor while a turn runs rests in the input field, on the type-ahead's caret, so the user sees where
 * typing goes. Conversation output is written at the transcript's end, so the cursor is moved there first
 * (sb_write_fn) and parked back afterwards. g_away: the cursor is not known to be at the transcript's end.
 * g_no_park: a question or menu is up and owns the cursor. */
static bool g_away = false;
static int  g_no_park = 0;
static int  g_ta_caret_row = -1, g_ta_caret_col = 0;   /* the type-ahead's caret, 1-based, as field_draw put it */
static bool want_box(void);
static void transcript_move(sbuf *o, bool flush);
static void box_move(sbuf *o);
static void chrome_park(sbuf *o);
static const char *g_field_text = "";  /* text shown in the field (the editor's buffer) */
static size_t g_field_len = 0, g_field_cur = 0;
static int  g_field_view = 0;          /* first visible wrapped row, when the text is taller */

/* Suggestions while a "/command" or an "@file" is being typed (see "suggestions" below). They
 * are chrome too: their rows sit between the field's lower rule and the status bar. */
#define SUG_MAX_ROWS 8
static term_sug *g_sug = NULL;         /* what term_suggest offered for the word at the cursor */
static int    g_sug_n = 0;
static int    g_sug_sel = -1;          /* the highlighted one; -1 = none, the list is only a preview */
static int    g_sug_top = 0;           /* first one shown, when there are more than fit */
static size_t g_sug_from = 0;          /* where the word they would replace starts */
static int    g_sug_rows = 0;          /* rows of the chrome the list holds */
/* the agents strip: the running sub-agents, one row each, between the field and the status bar (main.c
 * supplies the lines through term_agents_strip). g_strip_sel is the row the keys are on (-1: the field). */
#define STRIP_MAX 4
static sbuf g_strip;
static int  g_strip_rows = 0, g_strip_sel = -1;
int  (*term_agents_strip)(sbuf *o, int cols, int max) = NULL;
void (*term_agents_open)(int idx) = NULL;
static char  *g_ta_sug_for = NULL;     /* the list is the type-ahead's (the model is working): the text it is for */
static void sug_draw(sbuf *o, int row);
static void sug_clear(void);
static void put_clipped(sbuf *o, const char *s, int w, bool keep_tail);
static void ta_sug_sync(void);
static bool ta_sug_key(int k);

/* rows the app owns at the bottom: the two rules, the input rows, the suggestions and the
 * status bar — and, while the conversation is scrolled back, a line above them that says so
 * (view_note). On a terminal too short to spare them, only the bar is kept. */
#define FIELD_MIN_ROWS 6
static bool field_shown(void) { return g_fs && g_fs_rows >= FIELD_MIN_ROWS; }
static int fs_reserved_for(int rows) { return g_fs && rows >= FIELD_MIN_ROWS ? g_field_rows + 3 + g_sug_rows + g_strip_rows + (g_view_top >= 0) : 1; }
static int fs_reserved(void) { return fs_reserved_for(g_fs_rows); }
/* last row of the scrolling region (the conversation) */
static int fs_region(void) { int r = g_fs_rows - fs_reserved(); return r < 1 ? 1 : r; }

/* The field's text wrapped at `width` columns, starting at column `col0`, at word boundaries: a row breaks
 * after its last space, and a word longer than a row is cut. Walks the text once: the (row, col) reached
 * at byte `target`, and the byte where row `want` starts (-1 when `want` is not reached: start_out is then
 * len). Shared by the ANSI field, the editor's cursor arithmetic and the notcurses field (term_wrap). */
void term_wrap(const char *s, size_t len, int width, int col0, size_t target, int want, size_t *start_out, int *row_out, int *col_out) {
    if (width < 1) width = 1;
    int r = 0, c = col0;
    if (c >= width) { r = c / width; c %= width; }
    size_t rowstart = 0, brk = (size_t)-1;   /* brk: byte after the last space of this row (a break point) */
    int brk_c = 0;                           /* the column that break point is at */
    size_t start = len; bool have_start = false;
    if (want == 0) { start = 0; have_start = true; }
    size_t i = 0;
    while (i <= len) {
        if (i == target || i == len) break;
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') {
            r++; c = 0; i++; rowstart = i; brk = (size_t)-1;
            if (!have_start && r == want) { start = i; have_start = true; }
            continue;
        }
        size_t nx = u8_next(s, i, len);
        if (c + 1 > width) {                 /* the character does not fit: break at the last space, or cut here */
            if (brk != (size_t)-1 && brk > rowstart) {
                r++; c = c - brk_c; rowstart = brk; brk = (size_t)-1;
                if (!have_start && r == want) { start = rowstart; have_start = true; }
            } else {
                r++; c = 0; rowstart = i;
                if (!have_start && r == want) { start = i; have_start = true; }
            }
        }
        c++;
        if (ch == ' ') { brk = nx; brk_c = c; }
        i = nx;
    }
    if (c >= width) { /* the cursor at the very edge of a full row stays on it */ }
    *row_out = r; *col_out = c;
    if (start_out) *start_out = have_start ? start : len;
}
static void wrap_pos(const char *s, size_t len, int width, int col0, int *row, int *col) {
    term_wrap(s, len, width, col0, len, -1, NULL, row, col);
}

/* byte offset where wrapped row `want` of the field text starts */
static size_t field_row_start(int width, int col0, int want) {
    if (want <= 0) return 0;
    size_t st = g_field_len; int r, c;
    term_wrap(g_field_text, g_field_len, width, col0, g_field_len, want, &st, &r, &c);
    return st;
}

/* How many rows the current field text needs, and where its cursor sits. */
/* the field is a box: "│ " before its text and " │" after it, so the text starts at column FIELD_X0 + 1
 * and is field_width() columns wide */
#define FIELD_X0 2
static int field_width(void) { int w = g_fs_cols - 2 * FIELD_X0; return w > 2 ? w : 2; }
static void field_metrics(int *need, int *crow, int *ccol) {
    int width = field_width();
    int plen = vis_width(field_prompt());
    int erow, ecol;
    wrap_pos(g_field_text, g_field_len, width, plen, &erow, &ecol);
    wrap_pos(g_field_text, g_field_cur, width, plen, crow, ccol);
    (void)ecol;
    *need = erow + 1;
}

/* Recompute how tall the field and the suggestions under it have to be; returns true when
 * that changed (the caller re-applies the scroll region through layout_sync). */
static bool field_sync_rows(void) {
    int need, crow, ccol;
    field_metrics(&need, &crow, &ccol);
    int max = FIELD_MAX_ROWS;
    if (max > g_fs_rows - FIELD_MIN_ROWS) max = g_fs_rows - FIELD_MIN_ROWS;   /* always leave room to read */
    if (max < 1) max = 1;
    int rows = need < max ? need : max;
    if (crow < g_field_view) g_field_view = crow;
    if (crow >= g_field_view + rows) g_field_view = crow - rows + 1;
    if (g_field_view > need - rows) g_field_view = need - rows;
    if (g_field_view < 0) g_field_view = 0;
    /* The suggestions get what the terminal can spare beyond that. While the list is up it does
     * not shrink again: the field would jump with every letter that narrows it down. */
    int sug = 0;
    if (g_sug_n > 0) {
        sug = (g_sug_n < SUG_MAX_ROWS ? g_sug_n : SUG_MAX_ROWS) + 1;   /* + the row that says which keys work */
        if (sug < g_sug_rows) sug = g_sug_rows;
        if (sug > g_fs_rows - FIELD_MIN_ROWS - rows) sug = g_fs_rows - FIELD_MIN_ROWS - rows;
        if (sug < 2) sug = 0;
    }
    if (rows == g_field_rows && sug == g_sug_rows) return false;
    g_field_rows = rows; g_sug_rows = sug;
    return true;
}

/* A paste of more than a few lines is shown in the field as a token, "[Pasted text #K +N lines]",
 * and the text is kept here until the prompt is submitted (paste_expand() puts it back). */
#define PASTE_MAX 16
typedef struct { char *text; int lines; } paste_rec;
static paste_rec g_paste[PASTE_MAX]; static int g_paste_n = 0;   /* this prompt's pastes, K = index + 1 */
static void paste_reset(void) { for (int i = 0; i < g_paste_n; i++) free(g_paste[i].text); g_paste_n = 0; }

static bool g_ready = false;   /* the top-level prompt is waiting: the field's rules are green and say [ready] */

/* The line the conversation gets under it while it is scrolled back: where the window is and how
 * to get back. It is a row of its own, which the region gives up for as long as one reads. The
 * bar is not the place: mode, tokens and what the model is doing are looked for there, scrolled
 * back or not, and a bar that turns into something else looks like a drawing error. */
static void view_note(sbuf *o, int row) {
    sb_printf(o, "\x1b[%d;1H" C_DIM " ", row);
    put_clipped(o, g_view_hint, g_fs_cols - 2, false);
    sb_puts(o, C_RESET "\x1b[K");
}

static int field_top(void) { return g_fs_rows - g_field_rows - 2 - g_sug_rows - g_strip_rows; }   /* row of the upper rule (the bar is the last row, below the field's lower rule and the suggestions) */

/* Append text[from, from + n) with the "@path" mentions that name something that is there in
 * colour: a mention that took is one the message will carry, and one that did not is a typo
 * seen before it is sent. The escapes take no columns, so the wrapping worked out for the plain
 * text still holds. A "/command" or a "!line" has no mentions: those are not expanded. */
#define MENTION_MAX 16
static int field_mentions(const char *text, size_t len, size_t (*sp)[2]) {
    return len && text[0] != '/' && text[0] != '!' ? mention_spans(text, len, sp, MENTION_MAX) : 0;
}
/* what is typed in the field: a light yellow on a darker ground (notcurses uses the same colours) */
/* the typed text's ground and colour, from the theme */
static void put_field_on(sbuf *o) { sb_puts(o, th_bg(g_theme->field_bg)); sb_puts(o, th_fg(g_theme->field_text)); }
/* kind[i] 1 = a paste token (reverse video), anything else a mention (cyan); kind may be NULL */
static void put_mentions(sbuf *o, const char *text, size_t from, size_t n, size_t (*sp)[2], const int *kind, int ns) {
    size_t at = from, end = from + n;
    for (int i = 0; i < ns && at < end; i++) {
        if (sp[i][1] <= at) continue;
        if (sp[i][0] >= end) break;
        size_t a = sp[i][0] > at ? sp[i][0] : at, b = sp[i][1] < end ? sp[i][1] : end;
        sb_append(o, text + at, a - at);
        if (kind && kind[i] == 1) { sb_puts(o, "\x1b[7m"); sb_append(o, text + a, b - a); sb_puts(o, "\x1b[27m"); }
        else { sb_puts(o, C_CYAN); sb_append(o, text + a, b - a); sb_puts(o, th_fg(g_theme->field_text)); }
        at = b;
    }
    if (at < end) sb_append(o, text + at, end - at);
}

/* Draw the field with absolute moves. `typed` is the type-ahead shown when the editor
 * is not the one holding the field (i.e. the model is working). A field that shrinks hands
 * its upper rows back to the conversation; layout_sync() paints those again, so nothing the
 * field drew there stays behind. */
/* the top or bottom edge of the field: rounded corners, green while the prompt waits */
static void field_edge(sbuf *o, int row, bool top) {
    sb_printf(o, "\x1b[%d;1H%s%s", row, th_fg(g_ready ? g_theme->edge_ready : g_theme->edge_idle), top ? "╭" : "╰");
    for (int i = 0; i < g_fs_cols - 2; i++) sb_puts(o, "─");
    sb_puts(o, top ? "╮" : "╯");
    sb_puts(o, C_RESET);
}
/* the strip's lines, from the running sub-agents; the selected row is highlighted */
static void strip_refresh(void) {
    sb_clear(&g_strip);
    int rows = (g_fs || g_ui_nc) && term_agents_strip ? term_agents_strip(&g_strip, term_width(), STRIP_MAX) : 0;
    g_strip_rows = rows;
    if (g_strip_sel >= rows) g_strip_sel = rows - 1;
    if (rows == 0) g_strip_sel = -1;
}
static void strip_draw(sbuf *o, int top) {
    if (!g_strip_rows) return;
    const char *p = g_strip.data ? g_strip.data : "";
    int cols = g_fs_cols > 1 ? g_fs_cols : 1;
    for (int i = 0; i < g_strip_rows; i++) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        sbuf line; sb_init(&line);
        sb_append(&line, p, len);
        sb_printf(o, "\x1b[%d;1H", top + i);
        if (i == g_strip_sel) sb_puts(o, th_bg(g_theme->select));   /* the row the keys are on: a dark band, padded with spaces */
        sb_append(o, line.data ? line.data : "", line.len);
        for (int w = vis_width(line.data ? line.data : ""); w < cols; w++) sb_putc(o, ' ');
        sb_puts(o, C_RESET);
        sb_free(&line);
        p = e ? e + 1 : p + len;
    }
}
static void field_draw(sbuf *o, const char *typed) {
    if (!g_fs || g_fs_rows < FIELD_MIN_ROWS) return;
    int top = field_top();
    g_ta_caret_row = top + 1;   /* where the caret is when nothing is typed: after the prompt (the typed branch moves it) */
    g_ta_caret_col = FIELD_X0 + (g_field_view ? 0 : vis_width(field_prompt())) + 1;
    int width = field_width();
    if (g_view_top >= 0) view_note(o, top - 1);
    field_edge(o, top, true);
    if (g_ready) {   /* [ready], upper right, two cells in from the edge (as the mode is on the lower rule) */
        int c = g_fs_cols - 7 - 1;
        if (c >= 1) sb_printf(o, "\x1b[%d;%dH%s[ready]" C_RESET, top, c, th_fg(g_theme->edge_ready));
    }
    for (int i = 0; i < g_field_rows; i++) sb_printf(o, "\x1b[%d;%dH%s\x1b[K" C_RESET, top + 1 + i, FIELD_X0, th_bg(g_theme->field_bg));
    sb_printf(o, "\x1b[%d;%dH", top + 1, FIELD_X0 + 1);
    if (!g_field_view) sb_puts(o, field_prompt());
    put_field_on(o);
    if (typed) {
        /* not the editor's text: one line, tail kept, with a block for the cursor */
        int avail = width - vis_width(field_prompt()) - 2;
        char *t = xstrdup(typed);
        for (char *p = t; *p; p++) if (*p == '\n') *p = ' ';
        const char *p = t;
        int tw = vis_width(t), mark = 0;
        if (avail > 1 && tw > avail) {
            int cut = tw - (avail - 1);
            while (cut > 0 && *p) { if (((unsigned char)p[1] & 0xC0) != 0x80) cut--; p++; }
            while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++;
            sb_puts(o, "…"); mark = 1;
        }
        size_t sp[MENTION_MAX][2]; int ns = field_mentions(t, strlen(t), sp);
        put_mentions(o, t, (size_t)(p - t), strlen(p), sp, NULL, ns); sb_puts(o, C_DIM "▏" C_RESET); put_field_on(o);
        /* the caret is the ▏: after the prompt, the … if any, and the tail */
        g_ta_caret_col = FIELD_X0 + (g_field_view ? 0 : vis_width(field_prompt())) + mark + vis_width(p) + 1;
        free(t);
    } else if (g_field_len) {
        int plen = vis_width(field_prompt());
        size_t from = field_row_start(width, plen, g_field_view);
        /* mentions and paste tokens, in one list in the order they stand */
        size_t ms[MENTION_MAX][2], ps[PASTE_MAX][2], sp[MENTION_MAX + PASTE_MAX][2]; int kind[MENTION_MAX + PASTE_MAX];
        int nm = field_mentions(g_field_text, g_field_len, ms);
        size_t np = paste_tokens(g_field_text, g_field_len, ps, NULL, NULL, PASTE_MAX);
        int ns = 0;
        for (int i = 0, j = 0; i < nm || j < (int)np; ) {
            if (j >= (int)np || (i < nm && ms[i][0] <= ps[j][0])) { sp[ns][0] = ms[i][0]; sp[ns][1] = ms[i][1]; kind[ns++] = 0; i++; }
            else { sp[ns][0] = ps[j][0]; sp[ns][1] = ps[j][1]; kind[ns++] = 1; j++; }
        }
        for (int i = 0; i < g_field_rows; i++) {
            size_t to = field_row_start(width, plen, g_field_view + i + 1);
            if (from >= g_field_len && i) break;
            if (to > g_field_len) to = g_field_len;
            sb_printf(o, "\x1b[%d;%dH", top + 1 + i, FIELD_X0 + 1);
            if (i == 0 && g_field_view == 0) sb_puts(o, field_prompt());
            put_field_on(o);
            size_t n = to > from ? to - from : 0;
            while (n && g_field_text[from + n - 1] == '\n') n--;
            put_mentions(o, g_field_text, from, n, sp, kind, ns);
            sb_puts(o, "\x1b[K" C_RESET);
            from = to;
        }
    }
    /* the side borders of the text rows (after the text, which is cut short of them) */
    for (int i = 0; i < g_field_rows; i++) {
        int r = top + 1 + i;
        sb_printf(o, "\x1b[%d;1H%s│" C_RESET, r, th_fg(g_ready ? g_theme->edge_ready : g_theme->edge_idle));
        sb_printf(o, "\x1b[%d;%dH%s│" C_RESET, r, g_fs_cols, th_fg(g_ready ? g_theme->edge_ready : g_theme->edge_idle));
    }
    field_edge(o, top + g_field_rows + 1, false);
    {   /* the mode, on the lower rule: its last cell two columns in from the edge (the hint goes first when it does not fit) */
        sbuf m; sb_init(&m);
        int mw = term_mode_text(&m, g_fs_cols);
        int c = g_fs_cols - mw - 1;
        if (mw && c >= 1) { sb_printf(o, "\x1b[%d;%dH", top + g_field_rows + 1, c); sb_append(o, m.data, m.len); sb_puts(o, C_RESET); }
        sb_free(&m);
    }
    sug_draw(o, top + g_field_rows + 2);
}

/* Where conversation output continues on screen, without asking the terminal: the
 * scrollback model has produced N visual rows since the last clear and the region shows
 * their tail, so the cursor sits on row min(N, region) — at the bottom once it is full. */
typedef struct { int line; size_t off, len; } sb_row;
static sb_row *sb_rows(int width, int *count);
static int  g_sb_col;                 /* visual column of the scrollback cursor in its line */
static bool g_sb_active;              /* stdout is hooked (see sb_hook_stdout) */
/* where the transcript ends, as a row and column. flush: the stdio buffer goes first (the model only sees
 * what left it); the stdout hook passes false, since it is called from inside that flush. */
static int sb_rows_upto(int width, int need);
static void conv_pos_x(int region, int *row, int *col, bool flush) {
    int n = 1;
    if (flush) fflush(stdout);
    if (g_sb_active && g_fs_cols > 0) { int c = sb_rows_upto(g_fs_cols, region); if (c > 0) n = c; }
    *row = n > region ? region : n;
    *col = (g_fs_cols > 0 ? g_sb_col % g_fs_cols : 0) + 1;
}
static void conv_pos(int region, int *row, int *col) { conv_pos_x(region, row, col, true); }

/* Put the terminal cursor where the editor's cursor is inside the field. */
static void field_cursor(sbuf *o) {
    if (!g_fs || g_fs_rows < FIELD_MIN_ROWS) return;
    int need, crow, ccol;
    field_metrics(&need, &crow, &ccol);
    int first = field_top() + 1, row = first + (crow - g_field_view);
    if (row < first) row = first;
    if (row > first + g_field_rows - 1) row = first + g_field_rows - 1;
    sb_printf(o, "\x1b[%d;%dH", row, FIELD_X0 + ccol + 1);
    g_away = true;
}

/* Must run before any redraw. When the terminal was resized, or the chrome grew or shrank
 * (the input field, the suggestions under it), re-apply the scroll region for the new geometry
 * and paint the conversation into it again from the scrollback model (conv_repaint). What a
 * terminal makes of the old picture when its size changes differs from one to the next — cropped,
 * reflowed, a multiplexer's own idea of it — and asking where that left the cursor does not say
 * what is on the rows around it; the model is what the screen is meant to show anyway. Returns
 * true when it painted, i.e. the cursor is now where the transcript ends: a caller with something
 * transient drawn below that (a menu, the inline editor) starts over from there. None of this is
 * conversation content, so it is written with the scrollback capture paused. */
static void conv_repaint(sbuf *o);
static void chrome_invalidate(void);   /* forget what the chrome last sent: the screen may have changed under it */
static void chrome_send(sbuf *o);
static bool layout_sync(void) {
    if (!g_fs) return false;
    int rows, cols; term_size(&rows, &cols);
    int reserved = fs_reserved_for(rows);
    if (rows == g_fs_rows && cols == g_fs_cols && reserved == g_fs_reserved) return false;
    chrome_invalidate();   /* a resize: the screen is not what the chrome last sent */
    g_fs_rows = rows; g_fs_cols = cols; g_fs_reserved = reserved;
    trace_note('L', "rows=%d cols=%d region=1-%d chrome=%d field=%d suggestions=%d view=%d", rows, cols, fs_region(), reserved, g_field_rows, g_sug_rows, g_view_top);
    sbuf o; sb_init(&o);
    sb_printf(&o, "\x1b[1;%dr", fs_region());
    if (g_view_top >= 0) g_view_stale = true;   /* scrolled back: the window shown is the viewer's to paint */
    else conv_repaint(&o);
    bool was = g_sb_pause; sb_pause(true);
    fwrite(o.data, 1, o.len, stdout); fflush(stdout);
    sb_pause(was);
    sb_free(&o);
    return g_view_top < 0;
}

/* The location line, on the last row, right-aligned: the working directory as an absolute path, and the git branch it is on
 * when it is in a repository. Too long for the width, the path loses its start (…) and the branch stays. */
/* The two bottom rows share one background, flat (the notcurses UI draws the same colour). Each segment of
 * their text ends in a reset, which would drop it: sb_bar_row() puts it back after every reset, and pads the
 * row to its full width with spaces in it. (Not with an erase to the end of the line: tmux, for one, then
 * shows the row below without its background.) Truecolor: a terminal without it shows no background. */
static void sb_bar_row(sbuf *o, const char *text, int cols) {
    sb_puts(o, th_bg(g_theme->bar_a));
    for (const char *p = text; *p; ) {
        if (!strncmp(p, C_RESET, strlen(C_RESET))) { sb_puts(o, C_RESET); sb_puts(o, th_bg(g_theme->bar_a)); p += strlen(C_RESET); }
        else sb_putc(o, *p++);
    }
    for (int w = vis_width(text); w < cols; w++) sb_putc(o, ' ');
    sb_puts(o, C_RESET);
}

/* Draw the status bar on the last row (absolute; the caller preserves the cursor). On a terminal too short
 * for the field the bar is all there is. */
static void bar_draw(sbuf *o) {
    int bar_row = g_fs_rows >= 1 ? g_fs_rows : 1;
    sbuf t; sb_init(&t);
    sb_printf(o, "\x1b[%d;1H", bar_row);
    bar_build(&t, g_fs_cols);
    sb_bar_row(o, t.data ? t.data : "", g_fs_cols);
    sb_free(&t);
}

/* Append the whole chrome — input field + status bar — without disturbing the cursor
 * (DECSC/DECRC). For callers that erase to the end of the screen and redraw.
 * Callers run layout_sync() first, and draw with the capture paused. */
static void chrome_append(sbuf *o) {
    chrome_invalidate();   /* written in full here: the next refresh sends what differs from this */
    if (!g_fs) return;
    char *typed = g_busy && !g_field_focus ? ta_pending_text() : NULL;
    sb_puts(o, "\x1b" "7");
    field_draw(o, typed);
    bar_draw(o);
    sb_puts(o, "\x1b" "8");
    free(typed);
}

/* the chrome's text as the ANSI bar draws it, escapes included (term_nc.c parses it into cells) */
void term_status_text(sbuf *o, int cols) { bar_build(o, cols); }

static void view_paint(void);
void term_status_refresh(void) {
    if (g_ui_nc) { nc_render(); return; }
    if (!g_fs) return;
    layout_sync();
    ta_sug_sync();
    bool fchanged = field_sync_rows();   /* the field changed height: re-cut the region */
    int strip_before = g_strip_rows;
    strip_refresh();
    if (fchanged || strip_before != g_strip_rows) layout_sync();
    if (g_view_top >= 0 && g_view_stale) view_paint();
    char *typed = g_busy && !g_field_focus ? ta_pending_text() : NULL;
    sbuf o; sb_init(&o);
    if (!g_field_focus) sb_puts(&o, "\x1b" "7");
    field_draw(&o, typed);
    strip_draw(&o, field_top() + g_field_rows + 2 + g_sug_rows);
    bar_draw(&o);
    if (g_field_focus) field_cursor(&o); else { sb_puts(&o, "\x1b" "8"); chrome_park(&o); }
    chrome_send(&o);
    sb_free(&o);
    free(typed);
    gettimeofday(&g_live_t, NULL);
}

void term_status_live(long out_tokens) {
    g_live_out = out_tokens;
    if (g_ui_nc) { nc_render_soon(); return; }
    if (!g_fs) return;
    if (out_tokens == 0) return;   /* the caller refreshes once the final stats are in */
    struct timeval t; gettimeofday(&t, NULL);
    double dt = (double)(t.tv_sec - g_live_t.tv_sec) + (double)(t.tv_usec - g_live_t.tv_usec) / 1e6;
    if (dt >= 0.15 || dt < 0) term_status_refresh();
}

void term_busy(const char *label) {
    if (label != g_busy) trace_note('B', "%s", label ? label : "-");
    g_busy = label;
    if (label) g_busy_frame = 0;
    gettimeofday(&g_busy_t, NULL);
    term_status_refresh();
}

void term_busy_tick(void) {
    if (!g_busy || !(g_fs || g_ui_nc)) return;
    struct timeval t; gettimeofday(&t, NULL);
    double dt = (double)(t.tv_sec - g_busy_t.tv_sec) + (double)(t.tv_usec - g_busy_t.tv_usec) / 1e6;
    if (dt < 0.1 && dt >= 0) return;
    g_busy_t = t;
    g_busy_frame++;
    term_status_refresh();
}

/* The mouse is asked for (press/release reports, SGR coordinates) because of the wheel. On the
 * alternate screen a terminal that is not asked sends the wheel as ↑/↓ keys, which here walk the
 * prompt history instead of scrolling anything, and tmux with its own mouse on keeps the wheel
 * for its copy mode — of a screen that has no history. Asked, both hand the wheel over as what it
 * is (read_key: K_WHEEL_*), and the arrows stay the input field's. The price is the usual one
 * for a full-screen program: selecting text with the mouse needs Shift held down. */
#define MOUSE_ON  "\x1b[?1000h\x1b[?1006h"
#define MOUSE_OFF "\x1b[?1006l\x1b[?1000l"
/* DECSCUSR: a blinking block for the cursor the output and the editor share, and the terminal's own
 * style back on the way out (0 = the default) */
#define CURSOR_BLINK   "\x1b[?25h\x1b[1 q"
#define CURSOR_DEFAULT "\x1b[0 q"

void term_finish(void) {
    term_restore();
    g_ui_nc = false;   /* the program is ending: what is printed from here (the goodbye) goes to the terminal itself */
}

void term_fullscreen(bool on) {
    chrome_invalidate();
    if (g_ui_nc) {
        if (!on) { nc_stop(); return; }
        sb_hook_stdout();   /* the conversation is still captured: the pane is drawn from it */
        if (nc_start()) return;
        g_ui_nc = false;    /* could not start: the ANSI screen from here on (nc_start said why) */
    }
    if (on && !g_fs) {
        sb_hook_stdout();   /* start recording the conversation for PgUp */
        g_fs = true; g_fs_rows = g_fs_cols = g_fs_reserved = 0; g_live_out = 0;
        g_field_rows = 1; g_field_view = 0; g_conv_row = g_conv_col = 1;
        g_sug_rows = 0; g_view_top = -1;
        fputs("\x1b[?1049h\x1b[H\x1b[2J" MOUSE_ON CURSOR_BLINK, stdout);   /* alternate screen, cleared; a blinking cursor where the output goes on */
        term_status_refresh();                       /* also sets the scroll region */
    } else if (!on && g_fs) {
        g_fs = false; g_view_top = -1;
        fputs(C_RESET MOUSE_OFF CURSOR_DEFAULT "\x1b[r\x1b[?1049l", stdout);      /* reset margins, back to the main screen */
        fflush(stdout);
    }
}

void term_clear_screen(void) {
    if (g_ui_nc) { nc_clear(); return; }
    g_view_top = -1;               /* nothing left to be scrolled back in */
    fputs("\x1b[H\x1b[2J", stdout);
    g_conv_row = g_conv_col = 1;   /* the conversation starts over at the top of the region */
    term_status_refresh();
    fflush(stdout);
}

/* ---------- scrollback (PgUp / PgDn) ----------
 * The alternate screen has no scrollback of its own, so the app keeps one: everything the
 * conversation prints to stdout is teed (see sb_hook_stdout) into a list of logical lines,
 * with SGR colours kept and the few cursor movements we use (CR, cursor up/down/right,
 * erase line / erase below, DECSC…DECRC around the status bar) interpreted so the model
 * tracks what is on screen. Drawing that is not conversation content — the bottom chrome, the
 * menus, the viewer itself — runs with the capture paused (sb_pause); the field editor prints
 * its submitted line as ordinary output, the inline one notes it (sb_note). It is also what
 * tells the field editor where the transcript ended (conv_pos). PgUp at the prompt opens the viewer
 * (scroll_view): the region shows a window into the buffer, PgDn/End/Esc go back. */
#define SB_MAX_LINES 8000
static sbuf *g_sb = NULL;             /* logical lines */
static int   g_sb_n = 0, g_sb_cap = 0;
static int   g_sb_epoch = 0;      /* bumped whenever line numbers stop meaning what they did (lines dropped, cleared) */
/* Folds: a long result is one line that stays (the head, with a ▸) and a body that is in the
 * model but not on screen until the head is clicked. A body line is `hidden`: sb_rows() leaves
 * it out, so the region, the viewer and conv_pos() all see the transcript without it, and the
 * cursor movements step over it. */
enum { SB_PLAIN, SB_FOLD_HEAD, SB_FOLD_BODY };
typedef struct { int fold; unsigned char kind; bool hidden; } sb_meta;
static sb_meta *g_sb_meta = NULL;     /* one per line of g_sb */
static int   g_fold_seq = 0;          /* the last fold's number */
static int   g_fold_rec = 0;          /* the fold whose body is being written (term_fold_begin…end): recorded, not painted */
static int   g_click_frow = 0, g_click_fcol = 0;   /* a left click in the field below the conversation: its row and column (paste_click) */
static int   g_click_row = 0;         /* a left click on this row of the conversation is waiting to be acted on (fold_click) */
static bool sb_hidden(int line) { return line >= 0 && line < g_sb_n && g_sb_meta[line].hidden; }
/* what term_nc.c draws the conversation pane from: the scrollback model's lines, SGR escapes included */
int         term_sb_nlines(void) { return g_sb_n; }
int         term_sb_epoch(void) { return g_sb_epoch; }
bool        term_is_fullscreen(void) { return g_fs; }
const char *term_sb_line(int i) { return g_sb[i].data ? g_sb[i].data : ""; }
bool        term_sb_hidden(int i) { return sb_hidden(i); }
int         term_sb_col(void) { return g_sb_col; }
static int   g_sb_line = 0;           /* cursor: line index ... */
/* g_sb_col: visual column in that line (>= width means a wrapped row), declared with the field */
/* g_sb_pause (not conversation output: don't record) is declared at the top: the layout code uses it */
/* g_sb_active / g_sb_col are declared with the input field, which uses them */
static int   g_sb_decsc = 0;          /* inside DECSC…DECRC (status bar): ignore */
static char  g_sb_seq[48]; static int g_sb_seqn = -1;   /* escape sequence being collected (-1 = none) */
static bool  g_sb_cr = false;         /* a CR was seen; \r\n must not clear the line */
static char  g_sb_sgr[96]; static int g_sb_sgrn = 0;    /* colours set since the last reset: what output continues in */

static int sb_width(void) { return g_fs_cols > 0 ? g_fs_cols : term_width(); }

/* byte offset where the visible column `col` starts in s (escapes are zero width) */
static size_t vis_offset(const char *s, size_t len, int col) {
    int w = 0; size_t i = 0;
    while (i < len) {
        if (s[i] == 27) { while (i < len && s[i] != 'm') i++; if (i < len) i++; continue; }
        if (w >= col) break;
        i = u8_next(s, i, len); w++;
    }
    return i;
}
static int vis_width_n(const char *s, size_t len) {
    int w = 0;
    for (size_t i = 0; i < len; ) {
        if (s[i] == 27) { while (i < len && s[i] != 'm') i++; if (i < len) i++; continue; }
        if (((unsigned char)s[i] & 0xC0) != 0x80) w++;
        i++;
    }
    return w;
}
static void sb_ensure_line(void) {
    if (g_sb_n == 0) { g_sb_line = 0; g_sb_col = 0; }
    while (g_sb_line >= g_sb_n) {
        if (g_sb_n == g_sb_cap) {
            g_sb_cap = g_sb_cap ? g_sb_cap * 2 : 256;
            g_sb = xrealloc(g_sb, sizeof *g_sb * (size_t)g_sb_cap); g_sb_meta = xrealloc(g_sb_meta, sizeof *g_sb_meta * (size_t)g_sb_cap);
        }
        g_sb_meta[g_sb_n] = (sb_meta){ g_fold_rec, g_fold_rec ? SB_FOLD_BODY : SB_PLAIN, g_fold_rec != 0 };
        sb_init(&g_sb[g_sb_n]); sb_append(&g_sb[g_sb_n], "", 0); g_sb_n++;
    }
    if (g_sb_n > SB_MAX_LINES) {   /* drop the oldest quarter */
        int drop = SB_MAX_LINES / 4;
        for (int i = 0; i < drop; i++) sb_free(&g_sb[i]);
        memmove(g_sb, g_sb + drop, sizeof *g_sb * (size_t)(g_sb_n - drop));
        memmove(g_sb_meta, g_sb_meta + drop, sizeof *g_sb_meta * (size_t)(g_sb_n - drop));
        g_sb_n -= drop; g_sb_line -= drop; if (g_sb_line < 0) g_sb_line = 0;
        g_sb_epoch++;   /* the line numbers moved: a row map made before is not to be read */
    }
}
/* the cursor is about to write: cut the line at the cursor column (a redraw overwrites the rest) */
static void sb_cut_at_cursor(void) {
    sb_ensure_line();
    sbuf *l = &g_sb[g_sb_line];
    int w = vis_width_n(l->data, l->len);
    if (g_sb_col < w) { size_t off = vis_offset(l->data, l->len, g_sb_col); l->len = off; l->data[off] = 0; sb_puts(l, C_RESET); }
    else while (w < g_sb_col) { sb_putc(l, ' '); w++; }   /* cursor beyond the text: pad */
}
static void sb_drop_after(int line) {
    while (g_sb_n > line + 1) sb_free(&g_sb[--g_sb_n]);
}
static void sb_cursor_up(int n) {
    int width = sb_width(); if (width < 1) width = 1;
    while (n > 0) {
        int vrow = g_sb_col == 0 ? 0 : (g_sb_col - 1) / width;
        int col = g_sb_col - vrow * width;
        if (vrow >= n) { g_sb_col -= n * width; return; }
        n -= vrow + 1;
        if (g_sb_line == 0) { g_sb_col = col; return; }
        g_sb_line--;
        while (g_sb_line > 0 && sb_hidden(g_sb_line)) g_sb_line--;   /* a folded body is not on screen */
        sb_ensure_line();
        int pw = vis_width_n(g_sb[g_sb_line].data, g_sb[g_sb_line].len);
        int prow = pw == 0 ? 0 : (pw - 1) / width;
        g_sb_col = prow * width + col;
    }
}
static void sb_cursor_down(int n) {
    int width = sb_width(); if (width < 1) width = 1;
    while (n-- > 0) {
        int w = vis_width_n(g_sb[g_sb_line].data, g_sb[g_sb_line].len);
        int lastrow = w == 0 ? 0 : (w - 1) / width, vrow = g_sb_col == 0 ? 0 : (g_sb_col - 1) / width;
        if (vrow < lastrow) { g_sb_col += width; continue; }
        int next = g_sb_line + 1;
        while (sb_hidden(next)) next++;
        if (next >= g_sb_n) return;
        g_sb_line = next; g_sb_col = g_sb_col - vrow * width;
    }
}
static void sb_newline(void) {
    sb_ensure_line();
    int next = g_sb_line + 1;
    while (!g_fold_rec && sb_hidden(next)) next++;
    if (next < g_sb_n) { g_sb_line = next; g_sb_col = 0; return; }   /* (a "\n" after cursor-up: move down) */
    g_sb_line = g_sb_n; g_sb_col = 0; sb_ensure_line();
}
static void sb_escape_done(void) {
    const char *q = g_sb_seq; int n = g_sb_seqn; g_sb_seqn = -1;
    if (n < 2 || q[0] != '[') {   /* two-byte ESC sequences: DECSC / DECRC bracket the status bar; the rest is ignored */
        if (n == 1 && q[0] == '7') g_sb_decsc = 1;
        else if (n == 1 && q[0] == '8') g_sb_decsc = 0;
        return;
    }
    char fin = q[n - 1]; int a = q[1] >= '0' && q[1] <= '9' ? atoi(q + 1) : 0;
    if (fin == 'm') {
        sb_ensure_line(); sb_cut_at_cursor(); sbuf *l = &g_sb[g_sb_line]; sb_putc(l, 27); sb_append(l, q, (size_t)n);
        if (n == 2 || (n == 3 && q[1] == '0')) g_sb_sgrn = 0;                       /* "[m" / "[0m": back to plain */
        else if (g_sb_sgrn + n + 1 <= (int)sizeof g_sb_sgr) { g_sb_sgr[g_sb_sgrn++] = 27; memcpy(g_sb_sgr + g_sb_sgrn, q, (size_t)n); g_sb_sgrn += n; }
        return;
    }
    if (q[1] == '?') return;                                  /* private modes (bracketed paste, alt screen) */
    switch (fin) {
        case 'A': sb_cursor_up(a ? a : 1); break;
        case 'B': sb_cursor_down(a ? a : 1); break;
        case 'C': g_sb_col += a ? a : 1; break;
        case 'D': g_sb_col -= a ? a : 1; if (g_sb_col < 0) g_sb_col = 0; break;
        case 'K': if (a == 2) { int width = sb_width(); if (width < 1) width = 1; g_sb_col = ((g_sb_col == 0 ? 0 : (g_sb_col - 1) / width)) * width; }
                  sb_cut_at_cursor(); break;
        case 'J': if (a == 2) { for (int i = 0; i < g_sb_n; i++) sb_free(&g_sb[i]); g_sb_n = 0; g_sb_line = 0; g_sb_col = 0; g_sb_epoch++; }
                  else { sb_cut_at_cursor(); sb_drop_after(g_sb_line); }
                  break;
        default: break;                                       /* H, r, n … : absolute moves and queries, not tracked */
    }
}
static void sb_feed(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (g_sb_seqn >= 0) {   /* collecting an escape sequence */
            if (g_sb_seqn < (int)sizeof g_sb_seq - 1) g_sb_seq[g_sb_seqn++] = (char)c;
            bool csi = g_sb_seq[0] == '[';
            if (!csi) { if (g_sb_seqn == 1 && c != '[') sb_escape_done(); }        /* ESC x */
            else if (g_sb_seqn > 1 && c >= 0x40 && c <= 0x7E) sb_escape_done();      /* final byte */
            continue;
        }
        if (c == 27) { g_sb_seqn = 0; continue; }
        if (g_sb_decsc) continue;
        if (c == '\r') { g_sb_cr = true; continue; }
        if (g_sb_cr) { g_sb_cr = false; if (c != '\n') { int width = sb_width(); if (width < 1) width = 1; sb_ensure_line(); g_sb_col = ((g_sb_col == 0 ? 0 : (g_sb_col - 1) / width)) * width; } }
        if (c == '\n') { sb_newline(); continue; }
        if (c == '\b') { if (g_sb_col > 0) g_sb_col--; continue; }
        if (c == '\a' || c == 0) continue;
        if (c == '\t') { sb_cut_at_cursor(); sbuf *l = &g_sb[g_sb_line]; do { sb_putc(l, ' '); g_sb_col++; } while (g_sb_col % 8); continue; }
        sb_cut_at_cursor();
        sbuf *l = &g_sb[g_sb_line];
        sb_putc(l, (char)c);
        if ((c & 0xC0) != 0x80) g_sb_col++;   /* count code points, not continuation bytes */
    }
}

/* ---------- the cursor while a turn runs (see g_away) ---------- */
static void fd1_write(const char *b, size_t n) {   /* a cursor move to fd 1 itself (never through stdio: the hook calls this) */
    trace_bytes('O', b, n);
    size_t off = 0;
    while (off < n) { ssize_t w = write(STDOUT_FILENO, b + off, n - off); if (w < 0) { if (errno == EINTR) continue; return; } off += (size_t)w; }
}
static bool want_box(void) {
    return g_fs && g_busy && !g_field_focus && g_view_top < 0 && !g_fold_rec && !g_no_park && g_ta_caret_row > 0;
}
static void transcript_move(sbuf *o, bool flush) {
    int row, col;
    conv_pos_x(fs_region(), &row, &col, flush);
    sb_printf(o, "\x1b[%d;%dH", row, col);
    g_away = false;
}
static void box_move(sbuf *o) {
    sb_printf(o, "\x1b[%d;%dH", g_ta_caret_row, g_ta_caret_col);
    g_away = true;
}
/* the end of the chrome: the cursor goes to the type-ahead's caret, or back to the transcript if it was in the box */
static void chrome_park(sbuf *o) {
    if (want_box()) box_move(o);
    else if (g_away) transcript_move(o, true);
}
/* A question or menu is about to draw at the transcript's end: the cursor is brought there, and kept
 * there until park_free (the chrome does not park the cursor while one is up). */
static void park_hold(void) {
    if (g_away && g_fs && g_sb_active) {
        sbuf o; sb_init(&o); transcript_move(&o, true);
        sb_pause(true); fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_pause(false);
        sb_free(&o);
    }
    g_no_park++;
}
static void park_free(void) { if (g_no_park > 0) g_no_park--; }

static theme_map g_th_map;   /* an escape sequence a write ended inside (see theme_map_bytes) */
/* stdout hook: every byte goes to fd 1 and, unless paused, into the model above */
static ssize_t sb_write_fn(void *cookie, const char *buf, size_t n) {
    (void)cookie;
    /* Scrolled back: the screen shows an earlier window, so conversation output is only
     * recorded. Leaving the viewer paints what came meanwhile from the record (conv_repaint). */
    if ((g_view_top >= 0 || g_fold_rec || g_ov_text) && !g_sb_pause) { trace_bytes('H', buf, n); sb_feed(buf, n); return (ssize_t)n; }   /* (or it is the body of a fold) */
    /* the cursor rests in the input field during a turn: the transcript's end is where this goes.
     * The position comes from the model before these bytes are in it (conv_pos_x, no flush here). */
    if (g_away && !g_sb_pause && !g_ui_nc) { sbuf mv; sb_init(&mv); transcript_move(&mv, false); fd1_write(mv.data, mv.len); sb_free(&mv); }
    trace_bytes('O', buf, n);
    if (!g_ui_nc) {   /* notcurses draws the screen itself: the output is only recorded */
        /* the basic colour codes become the theme's RGB here, on the way out; the model keeps them as they came */
        sbuf mapped; sb_init(&mapped);
        theme_map_bytes(&g_th_map, buf, n, &mapped);
        size_t off = 0;
        while (off < mapped.len) {
            ssize_t w = write(STDOUT_FILENO, mapped.data + off, mapped.len - off);
            if (w < 0) { if (errno == EINTR) continue; sb_free(&mapped); return -1; }
            off += (size_t)w;
        }
        sb_free(&mapped);
    }
    if (!g_sb_pause) sb_feed(buf, n);
    if (g_ui_nc) nc_output();
    /* parked again only where the chunk ends between sequences: a chunk can end anywhere in a large write,
     * and a cursor move spliced into an escape would corrupt it. Until the next refresh the cursor stays at
     * the transcript, where the next chunk goes anyway. */
    else if (!g_sb_pause && want_box() && !tail_incomplete(buf, n)) { sbuf mv; sb_init(&mv); box_move(&mv); fd1_write(mv.data, mv.len); sb_free(&mv); }
    return (ssize_t)n;
}
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
static int sb_write_bsd(void *cookie, const char *buf, int n) { return (int)sb_write_fn(cookie, buf, (size_t)n); }
#endif
static void sb_hook_stdout(void) {
    if (g_sb_active) return;
    fflush(stdout);
    FILE *f = NULL;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    f = funopen(NULL, NULL, sb_write_bsd, NULL, NULL);
#else
    cookie_io_functions_t io = { NULL, sb_write_fn, NULL, NULL };
    f = fopencookie(NULL, "w", io);
#endif
    if (!f) return;
    setvbuf(f, NULL, _IOFBF, 1 << 16);
    stdout = f;
    g_sb_active = true;
}
/* ---------- the chrome as sent: a row whose text did not change is not sent again ----------
 * The chrome (field, rules, bar, caret) is one buffer of cursor moves, each followed by its row's text. Each
 * segment (from one cursor move to the next) that has text is kept with its position; a segment equal to the
 * one kept is not sent. Segments without text (the cursor, the saved-cursor escapes) are always sent. The
 * cache is cleared whenever something else may have drawn over the chrome (sb_pause(true): menus, overlays,
 * the viewer; a repaint; a resize; an erase). Over ssh, a keystroke then sends the row it changed. */
#define CHROME_KEYS 96
static struct { int row, col; char *text; size_t n; } g_chrome[CHROME_KEYS];
static int g_chrome_n = 0;
static void chrome_invalidate(void) {
    for (int i = 0; i < g_chrome_n; i++) { free(g_chrome[i].text); g_chrome[i].text = NULL; }
    g_chrome_n = 0;
}
/* a cursor move "ESC [ row ; col H" at p: its length, and its position */
static size_t cup_len(const char *p, const char *end, int *row, int *col) {
    if (end - p < 6 || p[0] != 27 || p[1] != '[') return 0;
    const char *q = p + 2; int r = 0, c = 0; int nr = 0, nc = 0;
    while (q < end && *q >= '0' && *q <= '9') { r = r * 10 + (*q - '0'); q++; nr++; }
    if (!nr || q >= end || *q != ';') return 0;
    q++;
    while (q < end && *q >= '0' && *q <= '9') { c = c * 10 + (*q - '0'); q++; nc++; }
    if (!nc || q >= end || *q != 'H') return 0;
    *row = r; *col = c;
    return (size_t)(q + 1 - p);
}
/* whether [p, end) has text on the screen (not only escapes) */
static bool seg_has_text(const char *p, const char *end) {
    while (p < end) {
        if (*p == 27) {
            p++;
            if (p < end && *p == '[') { p++; while (p < end && !(*p >= 0x40 && *p <= 0x7e)) p++; if (p < end) p++; }
            else if (p < end) p++;
            continue;
        }
        if ((unsigned char)*p >= 0x20) return true;
        p++;
    }
    return false;
}
/* whether [p, end) erases (ESC [ K, ESC [ J): part of a row's text, as the field clears a row before drawing it */
static bool seg_erases(const char *p, const char *end) {
    for (const char *q = p; q + 2 < end; q++) if (q[0] == 27 && q[1] == '[' && (q[2] == 'K' || q[2] == 'J')) return true;
    return false;
}
static void chrome_send(sbuf *o) {
    if (!o->len) return;
    const char *p = o->data, *end = o->data + o->len;
    sbuf out; sb_init(&out);
    /* a row is the group of its segments (its clear, its text, its borders): kept and compared as one. A
     * segment with no text and no erase (a bare cursor move, the caret), or one with the saved cursor, is a
     * barrier: it ends the group and is always sent. */
    sbuf grp; sb_init(&grp);
    int grow = -1; bool gtext = false;
    #define CHROME_FLUSH() do { \
        if (grp.len) { \
            bool sent = true; \
            if (grow >= 0 && gtext) { \
                int hit = -1; \
                for (int i = 0; i < g_chrome_n; i++) if (g_chrome[i].row == grow) { hit = i; break; } \
                if (hit >= 0 && g_chrome[hit].n == grp.len && !memcmp(g_chrome[hit].text, grp.data, grp.len)) sent = false; \
                else if (hit < 0 && g_chrome_n >= CHROME_KEYS) { } \
                else { int i = hit >= 0 ? hit : g_chrome_n++; free(g_chrome[i].text); g_chrome[i].row = grow; g_chrome[i].col = 0; g_chrome[i].text = xmalloc(grp.len); g_chrome[i].n = grp.len; memcpy(g_chrome[i].text, grp.data, grp.len); } \
            } \
            if (sent) { sb_puts(&out, C_RESET); sb_append(&out, grp.data, grp.len); } \
            sb_clear(&grp); grow = -1; gtext = false; \
        } \
    } while (0)
    while (p < end) {
        const char *stop = end;
        for (const char *q = p + 1; q < end; q++) { int r, c; if (cup_len(q, end, &r, &c)) { stop = q; break; } }
        int sr = 0, sc = 0;
        bool keyed = cup_len(p, end, &sr, &sc) > 0;
        bool saved = false;
        for (const char *q = p; q + 1 < stop; q++) if (q[0] == 27 && (q[1] == '7' || q[1] == '8')) { saved = true; break; }
        bool text = seg_has_text(p, stop), erase = seg_erases(p, stop);
        size_t n = (size_t)(stop - p);
        if (!keyed || saved || (!text && !erase)) {
            CHROME_FLUSH();
            sb_puts(&out, C_RESET); sb_append(&out, p, n);
        } else {
            if (grow != sr) { CHROME_FLUSH(); grow = sr; }
            sb_append(&grp, p, n);
            if (text) gtext = true;
        }
        p = stop;
    }
    CHROME_FLUSH();
    #undef CHROME_FLUSH
    bool was = g_sb_pause;
    if (g_sb_active) { fflush(stdout); g_sb_pause = true; }   /* not conversation */
    fwrite(out.data, 1, out.len, stdout); fflush(stdout);
    g_sb_pause = was;
    sb_free(&grp);
    sb_free(&out);
}
static void sb_pause(bool on) {
    if (!g_sb_active) return;
    fflush(stdout);
    if (on) chrome_invalidate();   /* something else is about to draw on the screen: the chrome is sent in full next time */
    g_sb_pause = on;
}
/* record text that was drawn while paused (the editor's submitted line) */
static void sb_note(const char *text) { if (g_sb_active) { fflush(stdout); sb_feed(text, strlen(text)); } }

/* The scrollback model knows which column the conversation output sits in, so anything
 * that has to start on its own line (a command run while the model is mid-sentence) can
 * ask for the break here instead of guessing. */
void term_line_break(void) {
    fflush(stdout);
    if (!g_sb_active || g_sb_col > 0) fputs("\n", stdout);
}

/* visual rows of the buffer at the given width: (line, byte offset, byte length) triples */
/* the visual rows of the transcript, counted from its end, up to `need` of them (the same wrap as sb_rows):
 * where the transcript ends needs no more, and the whole scrollback is not walked for it on every write */
static int sb_rows_upto(int width, int need) {
    if (width < 1) width = 1;
    int n = 0;
    for (int i = g_sb_n - 1; i >= 0 && n < need; i--) {
        if (g_sb_meta[i].hidden) continue;
        const char *d = g_sb[i].data; size_t len = g_sb[i].len, off = 0;
        do { off = vis_offset(d + off, len - off, width) + off; n++; } while (off < len);
    }
    return n;
}
static sb_row *sb_rows(int width, int *count) {
    if (width < 1) width = 1;
    int cap = 256, n = 0; sb_row *r = xmalloc(sizeof *r * (size_t)cap);
    for (int i = 0; i < g_sb_n; i++) {
        if (g_sb_meta[i].hidden) continue;
        const char *d = g_sb[i].data; size_t len = g_sb[i].len, off = 0;
        do {
            size_t end = vis_offset(d + off, len - off, width) + off;
            if (n == cap) { cap *= 2; r = xrealloc(r, sizeof *r * (size_t)cap); }
            r[n++] = (sb_row){ i, off, end - off };
            off = end;
        } while (off < len);
    }
    *count = n; return r;
}

/* Paint the conversation region from the model: the tail of the transcript, at the bottom of
 * the region once it fills it, and the cursor back where output continues. That place is reached
 * by printing the cursor's row once more up to the cursor instead of by an absolute move: an
 * absolute move would lose a wrap that is pending in the last column, and would land wrong
 * wherever the terminal counts a character wider than the model does. The rows themselves are
 * painted with autowrap off for the same reason — one the terminal finds too wide is clipped at
 * the margin rather than spilling into the row below. */
/* Apply the SGR sequences in d[0..len) to st: the colours in effect at the end of it (a reset
 * empties st). A row cut from a long line starts mid-line, so the colours set before its first
 * character must be sent again when it is painted, or the rest of the line loses them. */
void term_sgr_track(sbuf *st, const char *d, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (d[i] != 27 || i + 1 >= len || d[i + 1] != '[') continue;
        size_t j = i + 2;
        while (j < len && !((unsigned char)d[j] >= 0x40 && (unsigned char)d[j] <= 0x7E)) j++;
        if (j >= len) break;
        if (d[j] == 'm') {
            size_t pl = j - (i + 2);
            if (pl == 0 || (pl == 1 && d[i + 2] == '0')) sb_clear(st);
            else sb_append(st, d + i, j - i + 1);
        }
        i = j;
    }
}
/* Paint rows [from, from+count) of the model on the region's lines 1..count (the rest of them
 * blank). Each row starts with the colours in effect where its text starts, which is the same
 * screen as printing the transcript straight through. When `mark` is a row, the colours in effect
 * at its start go to `at_mark` (the cursor's row is printed again after this). */
static void paint_rows(sbuf *o, const sb_row *r, int n, int from, int count, int mark, sbuf *at_mark) {
    sbuf st; sb_init(&st);
    for (int idx = 0; idx < n && idx < from + count; idx++) {
        const char *t = g_sb[r[idx].line].data ? g_sb[r[idx].line].data + r[idx].off : "";
        if (idx == mark && at_mark) { sb_clear(at_mark); sb_append(at_mark, st.data, st.len); }
        if (idx >= from) {
            sb_printf(o, "\x1b[%d;1H\x1b[2K", idx - from + 1);
            sb_puts(o, C_RESET); sb_append(o, st.data, st.len);
            sb_append(o, t, r[idx].len); sb_puts(o, C_RESET);
        }
        term_sgr_track(&st, t, r[idx].len);
    }
    for (int i = n > from ? n - from : 0; i < count; i++) sb_printf(o, "\x1b[%d;1H\x1b[2K", i + 1);
    sb_free(&st);
}

/* The overlay's window, appended to `o` as cursor-addressed paint: its text in a box centred on the
 * region and painted over the rows, so what it covers is hidden. Lines are wrapped to the box's width
 * (the colours they carry are carried over to the next row); a text taller than the region ends in
 * a row that says the rest is cut. */
typedef struct { const char *p; size_t n; } ov_row;
/* the banner's modal: its lines as they are, centred on the conversation's region; the close button is the
 * top right cell of the box (the top border's last column) */
static void banner_box(sbuf *o, const bool *only) {
    int region = fs_region(), cols = g_fs_cols > 0 ? g_fs_cols : term_width();
    if (!g_bn_n || region < g_bn_n || cols < g_bn_bw) return;
    int top = (region - g_bn_n) / 2 + 1, left = (cols - g_bn_bw) / 2 + 1; if (left < 1) left = 1;
    g_bn_xr = top; g_bn_xc0 = left + g_bn_bw - BANNER_X_FROM_RIGHT - BANNER_X_W / 2; g_bn_xc1 = g_bn_xc0 + BANNER_X_W - 1;
    for (int i = 0; i < g_bn_n; i++) {
        if (only && !only[i]) continue;   /* a row that did not change is not sent again (over ssh every byte counts) */
        sb_printf(o, "\x1b[%d;%dH" C_RESET "%s" C_RESET, top + i, left, g_bn[i].data ? g_bn[i].data : "");
    }
}
static void overlay_box(sbuf *o) {
    if (!g_ov_text) return;
    if (g_banner_on) { banner_box(o, NULL); return; }
    int region = fs_region(), cols = g_fs_cols > 0 ? g_fs_cols : term_width();
    if (region < 4 || cols < 16) return;
    const char *base = g_ov_text;
    size_t len = strlen(base); while (len && base[len - 1] == '\n') len--;
    const char *end = base + len;
    int maxw = cols - 8; if (maxw < 10) maxw = 10;
    int W = 0;   /* the widest line, up to the box's width */
    for (const char *p = base; ; ) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        int w = vis_width_n(p, e ? (size_t)(e - p) : (size_t)(end - p)); if (w > W) W = w;
        if (!e) break;
        p = e + 1;
    }
    if (W > maxw) W = maxw;
    if (W < 20) W = maxw < 20 ? maxw : 20;
    /* the rows: each line cut where it no longer fits */
    size_t cap = 32; int R = 0; ov_row *rows = xmalloc(sizeof *rows * cap);
    for (const char *p = base; ; ) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        size_t ll = e ? (size_t)(e - p) : (size_t)(end - p), done = 0;
        do {
            size_t off = done < ll ? vis_offset(p + done, ll - done, W) : 0;
            if (off == 0) off = ll - done;   /* (an empty line is one empty row) */
            if (R == (int)cap) { cap *= 2; rows = xrealloc(rows, sizeof *rows * cap); }
            rows[R++] = (ov_row){ p + done, off };
            done += off;
        } while (done < ll);
        if (!e) break;
        p = e + 1;
    }
    int maxh = region - 2, show = R > maxh ? maxh - 1 : R;   /* the last row says the rest is cut */
    int bh = show + 2 + (R > maxh ? 1 : 0), bw = W + 4;
    int top = (region - bh) / 2 + 1, left = (cols - bw) / 2 + 1; if (left < 1) left = 1;
    sb_printf(o, "\x1b[%d;%dH" C_DIM "┌", top, left);
    for (int i = 0; i < bw - 2; i++) sb_puts(o, "─");
    sb_puts(o, "┐" C_RESET);
    sbuf st; sb_init(&st);   /* the colours in effect at the start of the row being painted */
    for (int row = 0; row < bh - 2; row++) {
        sb_printf(o, "\x1b[%d;%dH" C_DIM "│" C_RESET " " C_RESET, top + 1 + row, left);
        int vis = 0;
        if (row < show) {
            sb_append(o, st.data, st.len);
            sb_append(o, rows[row].p, rows[row].n);
            vis = vis_width_n(rows[row].p, rows[row].n);
            term_sgr_track(&st, rows[row].p, rows[row].n);
        } else if (row == show && R > maxh) { sb_puts(o, C_DIM "… more than fits here" C_RESET); vis = 22; }
        sb_puts(o, C_RESET);
        for (; vis < W; vis++) sb_putc(o, ' ');
        sb_printf(o, " " C_DIM "│" C_RESET);
    }
    sb_printf(o, "\x1b[%d;%dH" C_DIM "└", top + bh - 1, left);
    for (int i = 0; i < bw - 2; i++) sb_puts(o, "─");
    sb_puts(o, "┘" C_RESET);
    sb_free(&st); free(rows);
}

/* which of the model's rows is the region's first on the live screen; *cur and *ccol: the cursor's row, and its column in it */
static int conv_first(const sb_row *r, int n, int region, int cols, int *cur_out, int *ccol_out) {
    int cur = n ? n - 1 : 0, ccol = g_sb_col;
    for (int i = 0; i < n; i++) if (r[i].line == g_sb_line) {
        cur = i;
        while (ccol > cols && cur + 1 < n && r[cur + 1].line == g_sb_line) { cur++; ccol -= cols; }
        break;
    }
    if (ccol > cols) ccol = cols;
    int first = n > region ? n - region : 0;
    if (cur < first) first = cur;
    if (cur_out) *cur_out = cur;
    if (ccol_out) *ccol_out = ccol;
    return first;
}
static void conv_repaint(sbuf *o) {
    chrome_invalidate();   /* the region is painted again, which may have erased rows of the chrome */
    fflush(stdout);   /* the model only sees what left stdio's buffer */
    int region = fs_region(), cols = g_fs_cols > 0 ? g_fs_cols : 1, n = 0;
    sb_row *r = sb_rows(cols, &n);
    int cur, ccol;   /* the cursor's visual row, and its column in that row */
    int first = conv_first(r, n, region, cols, &cur, &ccol);
    sb_puts(o, "\x1b[?7l");
    sbuf at_cur; sb_init(&at_cur);
    paint_rows(o, r, n, first, region, cur, &at_cur);
    overlay_box(o);
    sb_puts(o, "\x1b[?7h");
    int srow = cur - first + 1;
    sb_printf(o, "\x1b[%d;1H", srow);
    if (n && ccol > 0) {
        const char *d = g_sb[r[cur].line].data + r[cur].off;
        size_t off = vis_offset(d, r[cur].len, ccol);
        int have = vis_width_n(d, off);
        sb_puts(o, C_RESET); sb_append(o, at_cur.data, at_cur.len);
        sb_append(o, d, off);
        if (have < ccol) sb_printf(o, "\x1b[%dC", ccol - have);   /* the cursor stood beyond the text */
    }
    sb_puts(o, C_RESET); sb_append(o, g_sb_sgr, (size_t)g_sb_sgrn);
    g_conv_row = srow; g_conv_col = ccol % cols + 1;
    g_away = false;   /* the cursor is where the transcript ends */
    sb_free(&at_cur);
    free(r);
}

/* ---------- scrolling back ----------
 * The wheel (over the conversation) and PgUp/PgDn move a window over the scrollback, at the
 * prompt and while the model works alike. g_view_top says which rows the region shows; while it
 * is set, conversation output is recorded but not painted (sb_write_fn), a line under the window
 * says where it is (view_note: one row of the region goes to the chrome for it — the bar stays
 * the bar), and the chrome goes on being drawn as ever — the input field keeps taking what is
 * typed. Scrolling down to the last row, or anything that needs the live screen (a question, a
 * menu), goes back to it: view_leave() paints the tail again, with what arrived meanwhile. */
#define WHEEL_ROWS 3
/* rows the window has while scrolled back: the line under it takes one of the region's */
static int view_rows(void) { int r = fs_region() - (g_view_top < 0 && field_shown()); return r < 1 ? 1 : r; }

static void view_paint(void) {
    bool was = g_sb_pause; sb_pause(true);
    layout_sync(); g_view_stale = false;
    int rows = fs_region(), nrows; sb_row *r = sb_rows(g_fs_cols, &nrows);
    int maxtop = nrows > rows ? nrows - rows : 0;
    if (g_view_top > maxtop) g_view_top = maxtop;
    trace_note('V', "top=%d window=%d of=%d", g_view_top, rows, nrows);
    sbuf o; sb_init(&o);
    sb_puts(&o, "\x1b[?7l");
    paint_rows(&o, r, nrows, g_view_top, rows, -1, NULL);
    overlay_box(&o);
    sb_puts(&o, "\x1b[?7h");
    snprintf(g_view_hint, sizeof g_view_hint, g_field_focus ? "↑ scrollback · rows %d-%d of %d · wheel/PgUp/PgDn ↑/↓ scroll · End/Esc/Enter back"
                                                           : "↑ scrollback · rows %d-%d of %d · Esc or scrolling down: back to the output",
             nrows ? g_view_top + 1 : 0, g_view_top + rows < nrows ? g_view_top + rows : nrows, nrows);
    chrome_append(&o);
    fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o); free(r);
    sb_pause(was);
}

/* Back to the live screen: the tail of the transcript, the cursor where output continues. */
static void view_leave(void) {
    if (g_view_top < 0) return;
    trace_note('V', "left (was top=%d)", g_view_top);
    g_view_top = -1; g_view_stale = false;
    bool was = g_sb_pause; sb_pause(true);
    if (!layout_sync()) {   /* (a layout change paints it by itself) */
        sbuf o; sb_init(&o); conv_repaint(&o);
        fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);
    }
    sb_pause(was);
    term_status_refresh();   /* the chrome without the viewer's line; the editor gets its cursor back */
}

/* The overlay: a text in a centred window while the conversation goes on underneath (the output is
 * recorded, not painted, until it is closed — the same as while scrolled back). Esc or Ctrl-C closes
 * it; anything that needs the live screen does too. */
static void overlay_paint(void) {
    bool was = g_sb_pause; sb_pause(true);
    fflush(stdout);
    sbuf o; sb_init(&o); conv_repaint(&o);
    fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);
    sb_pause(was);
}
/* the banner's modal: its lines (kept here for the redraws), with its width; the ANSI screen draws it
 * over the conversation, the notcurses screen draws it on its own (term_nc.c) */
static void banner_store(const char **lines, int n, int bw) {
    for (int i = 0; i < g_bn_n; i++) sb_free(&g_bn[i]);
    free(g_bn); g_bn = NULL; g_bn_n = 0;
    if (n <= 0) return;
    g_bn = xmalloc(sizeof *g_bn * (size_t)n); g_bn_n = n; g_bn_bw = bw;
    for (int i = 0; i < n; i++) { sb_init(&g_bn[i]); sb_puts(&g_bn[i], lines[i]); }
}
bool term_banner_show(const char **lines, int n, int bw) {
    if (g_ui_nc) { nc_banner_set(lines, n, bw, true); return true; }
    if (!g_fs || !g_sb_active || n <= 0) return false;
    banner_store(lines, n, bw);
    if (!g_ov_text) g_ov_text = xstrdup("");   /* the overlay's state: Esc closes it, the conversation is recorded, not painted */
    g_banner_on = true; g_bn_xr = g_bn_xc0 = g_bn_xc1 = -1;
    overlay_paint();
    term_status_refresh();
    return true;
}
void term_banner_draw(const char **lines, int n, int bw) {
    if (g_ui_nc) { nc_banner_set(lines, n, bw, nc_banner_open()); return; }
    if (!g_banner_on) return;
    /* only the rows that changed are sent: a tick changes the crow and the spark, most rows of the box stay */
    bool *changed = xmalloc(sizeof *changed * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) changed[i] = n != g_bn_n || !g_bn[i].data || strcmp(g_bn[i].data, lines[i]) != 0;
    banner_store(lines, n, bw);
    sbuf o; sb_init(&o);
    bool was = g_sb_pause; sb_pause(true);   /* the box only: the conversation is not repainted for a tick */
    sb_puts(&o, "\x1b" "7");   /* the cursor (it rests in the field) is saved and put back */
    banner_box(&o, changed);
    free(changed);
    sb_puts(&o, "\x1b" "8");
    fwrite(o.data, 1, o.len, stdout); fflush(stdout);
    sb_pause(was);
    sb_free(&o);
}
void term_banner_hide(void) {
    if (g_ui_nc) { nc_banner_set(NULL, 0, 0, false); return; }
    if (g_banner_on) term_overlay_close();
}
bool term_banner_open(void) { return g_ui_nc ? nc_banner_open() : g_banner_on; }

bool term_overlay_show(const char *text) {
    if (g_ui_nc) { if (!text) return false; nc_overlay_set(text); return true; }   /* the notcurses UI draws it as a modal too */
    if (!g_fs || !g_sb_active || !text) return false;
    free(g_ov_text); g_ov_text = xstrdup(text);
    overlay_paint();
    term_status_refresh();
    return true;
}
void term_overlay_close(void) {
    if (g_ui_nc) { nc_overlay_set(NULL); return; }
    if (!g_ov_text) return;
    free(g_ov_text); g_ov_text = NULL; g_banner_on = false;
    bool was = g_sb_pause; sb_pause(true);
    if (!layout_sync()) { sbuf o; sb_init(&o); conv_repaint(&o); fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o); }
    sb_pause(was);
    term_status_refresh();
}

/* Move the window by `delta` rows (negative = back), entering or leaving the viewer as needed. */
static void view_scroll(int delta) {
    if (!g_fs || !g_sb_active) return;
    fflush(stdout);
    int live = view_rows() + field_shown(), nrows; free(sb_rows(g_fs_cols, &nrows));   /* the region when not scrolled back */
    int maxtop = nrows > live ? nrows - live : 0;
    long top = (long)(g_view_top >= 0 ? g_view_top : maxtop) + delta;
    if (top < 0) top = 0;
    if (top >= maxtop) { view_leave(); return; }   /* the live screen starts there or above: back to it */
    g_view_top = (int)top;
    view_paint();
}


/* ---------- folds ----------
 * term_fold_begin(head) prints `head` — one line, with TERM_FOLD_CLOSED in it — and from there
 * to term_fold_end() what is printed goes into the model as that line's body without being
 * painted. A left click on the head (mouse_key notes the row, fold_click acts on it, at the
 * prompt, in the viewer and while the model works alike) shows or hides the body and turns the
 * mark; the region is then painted again from the model, as after a resize. False = no folds
 * here (no full-screen mode, so no model and no mouse): the caller prints a preview instead. */
bool term_fold_begin(const char *head) {
    if ((!g_fs && !g_ui_nc) || !g_sb_active || g_fold_rec) return false;   /* no folds without a screen to click (ANSI full-screen or notcurses); none inside one */
    fflush(stdout);
    if (g_sb_col > 0) fputs("\n", stdout);
    fputs(head, stdout); fflush(stdout);
    sb_ensure_line();
    g_sb_meta[g_sb_line] = (sb_meta){ ++g_fold_seq, SB_FOLD_HEAD, false };
    fputs("\n", stdout); fflush(stdout);
    g_fold_rec = g_fold_seq;
    sb_ensure_line();
    g_sb_meta[g_sb_line] = (sb_meta){ g_fold_rec, SB_FOLD_BODY, true };
    return true;
}
void term_fold_end(void) {
    if (!g_fold_rec) return;
    fflush(stdout);
    if (g_sb_col > 0) sb_feed("\n", 1);
    g_fold_rec = 0;
    sb_ensure_line();
    g_sb_meta[g_sb_line] = (sb_meta){ 0, SB_PLAIN, false };   /* the line output continues on is no part of the body */
}
/* The block stays up in the viewer, as after PgUp: Esc, Enter or End return to the prompt. A block
 * of plain text shown as an open fold, and the view moved to its head: what ← at the
 * prompt shows a sub-agent's log with. The lines go into the model as the fold's body without
 * being painted (the view is painted from the model below). Off full-screen mode: printed. */
static void show_block_impl(const char *head, const char *body);
void term_show_block(const char *head, const char *body) { park_hold(); show_block_impl(head, body); park_free(); }
static void show_block_impl(const char *head, const char *body) {
    size_t blen = strlen(body);
    if (!g_fs || !g_sb_active) { printf("%s\n%s%s", head, body, blen && body[blen - 1] != '\n' ? "\n" : ""); fflush(stdout); return; }
    fflush(stdout);
    if (g_sb_col > 0) sb_feed("\n", 1);
    int id = ++g_fold_seq;
    sb_ensure_line();
    g_fold_rec = id;
    g_sb_meta[g_sb_line] = (sb_meta){ id, SB_FOLD_HEAD, false };
    sb_feed(head, strlen(head)); sb_feed("\n", 1);
    sb_feed(body, blen);
    if (blen && body[blen - 1] != '\n') sb_feed("\n", 1);
    g_fold_rec = 0;
    sb_ensure_line();
    g_sb_meta[g_sb_line] = (sb_meta){ 0, SB_PLAIN, false };   /* the prompt's line is no part of the block */
    for (int i = 0; i < g_sb_n; i++) if (g_sb_meta[i].fold == id) g_sb_meta[i].hidden = false;   /* open */
    int n, top = -1; sb_row *r = sb_rows(g_fs_cols, &n);
    int head_line = -1;
    for (int i = 0; i < g_sb_n && head_line < 0; i++) if (g_sb_meta[i].fold == id && g_sb_meta[i].kind == SB_FOLD_HEAD) head_line = i;
    for (int i = 0; i < n && top < 0 && head_line >= 0; i++) if (r[i].line == head_line) top = i;
    free(r);
    if (top < 0) return;
    g_view_top = top;
    view_paint();
}

/* The head at scrollback line `line` is shown or hidden: its body lines (line+1 … line+n) are flipped and
 * its mark turned. Returns n, the body lines flipped; -1 when `line` is no fold's head (or a fold is being
 * written). Only the model changes; the caller repaints the screen it draws. Shared by the ANSI screen's
 * clicks and the notcurses pane's. */
int term_fold_toggle(int line) {
    if (g_fold_rec || line < 0 || line >= g_sb_n || g_sb_meta[line].kind != SB_FOLD_HEAD) return -1;
    fflush(stdout);
    int id = g_sb_meta[line].fold, nb = 0; bool open = false;
    for (int i = line + 1; i < g_sb_n && g_sb_meta[i].fold == id && g_sb_meta[i].kind == SB_FOLD_BODY; i++) { g_sb_meta[i].hidden = !g_sb_meta[i].hidden; open = !g_sb_meta[i].hidden; nb++; }
    char *mark = strstr(g_sb[line].data, open ? TERM_FOLD_CLOSED : TERM_FOLD_OPEN);   /* the two marks are as long as each other */
    if (mark) memcpy(mark, open ? TERM_FOLD_OPEN : TERM_FOLD_CLOSED, strlen(TERM_FOLD_OPEN));
    trace_note('F', "fold %d %s (line %d)", id, open ? "opened" : "closed", line);
    return nb;
}

static void fold_click(void) {
    int row = g_click_row; g_click_row = 0;
    if (!row || !g_fs || !g_sb_active || g_fold_rec || row > fs_region()) return;
    fflush(stdout);
    int n; sb_row *r = sb_rows(g_fs_cols, &n);
    int first = g_view_top >= 0 ? g_view_top : conv_first(r, n, fs_region(), g_fs_cols > 0 ? g_fs_cols : 1, NULL, NULL);
    int idx = first + row - 1, line = idx >= 0 && idx < n ? r[idx].line : -1;
    free(r);
    if (term_fold_toggle(line) < 0) return;
    if (g_view_top >= 0) view_paint();
    else {
        bool was = g_sb_pause; sb_pause(true);
        sbuf o; sb_init(&o); conv_repaint(&o);
        fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);
        sb_pause(was);
    }
    term_status_refresh();   /* the chrome, and the editor's cursor back in the field */
}

/* Type-ahead: bytes typed while the model was generating are kept here so
 * they are not lost; read_key() consumes them before touching stdin. */
static unsigned char g_ta[8192];
static size_t g_ta_len = 0, g_ta_pos = 0;
static int ta_get(void) { return g_ta_pos < g_ta_len ? g_ta[g_ta_pos++] : -1; }
static void ta_put(unsigned char c) {
    if (g_ta_pos == g_ta_len) g_ta_pos = g_ta_len = 0;
    if (g_ta_len < sizeof g_ta) g_ta[g_ta_len++] = c;
}
/* Set the pending type-ahead aside (so it cannot answer a question the user
 * has not seen yet) and bring it back afterwards for the next prompt. */
typedef struct { unsigned char *data; size_t len; } ta_stash;
static ta_stash ta_take(void) {
    ta_stash st = { NULL, 0 };
    if (g_ta_pos < g_ta_len) {
        st.len = g_ta_len - g_ta_pos;
        st.data = xmalloc(st.len);
        memcpy(st.data, g_ta + g_ta_pos, st.len);
    }
    g_ta_pos = g_ta_len = 0;
    if (g_ta_sug_for) term_status_refresh();   /* its suggestions go with it (ta_sug_sync) */
    return st;
}
static void ta_restore(ta_stash st) {
    if (!st.data) return;
    /* whatever was typed during the question comes first, then the older text */
    size_t cur = g_ta_len - g_ta_pos;
    unsigned char tmp[sizeof g_ta]; size_t tl = 0;
    if (cur) { memcpy(tmp, g_ta + g_ta_pos, cur); tl = cur; }
    g_ta_pos = g_ta_len = 0;
    for (size_t i = 0; i < st.len; i++) ta_put(st.data[i]);
    for (size_t i = 0; i < tl; i++) ta_put(tmp[i]);
    free(st.data);
}

/* ---------- message queue (Enter while the model is busy) ----------
 * Like Claude Code: text typed while the model is generating or a tool runs is
 * kept, and pressing Enter queues it as a message. Queued messages are shown in
 * the status bar and delivered by main.c at the next opportunity (between tool
 * rounds, or right after the turn ends). A queued message never stops the work in flight:
 * it waits. Text without Enter stays type-ahead and simply reappears in the editor. Slash
 * commands that main.c can answer without touching the conversation run right away instead
 * (term_run_while_busy). */
#define QUEUE_MAX 32
static char *g_queue[QUEUE_MAX];
int (*term_run_while_busy)(const char *line) = NULL;   /* set by main.c; see common.h */
static bool  g_ta_paste = false;     /* inside a bracketed paste (ESC[200~ … ESC[201~) */
static unsigned char g_ta_tail[6];   /* last bytes appended, to spot the paste brackets */

/* Whether a queued line is a message for the model (a /command or !line is the REPL's). */
static bool queue_is_plain(const char *m) { return m && *m != '/' && *m != '!'; }

int term_queue_count(void) { return g_queue_n; }
const char *term_queue_peek(void) { return g_queue_n ? g_queue[0] : NULL; }
char *term_queue_pop(void) {
    if (!g_queue_n) return NULL;
    char *m = g_queue[0];
    memmove(g_queue, g_queue + 1, sizeof(char*) * (size_t)(g_queue_n - 1));
    g_queue_n--;
    return m;
}
void term_queue_push(const char *msg) {
    if (!msg || !*msg) return;
    if (g_queue_n >= QUEUE_MAX) { free(g_queue[0]); memmove(g_queue, g_queue + 1, sizeof(char*) * (QUEUE_MAX - 1)); g_queue_n--; }
    g_queue[g_queue_n++] = xstrdup(msg);
}
void term_queue_clear(void) { while (g_queue_n) free(g_queue[--g_queue_n]); }

/* The oldest plain message, stepping over any /command or !line queued in front of it: those
 * wait for the REPL at the end of the turn, and a message behind one would otherwise wait
 * with them — for the whole turn — instead of reaching the model at the next round. */
char *term_queue_pop_plain(void) {
    for (int i = 0; i < g_queue_n; i++) {
        if (!queue_is_plain(g_queue[i])) continue;
        char *m = g_queue[i];
        memmove(g_queue + i, g_queue + i + 1, sizeof(char*) * (size_t)(g_queue_n - i - 1));
        g_queue_n--;
        return m;
    }
    return NULL;
}

/* Turn raw keystrokes typed while busy into message text: printable bytes are
 * kept, backspace deletes, Ctrl-U clears the line, Ctrl-W deletes a word, escape
 * sequences (arrows, paste brackets, alt+key) are dropped, Alt+Enter / Ctrl-J /
 * CR inside a paste become newlines. Returns malloc'd, trimmed text (may be ""). */
static char *keys_text(const unsigned char *b, size_t n, bool trim) {
    sbuf o; sb_init(&o); sb_append(&o, "", 0);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 27) {
            if (i + 1 >= n) break;
            unsigned char a = b[++i];
            if (a == '\r' || a == '\n') { sb_putc(&o, '\n'); continue; }          /* Alt+Enter */
            if (a == '[') { while (++i < n && !(b[i] >= 0x40 && b[i] <= 0x7E)) {} continue; }   /* CSI … final */
            if (a == 'O') { i++; continue; }                                        /* SS3 x */
            continue;                                                               /* Alt+letter: ignore */
        }
        if (c == 127 || c == 8) { if (o.len) { size_t p = o.len - 1; while (p > 0 && ((unsigned char)o.data[p] & 0xC0) == 0x80) p--; o.len = p; o.data[o.len] = 0; } continue; }
        if (c == 21) { sb_clear(&o); sb_append(&o, "", 0); continue; }             /* Ctrl-U */
        if (c == 23) {                                                              /* Ctrl-W */
            size_t p = o.len;
            while (p > 0 && (o.data[p-1] == ' ' || o.data[p-1] == '\n')) p--;
            while (p > 0 && o.data[p-1] != ' ' && o.data[p-1] != '\n') p--;
            o.len = p; o.data[o.len] = 0; continue;
        }
        if (c == '\r' || c == '\n') { if (c == '\r' && i + 1 < n && b[i+1] == '\n') i++; sb_putc(&o, '\n'); continue; }
        if (c < 32 && c != '\t') continue;
        sb_putc(&o, (char)c);
    }
    if (!trim) return sb_detach(&o);
    size_t st = 0; while (st < o.len && (o.data[st] == ' ' || o.data[st] == '\t' || o.data[st] == '\n')) st++;
    size_t en = o.len; while (en > st && (o.data[en-1] == ' ' || o.data[en-1] == '\t' || o.data[en-1] == '\n')) en--;
    char *r = xstrndup(o.data + st, en - st);
    sb_free(&o);
    return r;
}
char *term_keys_to_text(const unsigned char *b, size_t n) { return keys_text(b, n, true); }

/* The pending type-ahead as text, exactly as typed (a space at its end is where the next word
 * starts, which is what the suggestions go by); NULL when there is none. */
static char *ta_text(void) {
    if (g_ta_pos >= g_ta_len) return NULL;
    char *t = keys_text(g_ta + g_ta_pos, g_ta_len - g_ta_pos, false);
    if (!*t) { free(t); return NULL; }
    return t;
}
static void ta_set_text(const char *s) {
    g_ta_pos = g_ta_len = 0;
    for (; *s; s++) ta_put((unsigned char)*s);
}

/* What the field shows of it: from its first letter on; NULL when there is nothing to see. */
static char *ta_pending_text(void) {
    char *t = ta_text();
    size_t st = 0;
    while (t && (t[st] == ' ' || t[st] == '\t' || t[st] == '\n')) st++;
    if (!t || !t[st]) { free(t); return NULL; }
    memmove(t, t + st, strlen(t + st) + 1);
    return t;
}

static int g_ta_hist = -1;   /* ↑/↓ while busy: the history entry shown (g_hist_n: none, -1: not browsing) */
static char *g_ta_draft;     /* what was typed before the history browse (↓ to the end brings it back) */
/* Enter pressed while busy: the pending type-ahead becomes a queued message — unless it
 * is a slash command main.c can run right there and then (/status, /mode, …). */
static void ta_submit(void) {
    size_t n = g_ta_len - g_ta_pos;
    char *text = term_keys_to_text(g_ta + g_ta_pos, n);
    g_ta_pos = g_ta_len = 0;
    g_ta_hist = -1; free(g_ta_draft); g_ta_draft = NULL;   /* the history browse ends with the line */
    if (*text) {
        view_leave();   /* sending is the end of reading back: what the line does shows on the live screen */
        if (!(*text == '/' && term_run_while_busy && term_run_while_busy(text))) term_queue_push(text);
        term_status_refresh();
    }
    free(text);
}

/* Append a byte typed while busy; Enter (outside a paste, not Alt+Enter, not after
 * a trailing backslash) submits the pending text as a queued message. */
static int csi_key(const char *params, int fin);
static int mouse_key(int cb, int row, int col);
static void fold_click(void);
static void ta_act(int k) {
    int page = view_rows() - 1;
    switch (k) {
        case K_SHIFT_TAB:  g_cfg.mode = (g_cfg.mode + 1) % MODE_COUNT; term_status_refresh(); break;
        case K_WHEEL_UP:   view_scroll(-WHEEL_ROWS); break;
        case K_WHEEL_DOWN: view_scroll(WHEEL_ROWS); break;
        case K_PGUP:       view_scroll(page > 1 ? -page : -1); break;
        case K_PGDN:       view_scroll(page > 1 ? page : 1); break;
        case K_MOUSE:      fold_click(); break;
        default: break;
    }
}
static int g_ta_x10 = 0;             /* bytes of an old-style mouse report (ESC [ M b x y) still to come */
/* ↑/↓ while the suggestions are up are theirs (ta_sug_key): the bytes of the key that are pending
 * already, from `at` on, go — or stay, for the next prompt, when the list has no use for it. */
static bool ta_sug_arrow(int k, size_t at) {
    size_t whole = g_ta_len;
    g_ta_len = at;
    if (ta_sug_key(k)) return true;
    g_ta_len = whole;
    return false;
}
/* The pending type-ahead ends in a CSI sequence that is not finished yet: where its parameters
 * start (just after "ESC ["), or 0 when it does not. */
static size_t ta_csi(void) {
    size_t st = g_ta_len;
    while (st > g_ta_pos && g_ta[st - 1] >= 0x30 && g_ta[st - 1] <= 0x3F) st--;
    return st >= g_ta_pos + 2 && g_ta[st - 1] == '[' && g_ta[st - 2] == 27 ? st : 0;
}
static void ta_hist(int k);   /* ↑/↓ while busy: the queue first, then the history (below) */
static void ta_type(unsigned char c) {
    if (g_ta_x10) {   /* button, column, row — each offset by 32 */
        static unsigned char rep[3];
        rep[3 - g_ta_x10] = c;
        if (--g_ta_x10 == 0) ta_act(mouse_key(rep[0] - 32, rep[2] - 32, rep[1] - 32));
        return;
    }
    memmove(g_ta_tail, g_ta_tail + 1, sizeof g_ta_tail - 1); g_ta_tail[sizeof g_ta_tail - 1] = c;
    if (!memcmp(g_ta_tail, "\x1b[200~", 6)) g_ta_paste = true;
    else if (!memcmp(g_ta_tail, "\x1b[201~", 6)) g_ta_paste = false;
    if (c == '\r' && !g_ta_paste && g_cfg.interactive) {
        size_t n = g_ta_len - g_ta_pos;
        bool alt = n > 0 && g_ta[g_ta_len - 1] == 27;                          /* ESC CR = Alt+Enter */
        bool bs  = n > 0 && g_ta[g_ta_len - 1] == '\\';                        /* trailing \ = newline */
        if (!alt && !bs) { if (!ta_sug_key('\r')) ta_submit(); return; }   /* (a highlighted suggestion is taken instead) */
        if (bs) { g_ta_len--; c = '\n'; }
    }
    /* Keys that act at once instead of being replayed at the next prompt. Shift+Tab cycles the
     * permission mode, like in the editor: it applies to the tool confirmations still to come in
     * this turn. The wheel and PgUp/PgDn scroll the conversation — reading back is what one does
     * while the model works. Any other mouse report is dropped: replayed, it would be noise. */
    if (c >= 0x40 && c <= 0x7E && !g_ta_paste) {   /* this byte may end a CSI sequence */
        size_t st = ta_csi();
        if (st && g_ta_len - st < 24) {
            char par[24]; memcpy(par, g_ta + st, g_ta_len - st); par[g_ta_len - st] = 0;
            int k = csi_key(par, c);
            if (k == K_SHIFT_TAB || k == K_PGUP || k == K_PGDN || k == K_WHEEL_UP || k == K_WHEEL_DOWN || k == K_MOUSE || k == K_MOUSE_X10) {
                g_ta_len = st - 2;
                if (k == K_MOUSE_X10) g_ta_x10 = 3; else ta_act(k);
                return;
            }
            if ((k == K_UP || k == K_DOWN) && ta_sug_arrow(k, st - 2)) return;
            if (k == K_UP || k == K_DOWN) { g_ta_len = st - 2; ta_hist(k); return; }   /* the arrow is taken here */
        }
        if ((c == 'A' || c == 'B') && g_ta_len - g_ta_pos >= 2 && g_ta[g_ta_len - 1] == 'O' && g_ta[g_ta_len - 2] == 27) {
            if (ta_sug_arrow(c == 'A' ? K_UP : K_DOWN, g_ta_len - 2)) return;   /* (ESC O A: a terminal in application-cursor mode) */
            g_ta_len -= 2; ta_hist(c == 'A' ? K_UP : K_DOWN); return;
        }
    }
    if (!g_ta_paste && (c == 14 || c == 16) && ta_sug_key(c == 14 ? K_DOWN : K_UP)) return;
    if (!g_ta_paste && (c == 14 || c == 16)) { ta_hist(c == 14 ? K_DOWN : K_UP); return; }   /* Ctrl-N / Ctrl-P: as ↓ / ↑ */
    if (!g_ta_paste && c == '\t' && g_fs) { ta_sug_key(c); return; }   /* Tab completes, or does nothing: it is not text */
    ta_put(c);
    /* show it in the field — unless it is a byte of a key sequence still arriving (a wheel
     * report is a dozen of them), which is not text and changes nothing there */
    if (g_fs && g_cfg.interactive && (c >= 32 || c == 127 || c == 8 || c == 21 || c == 23 || c == '\n') && (g_ta_paste || !ta_csi())) term_status_refresh();
}

/* Give queued messages back to the user (after an interrupt): they reappear in the
 * editor, one per line, together with whatever was typed but not yet sent. */
void term_queue_to_editor(void) {
    if (!g_queue_n) return;
    ta_stash pending = ta_take();
    for (int i = 0; i < g_queue_n; i++) {
        if (i) ta_put('\n');
        for (const char *p = g_queue[i]; *p; p++) ta_put((unsigned char)*p);
    }
    term_queue_clear();
    if (pending.len) { ta_put('\n'); for (size_t i = 0; i < pending.len; i++) ta_put(pending.data[i]); }
    free(pending.data);
    term_status_refresh();
}

void term_editor_prefill(const char *text) {
    if (!text) return;
    if (g_ui_nc) { nc_prefill(text); return; }
    for (const char *p = text; *p; p++) ta_put((unsigned char)*p);
}

int term_interrupt_fd(void) { return g_ui_nc ? nc_input_fd() : STDIN_FILENO; }

int term_strip_sel(void) { return g_strip_sel; }
/* The strip for the notcurses screen (which draws it itself): refreshes it, and gives its lines and rows. */
int term_strip_update(const char **text) {
    strip_refresh();
    if (text) *text = g_strip.data ? g_strip.data : "";
    return g_strip_rows;
}
/* A key on the strip (STRIP_*; STRIP_OTHER returns to the field and is not taken).
 * may_enter: the field is empty, so ↓ goes into the strip. Returns whether the strip took the key. */
bool term_strip_key(int key, bool may_enter) {
    if (!g_strip_rows) return false;
    if (g_strip_sel < 0) { if (key == STRIP_DOWN && may_enter) { g_strip_sel = 0; return true; } return false; }
    if (key == STRIP_DOWN) { if (g_strip_sel < g_strip_rows - 1) g_strip_sel++; return true; }
    if (key == STRIP_UP) { g_strip_sel--; return true; }
    if (key == STRIP_ESC) { g_strip_sel = -1; return true; }
    if (key == STRIP_ENTER) { if (term_agents_open) term_agents_open(g_strip_sel); return true; }
    g_strip_sel = -1;
    return false;
}

/* While a turn runs, the agents strip takes the keys that are on it: ↓ from an empty type-ahead goes into it,
 * ↑/↓ move, Enter opens the log picked, Esc returns to the field. Returns the bytes it took (0: none). */
static ssize_t busy_strip_key(const unsigned char *kb, ssize_t k, ssize_t i) {
    if (!g_strip_rows) return 0;
    int code = 0; ssize_t used = 0;
    if (kb[i] == 27 && i + 2 < k && (kb[i + 1] == '[' || kb[i + 1] == 'O') && (kb[i + 2] == 'A' || kb[i + 2] == 'B')) { code = kb[i + 2] == 'A' ? K_UP : K_DOWN; used = 3; }
    else if (kb[i] == 27 && i == k - 1) { code = K_ESC; used = 1; }
    else if (kb[i] == '\r' || kb[i] == '\n') { code = '\r'; used = 1; }
    if (!code) return 0;
    if (g_strip_sel < 0) {
        if (code == K_DOWN && g_ta_len == 0) { g_strip_sel = 0; term_status_refresh(); return used; }
        return 0;
    }
    if (code == K_DOWN) { if (g_strip_sel < g_strip_rows - 1) g_strip_sel++; term_status_refresh(); return used; }
    if (code == K_UP) { g_strip_sel--; term_status_refresh(); return used; }
    if (code == K_ESC) { g_strip_sel = -1; term_status_refresh(); return used; }
    if (code == '\r') { if (term_agents_open) term_agents_open(g_strip_sel); term_status_refresh(); return used; }
    return 0;
}

int term_poll_interrupt(void) {
    if (g_ui_nc) return nc_poll_interrupt();
    unsigned char kb[256];
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
        struct timeval tv = { 0, 0 };
        if (select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv) <= 0) return 0;
        ssize_t k = in_read(kb, sizeof kb);
        if (k <= 0) return 0;
        for (ssize_t i = 0; i < k; i++) {
            if (kb[i] == 3) { term_overlay_close(); view_leave(); return 1; }     /* Ctrl-C */
            { ssize_t took = busy_strip_key(kb, k, i); if (took) { i += took - 1; continue; } }
            if (kb[i] == 27 && i == k - 1 && !g_ta_x10) {   /* bare Esc, or a split escape sequence? */
                fd_set rf2; FD_ZERO(&rf2); FD_SET(STDIN_FILENO, &rf2);
                struct timeval tv2 = { 0, 40 * 1000 };
                if (select(STDIN_FILENO + 1, &rf2, NULL, NULL, &tv2) <= 0) {
                    /* Scrolled back, Esc is "back to the output", as it is in the viewer at the
                     * prompt — it takes a second one to stop the model. */
                    if (g_ov_text) { term_overlay_close(); continue; }   /* the overlay goes first, like the viewer */
                    if (g_view_top >= 0) { view_leave(); continue; }
                    if (g_strip_sel >= 0) { g_strip_sel = -1; term_status_refresh(); continue; }   /* the strip first: Esc back to the field */
                    if (ta_sug_key(K_ESC)) continue;   /* and with the suggestions up, it closes those */
                    return 1;
                }
                ta_type(kb[i]);   /* more bytes follow: it is a sequence, keep it */
                continue;
            }
            ta_type(kb[i]);
        }
    }
}

/* Idle work at the prompt (see common.h): main.c sets these before each term_readline(). */
void (*term_idle_hook)(void) = NULL;
void (*term_agents_menu)(void) = NULL;
int term_idle_ms = 0;

static int read_byte_timeout(int ms) {
    int t = ta_get(); if (t >= 0) return t;
    fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv);
    if (r <= 0) return -1;
    unsigned char c;
    if (in_read(&c, 1) != 1) return -1;
    return c;
}

/* A mouse report as a key: button code and row (1-based). Only the wheel means anything here,
 * and only over the conversation — over the input field and the bar it does nothing, their keys
 * are the keyboard's. Clicks, releases and drags are reported too (there is no wheel-only mode). */
static int mouse_key(int cb, int row, int col) {
    if ((cb & 0xE3) == 0 && g_banner_on && row == g_bn_xr && col >= g_bn_xc0 && col <= g_bn_xc1) return K_BANNER_X;   /* the banner's close button */
    g_click_row = (cb & 0xE3) == 0 && g_fs && row >= 1 && row <= fs_region() ? row : 0;   /* the left button, pressed, not dragged: whoever reads the key may act on it (fold_click) */
    g_click_frow = (cb & 0xE3) == 0 && g_fs && row > fs_region() ? row : 0; g_click_fcol = col;   /* ... or in the field below it (paste_click) */
    if ((cb & 0xC0) != 64 || (cb & 2)) return K_MOUSE;   /* not the vertical wheel */
    if (g_fs && row > fs_region()) return K_MOUSE;
    return (cb & 1) ? K_WHEEL_DOWN : K_WHEEL_UP;
}

/* What a CSI sequence is as a key: `params` is what came between "ESC [" and the final byte. */
static int csi_key(const char *params, int fin) {
    if (params[0] == '<') {   /* SGR mouse report: <button;column;row, M = pressed, m = released */
        int cb = 0, x = 0, y = 0;
        if ((fin != 'M' && fin != 'm') || sscanf(params + 1, "%d;%d;%d", &cb, &x, &y) != 3) return K_ESC;
        if (fin == 'm') { g_click_row = g_click_frow = 0; return K_MOUSE; }
        return mouse_key(cb, y, x);
    }
    switch (fin) {
        case 'M': return params[0] ? K_ESC : K_MOUSE_X10;   /* a terminal without SGR reports */
        case 'Z': return K_SHIFT_TAB;
        case 'A': return K_UP; case 'B': return K_DOWN;
        case 'C': return !strcmp(params, "1;5") ? K_CTRL_RIGHT : K_RIGHT;
        case 'D': return !strcmp(params, "1;5") ? K_CTRL_LEFT : K_LEFT;
        case 'H': return K_HOME; case 'F': return K_END;
        case '~':
            if (!strcmp(params, "1") || !strcmp(params, "7")) return K_HOME;
            if (!strcmp(params, "4") || !strcmp(params, "8")) return K_END;
            if (!strcmp(params, "3")) return K_DEL;
            if (!strcmp(params, "5")) return K_PGUP;
            if (!strcmp(params, "6")) return K_PGDN;
            if (!strcmp(params, "200")) return K_PASTE_START;
            if (!strcmp(params, "201")) return K_PASTE_END;
            return K_ESC;
        default: return K_ESC;
    }
}

/* Wait for a byte on stdin: 1 = there is one, -2 = the window was resized meanwhile, -1 = error.
 * Not a blocking read(): signal() makes the kernel restart a read() after the handler ran, so a
 * resize at an idle prompt went unnoticed until the next key — and under tmux, where panes are
 * split and zoomed all the time, the input field and the bar stayed where the old size had them.
 * SIGWINCH is held back around the check and let in by pselect() itself, so one that arrives
 * between the two is not lost either. */
static int stdin_wait(void) {
    sigset_t winch, old;
    sigemptyset(&winch); sigaddset(&winch, SIGWINCH);
    sigprocmask(SIG_BLOCK, &winch, &old);
    int r = 1;
    for (;;) {
        if (g_winch) { g_winch = 0; r = -2; break; }
        fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
        int k = pselect(STDIN_FILENO + 1, &rf, NULL, NULL, NULL, &old);
        if (k > 0) break;
        if (k < 0 && errno != EINTR) { r = -1; break; }
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return r;
}

/* Reads one key. Printable bytes returned as-is (0..255); specials as K_*.
 * Returns -1 on EOF/error, -2 when the window was resized (the caller redraws). */
static int read_key(void) {
    unsigned char c;
    int t = ta_get();
    if (t >= 0) c = (unsigned char)t;
    else {
        int w = stdin_wait();
        if (w < 0) return w;
        if (in_read(&c, 1) != 1) return -1;
    }
    if (c != 27) return c;
    int a = read_byte_timeout(40);
    if (a < 0) return K_ESC;
    if (a == '\r' || a == '\n') return K_ALT_ENTER;
    if (a == 'b') return K_ALT_B;
    if (a == 'f') return K_ALT_F;
    if (a == 127 || a == 8) return K_ALT_BS;
    if (a == 'O') {
        int b = read_byte_timeout(40);
        switch (b) { case 'H': return K_HOME; case 'F': return K_END; case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT; }
        return K_ESC;
    }
    if (a != '[') return K_ESC;
    /* CSI: collect params */
    char params[24] = {0}; int pl = 0; int b;
    for (;;) {
        b = read_byte_timeout(40);
        if (b < 0) return K_ESC;
        if ((b >= '0' && b <= '9') || b == ';' || (b == '<' && pl == 0)) { if (pl < (int)sizeof params - 1) params[pl++] = (char)b; continue; }
        break;
    }
    int k = csi_key(params, b);
    if (k == K_MOUSE_X10) {   /* button, column, row follow, each offset by 32 */
        int cb = read_byte_timeout(40), x = read_byte_timeout(40), y = read_byte_timeout(40);
        return y < 0 ? K_ESC : mouse_key(cb - 32, y - 32, x - 32);
    }
    return k;
}

/* read_key(), but gives up after `ms` without a byte: -3 = nothing was typed. */
#define K_IDLE (-3)
/* ---------- the banner's tick ----------
 * While the top-level prompt waits (term_readline) a tick runs every TICK_MS: the banner's modal is
 * redrawn with its next frame (main.c: banner_tick, term_banner_draw). The tick gets still=true once, when
 * the prompt returns, so the modal settles on its last frame. */
#define TICK_MS 100
static void (*g_tick_fn)(bool still) = NULL;
static bool g_tick_done = false;            /* the first task has stopped the ticks for good: no tick is set again */
static bool g_tick_live = false;
static long g_tick_next = 0;                   /* monotonic ms of the next tick */
static long mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }
static int tick_wait_ms(void) {
    if (!g_tick_fn || !g_tick_live) return -1;
    long left = g_tick_next - mono_ms();
    return left < 0 ? 0 : (int)left;
}
void term_tick_set(void (*fn)(bool still)) { if (!g_tick_done) g_tick_fn = fn; }
int  term_tick_wait(void) { return tick_wait_ms(); }
void term_tick_run(void) {
    if (!g_tick_fn || !g_tick_live || tick_wait_ms() > 0) return;
    g_tick_fn(false);
    g_tick_next = mono_ms() + TICK_MS;
}
static int read_key_wait(int ms);
/* A key at the prompt. A tick (when one runs) cuts the wait short; the idle work, when armed, is due
 * term_idle_ms after the prompt was last touched, whatever ticks come meanwhile (a deadline, not a wait).
 * Returns K_IDLE only for the idle work. */
static int read_key_prompt(bool idle) {
    long deadline = idle && term_idle_ms > 0 ? mono_ms() + term_idle_ms : -1;
    for (;;) {
        int wait = tick_wait_ms();
        if (deadline >= 0) {
            long left = deadline - mono_ms(); if (left < 0) left = 0;
            if (wait < 0 || left < wait) wait = (int)left;
        }
        if (wait < 0) return read_key();
        int k = read_key_wait(wait);
        if (k != K_IDLE) return k;
        term_tick_run();
        if (deadline >= 0 && mono_ms() >= deadline) return K_IDLE;
    }
}

static int read_key_wait(int ms) {
    if (g_ta_pos >= g_ta_len) {   /* type-ahead counts as typed */
        fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        int r = select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv);
        if (r == 0) return K_IDLE;
        if (r < 0) { if (errno != EINTR) return -1; g_winch = 0; return -2; }
    }
    return read_key();
}

/* The viewer at the prompt: here the keyboard scrolls too, until the window is back at the last
 * row. `delta` is the first move (0 = carry on with a window that is already up, from the turn
 * that just ended). End, Esc and Enter go back to the prompt; so does anything typed, and that
 * key is the message's — it is handed on to the editor instead of being swallowed. */
static void scroll_view_impl(int delta);
static void scroll_view(int delta) { park_hold(); scroll_view_impl(delta); park_free(); }
static void scroll_view_impl(int delta) {
    if (!g_fs || !g_sb_active) return;
    ta_stash stash = ta_take();   /* what is still to be replayed into the editor is not for the viewer */
    int typed = -1;
    if (delta) view_scroll(delta); else if (g_view_top >= 0) view_paint();
    while (g_view_top >= 0) {
        int k = read_key();
        if (k == -2) { view_paint(); continue; }
        if (k == K_MOUSE) { fold_click(); continue; }
        int page = view_rows() - 1; if (page < 1) page = 1;
        if (k == K_PGUP) view_scroll(-page);
        else if (k == K_PGDN || k == ' ') view_scroll(page);
        else if (k == K_UP || k == 'k') view_scroll(-1);
        else if (k == K_DOWN || k == 'j') view_scroll(1);
        else if (k == K_WHEEL_UP) view_scroll(-WHEEL_ROWS);
        else if (k == K_WHEEL_DOWN) view_scroll(WHEEL_ROWS);
        else if (k == K_HOME || k == 'g') view_scroll(-(1 << 24));
        else { view_leave(); if (k > 32 && k < 256 && k != 'q' && k != 127) typed = k; }
    }
    ta_restore(stash);
    if (typed >= 0) ta_put((unsigned char)typed);
}

int term_getkey(void) {
    term_raw(true);
    int k;
    do { k = read_key(); } while (k == -2);
    return k;
}

/* ---------- history ---------- */
#define HIST_MAX 100   /* the latest 100 queries are kept (and persisted) */
/* The file is shared: several corbienest sessions may have it open at once, each with its own
 * window of the last HIST_MAX queries. Rewriting it from that window would throw away whatever
 * the other sessions typed since we started, so the file is only ever *appended* to, and folded
 * back down to HIST_MAX entries once it has grown well past them. */
#define HIST_FILE_MAX (64 * 1024)   /* bytes; ~1000 queries, so folding is rare */
static char *g_hist[HIST_MAX];
static int g_hist_n = 0;
/* ↑/↓ while busy: the history the type-ahead shows (g_ta_hist = g_hist_n: none, -1: not browsing since the
 * last line), and what was typed before browsing (g_ta_draft), which ↓ back to the end brings back. */
static void ta_hist(int k) {
    if (k == K_UP && term_queue_count()) { term_queue_to_editor(); term_status_refresh(); return; }   /* the queue comes back first */
    if (g_ta_hist < 0) g_ta_hist = g_hist_n;
    if (k == K_UP) {
        if (g_ta_hist == 0) return;
        if (g_ta_hist == g_hist_n) { free(g_ta_draft); g_ta_draft = ta_text(); }
        ta_set_text(g_hist[--g_ta_hist]);
    } else if (g_ta_hist < g_hist_n) {
        if (++g_ta_hist == g_hist_n) ta_set_text(g_ta_draft ? g_ta_draft : "");
        else ta_set_text(g_hist[g_ta_hist]);
    }
    term_status_refresh();
}
static int g_hist_pending = 0;   /* entries added since the last save: the tail of g_hist */

int hist_count(void) { return g_hist_n; }
const char *hist_get(int i) { return i >= 0 && i < g_hist_n ? g_hist[i] : NULL; }

static const char *hist_path(void) {
    static char p[1200];
    if (!p[0]) snprintf(p, sizeof p, "%s/history", config_dir());
    return p;
}

void hist_add(const char *line) {
    if (!line || !*line) return;
    if (g_hist_n && !strcmp(g_hist[g_hist_n - 1], line)) return;
    if (g_hist_n == HIST_MAX) { free(g_hist[0]); memmove(g_hist, g_hist + 1, sizeof(char*) * (HIST_MAX - 1)); g_hist_n--; }
    g_hist[g_hist_n++] = xstrdup(line);
    if (g_hist_pending < g_hist_n) g_hist_pending++;
}

void hist_load(void) {
    size_t n; char *d = read_whole_file(hist_path(), &n, 4 << 20);
    if (!d) return;
    /* entries separated by \n; embedded newlines stored as \x1f */
    char *save = NULL;
    for (char *l = strtok_r(d, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        for (char *c = l; *c; c++) if (*c == 0x1f) *c = '\n';
        hist_add(l);
    }
    free(d);
    g_hist_pending = 0;   /* what is already in the file is not ours to append again */
}

/* Append-only means the file grows. Once it is well past the window, fold it back to the last
 * HIST_MAX entries — of the *file*, not of our own window, so the other sessions' queries are
 * kept too. The swap is atomic; a session appending in the same instant loses at worst those
 * few entries, which is why the threshold is high enough that this almost never happens. */
static void hist_compact(void) {
    struct stat st;
    if (stat(hist_path(), &st) != 0 || st.st_size <= HIST_FILE_MAX) return;
    /* read it whole: a cap here would fold the *oldest* part of the file back and throw the
       newest entries away, which is the opposite of what this is for */
    size_t n; char *d = read_whole_file(hist_path(), &n, (size_t)st.st_size + 1);
    if (!d) return;
    int lines = 0;
    for (size_t i = 0; i < n; i++) if (d[i] == '\n') lines++;
    size_t start = 0;
    for (size_t i = 0; i < n && lines > HIST_MAX; i++) if (d[i] == '\n') { lines--; start = i + 1; }
    write_whole_file_atomic(hist_path(), d + start, n - start);
    free(d);
}

void hist_save(void) {
    if (g_hist_pending <= 0) return;
    sbuf b; sb_init(&b);
    for (int i = g_hist_n - g_hist_pending; i < g_hist_n; i++) {
        for (const char *c = g_hist[i]; *c; c++) sb_putc(&b, *c == '\n' ? 0x1f : *c);
        sb_putc(&b, '\n');
    }
    /* O_APPEND: every write lands at the end of the file as it is *now*, so it goes after
     * whatever another session wrote in the meantime instead of on top of it. */
    int fd = open(hist_path(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) { sb_free(&b); return; }
    for (size_t off = 0; off < b.len; ) {
        ssize_t k = write(fd, b.data + off, b.len - off);
        if (k < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)k;
    }
    close(fd);
    sb_free(&b);
    g_hist_pending = 0;
    hist_compact();
}

/* ---------- UTF-8 helpers ---------- */
static int u8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 6) return 2;
    if ((c >> 4) == 14) return 3;
    if ((c >> 3) == 30) return 4;
    return 1;
}
static size_t u8_prev(const char *s, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}
static size_t u8_next(const char *s, size_t pos, size_t len) {
    if (pos >= len) return len;
    size_t n = pos + (size_t)u8_len((unsigned char)s[pos]);
    return n > len ? len : n;
}
static int vis_width(const char *s) {   /* width of prompt ignoring escapes */
    int w = 0;
    for (const char *p = s; *p; ) {
        if (*p == 27) { while (*p && *p != 'm') p++; if (*p) p++; continue; }
        if (((unsigned char)*p & 0xC0) != 0x80) w++;
        p++;
    }
    return w;
}

/* ---------- editor ----------
 * In full-screen mode the main prompt lives in the input field at the bottom of the
 * screen (in_field): the editor only hands its buffer to the field and lets
 * term_status_refresh() draw it, and it keeps the terminal cursor there. Conversation
 * output from inside the editor — completion candidates, hints, the submitted line —
 * goes through ed_conv_out(), which jumps back to where the transcript ended.
 * term_ask_line() (a question inside a turn) keeps drawing inline, as before. */
typedef struct {
    sbuf buf;
    size_t cur;          /* byte offset of cursor */
    const char *prompt;
    int plen;
    bool in_field;       /* drawn in the bottom input field rather than inline */
    int prev_cursor_row; /* row (relative to first line) where cursor was after last refresh (inline only) */
    int hist_idx;        /* g_hist_n == not browsing */
    char *hist_saved;    /* current line saved when browsing */
    bool sug_off;        /* no suggestions until the text is edited again (it was recalled, or Esc closed them) */
} editor;

/* compute (row,col) at byte offset `upto`, walking from prompt. Returns via out params. */
static void ed_pos(editor *e, size_t upto, int width, int *row, int *col) {
    wrap_pos(e->buf.data ? e->buf.data : "", upto < e->buf.len ? upto : e->buf.len, width, e->plen, row, col);
}

/* Print conversation content while the editor owns the cursor in the field. */
static void ed_conv_out(editor *e, const char *text) {
    if (!e->in_field) { fputs(text, stdout); return; }
    sb_pause(true);
    printf("\x1b[%d;%dH", g_conv_row, g_conv_col);
    fflush(stdout);
    sb_pause(false);
    fputs(text, stdout);
    fflush(stdout);
    conv_pos(fs_region(), &g_conv_row, &g_conv_col);
}

static void ed_refresh(editor *e);

/* Run term_idle_hook() with the terminal in the state a turn runs in: the field released and
 * the cursor back where the transcript ended, so the hook's own output (and the status bar it
 * refreshes) lands like any other conversation output. The prompt is redrawn afterwards. */
static void ed_hook_run(editor *e, void (*hook)(void)) {
    if (e->in_field) {
        g_field_focus = false;
        g_field_text = ""; g_field_len = g_field_cur = 0; g_field_view = 0;
        sb_pause(true);
        printf("\x1b[%d;%dH", g_conv_row, g_conv_col);
        fflush(stdout);
        sb_pause(false);
        term_status_refresh();
        hook();
        sug_clear();   /* (the type-ahead's, for what was typed meanwhile: those keys come to the editor now) */
        conv_pos(fs_region(), &g_conv_row, &g_conv_col);
        layout_sync();
        g_field_focus = true; g_field_view = 0;
    } else {
        fputs("\r\x1b[J", stdout); fflush(stdout);   /* wipe the prompt line; it is redrawn below */
        e->prev_cursor_row = 0;
        hook();
    }
    ed_refresh(e);
}
static void ed_idle_run(editor *e) {
    if (term_idle_hook) ed_hook_run(e, term_idle_hook);
}

static void ed_refresh(editor *e) {
    if (e->in_field) {
        g_field_text = e->buf.data ? e->buf.data : "";
        g_field_len = e->buf.len;
        g_field_cur = e->cur;
        term_status_refresh();
        return;
    }
    if (layout_sync()) e->prev_cursor_row = 0;   /* repainted: the cursor is back where the prompt starts */
    int width = term_width();
    sbuf o; sb_init(&o);
    sb_puts(&o, "\r");
    if (e->prev_cursor_row > 0) sb_printf(&o, "\x1b[%dA", e->prev_cursor_row);
    sb_puts(&o, "\x1b[J");
    sb_puts(&o, e->prompt);
    if (e->buf.len) sb_append(&o, e->buf.data, e->buf.len);
    sb_puts(&o, C_RESET);
    int erow, ecol; ed_pos(e, e->buf.len, width, &erow, &ecol);
    /* pending-wrap fix: if we ended exactly at column 0 due to wrapping, force newline */
    if (ecol == 0 && erow > 0 && e->buf.len && e->buf.data[e->buf.len - 1] != '\n') sb_puts(&o, "\n");
    else if (ecol == 0 && erow > 0 && e->buf.len == 0) sb_puts(&o, "\n");
    /* move the cursor back up into the text */
    int crow, ccol; ed_pos(e, e->cur, width, &crow, &ccol);
    if (erow - crow > 0) sb_printf(&o, "\x1b[%dA", erow - crow);
    sb_puts(&o, "\r");
    if (ccol > 0) sb_printf(&o, "\x1b[%dC", ccol);
    e->prev_cursor_row = crow;
    chrome_append(&o);   /* the "\x1b[J" above wiped the status bar; the mode may have changed too */
    sb_pause(true);   /* the live editor is not conversation content (sb_note adds the submitted line) */
    fwrite(o.data, 1, o.len, stdout);
    fflush(stdout);
    sb_pause(false);
    sb_free(&o);
}

static void ed_insert(editor *e, const char *s, size_t n) {
    sbuf *b = &e->buf;
    sb_append(b, "", 0);   /* ensure alloc */
    if (b->len + n + 1 > b->cap) { size_t nc = (b->len + n + 1) * 2; b->data = xrealloc(b->data, nc); b->cap = nc; }
    memmove(b->data + e->cur + n, b->data + e->cur, b->len - e->cur);
    memcpy(b->data + e->cur, s, n);
    b->len += n; b->data[b->len] = 0; e->cur += n;
}
static void ed_delete_range(editor *e, size_t from, size_t to) {
    if (to > e->buf.len) to = e->buf.len;
    if (from >= to) return;
    memmove(e->buf.data + from, e->buf.data + to, e->buf.len - to);
    e->buf.len -= (to - from); e->buf.data[e->buf.len] = 0;
    if (e->cur > to) e->cur -= (to - from); else if (e->cur > from) e->cur = from;
}
static bool is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || ((unsigned char)c >= 0x80); }
static size_t word_left(editor *e) {
    size_t p = e->cur;
    while (p > 0 && !is_word(e->buf.data[p-1])) p--;
    while (p > 0 && is_word(e->buf.data[p-1])) p--;
    return p;
}
static size_t word_right(editor *e) {
    size_t p = e->cur, n = e->buf.len;
    while (p < n && !is_word(e->buf.data[p])) p++;
    while (p < n && is_word(e->buf.data[p])) p++;
    return p;
}
static void ed_set(editor *e, const char *s) {
    sb_clear(&e->buf); sb_puts(&e->buf, s); e->cur = e->buf.len;
}

/* ---------- suggestions ----------
 * While a "/command", an "@file" or a "!line" is being typed, term_suggest (main.c) says what
 * the word at the cursor could become, and the list is shown under the input field, narrowing
 * with every letter. It is a preview until it is asked for: Enter still sends what was typed and
 * ↑ still walks the history. Tab completes — the one candidate, else as far as they all agree,
 * else it starts choosing — and ↓ steps into the list; with a row highlighted ↑/↓ move, Enter or
 * Tab take it, Esc closes. The same goes for what is typed while the model works (ta_sug_sync). */
int (*term_suggest)(const char *buf, size_t cur, size_t *from, term_sug **items) = NULL;

static void sug_clear(void) {
    for (int i = 0; i < g_sug_n; i++) { free(g_sug[i].text); free(g_sug[i].desc); }
    free(g_sug); g_sug = NULL; g_sug_n = 0; g_sug_sel = -1; g_sug_top = 0;
    free(g_ta_sug_for); g_ta_sug_for = NULL;   /* whoever fills the list next says whose it is */
}

/* Append `s` (plain text) in at most `w` columns, cut on a character boundary with an … — at
 * the end, or at the front when it is the tail that tells one entry from the next (a path). */
static void put_clipped(sbuf *o, const char *s, int w, bool keep_tail) {
    int sw = vis_width(s);
    if (w < 1) return;
    if (sw <= w) { sb_puts(o, s); return; }
    if (keep_tail) { sb_puts(o, "…"); sb_puts(o, s + vis_offset(s, strlen(s), sw - (w - 1))); }
    else { sb_append(o, s, vis_offset(s, strlen(s), w - 1)); sb_puts(o, "…"); }
}

/* Draw the list on its rows of the chrome, from `row` down: the candidates, then a line that
 * says which keys do what. Absolute moves, like the rest of the chrome. */
static void sug_draw(sbuf *o, int row) {
    if (g_sug_rows < 2) return;
    int show = g_sug_rows - 1, cols = g_fs_cols;
    if (g_sug_sel >= 0 && g_sug_sel < g_sug_top) g_sug_top = g_sug_sel;
    if (g_sug_sel >= g_sug_top + show) g_sug_top = g_sug_sel - show + 1;
    if (g_sug_top > g_sug_n - show) g_sug_top = g_sug_n - show;
    if (g_sug_top < 0) g_sug_top = 0;
    int namew = 0; bool descs = false;
    for (int i = g_sug_top; i < g_sug_n && i < g_sug_top + show; i++) {
        int w = vis_width(g_sug[i].text);
        if (w > namew) namew = w;
        if (g_sug[i].desc && *g_sug[i].desc) descs = true;
    }
    int room = cols - 3;   /* the marker's two columns, and never the last one */
    if (descs && namew > room / 2) namew = room / 2 > 12 ? room / 2 : 12;
    if (namew > room) namew = room;
    int hint_row = row + (g_sug_n - g_sug_top < show ? g_sug_n - g_sug_top : show);   /* right under the last one */
    for (int i = 0; i <= show; i++) {
        int idx = g_sug_top + i;
        sb_printf(o, "\x1b[%d;1H\x1b[K", row + i);
        if (idx >= g_sug_n || i == show) continue;
        bool sel = idx == g_sug_sel;
        sb_puts(o, sel ? C_ORANGE "❯ " C_BOLD : "  ");
        put_clipped(o, g_sug[idx].text, namew, true);
        sb_puts(o, C_RESET);
        int droom = room - namew - 2;
        if (g_sug[idx].desc && *g_sug[idx].desc && droom > 3) {
            for (int k = vis_width(g_sug[idx].text); k < namew + 2; k++) sb_putc(o, ' ');
            sb_puts(o, C_DIM); put_clipped(o, g_sug[idx].desc, droom, false); sb_puts(o, C_RESET);
        }
    }
    if (!g_sug_n) return;   /* (its rows are still held: given back at the next field_sync_rows) */
    char hint[160], count[48] = "";
    if (g_sug_n > show) snprintf(count, sizeof count, " · %d-%d of %d", g_sug_top + 1, g_sug_top + show, g_sug_n);
    snprintf(hint, sizeof hint, "  %s%s", g_sug_sel >= 0 ? "enter take it · ↑/↓ move · esc close" : "tab complete · ↓ choose · esc close", count);
    sb_printf(o, "\x1b[%d;1H" C_GRAY, hint_row);
    put_clipped(o, hint, cols - 1, false);
    sb_puts(o, C_RESET);
}

/* What could the word at the cursor become? */
static void sug_fill(const char *buf, size_t cur) {
    sug_clear();
    if (!term_suggest) return;
    term_sug *items = NULL; size_t from = 0;
    int n = term_suggest(buf, cur, &from, &items);
    if (n > 0 && items && from <= cur) { g_sug = items; g_sug_n = n; g_sug_from = from; }
}
static void sug_update(editor *e) {
    if (e->in_field && !e->sug_off) sug_fill(e->buf.data, e->cur); else sug_clear();
}

/* Put suggestion `i` in place of the word at the cursor. */
static void sug_accept(editor *e, int i) {
    if (i < 0 || i >= g_sug_n) return;
    size_t end = e->cur;   /* the rest of the word goes too: the cursor may stand inside it */
    while (end < e->buf.len && e->buf.data[end] != ' ' && e->buf.data[end] != '\n' && e->buf.data[end] != '\t') end++;
    ed_delete_range(e, g_sug_from, end);
    e->cur = g_sug_from;
    ed_insert(e, g_sug[i].text, strlen(g_sug[i].text));
    if (g_sug[i].open) return;   /* a directory: what is in it comes next */
    if (e->cur < e->buf.len && e->buf.data[e->cur] == ' ') e->cur++; else ed_insert(e, " ", 1);
}

/* Tab. Returns true when the text changed (the list is then worked out again). */
static bool sug_tab(editor *e) {
    if (g_sug_sel >= 0 || g_sug_n == 1) { sug_accept(e, g_sug_sel >= 0 ? g_sug_sel : 0); return true; }
    const char *tok = e->buf.data + g_sug_from; size_t tl = e->cur - g_sug_from;
    size_t cp = strlen(g_sug[0].text);   /* what all candidates start with, if they start with what was typed */
    for (int i = 0; i < g_sug_n && cp > tl; i++) {
        if (strncmp(g_sug[i].text, tok, tl)) { cp = 0; break; }
        size_t k = 0; while (k < cp && g_sug[i].text[k] == g_sug[0].text[k]) k++;
        cp = k;
    }
    if (cp <= tl) { g_sug_sel = 0; return false; }   /* nothing more they agree on: start choosing */
    char *pre = xstrndup(g_sug[0].text, cp);
    ed_delete_range(e, g_sug_from, e->cur);
    e->cur = g_sug_from;
    ed_insert(e, pre, cp);
    free(pre);
    return true;
}

/* The same list while the model works. There the text is the pending type-ahead and the cursor
 * its end, and term_status_refresh() calls this, so the list follows whatever happens to that
 * text — a key, a line sent, a question that sets it aside. It is worked out once per change,
 * not per refresh (the spinner alone asks ten times a second), and it stays up through the
 * moments between two calls when nothing is "busy". With the editor in the field the list is
 * the editor's own. */
static void ta_sug_sync(void) {
    if (!g_fs || g_field_focus || g_ta_paste) return;
    char *t = ta_text();
    if (!t && !g_ta_sug_for) return;
    if (t && g_ta_sug_for && !strcmp(t, g_ta_sug_for)) { free(t); return; }
    if (t) sug_fill(t, strlen(t)); else sug_clear();
    g_ta_sug_for = t;
}

/* Its keys there act at once, as Shift+Tab does: replayed at the next prompt they would come too
 * late to choose anything. They are the editor's — Tab completes, ↓ steps into the list, ↑/↓ move,
 * Enter takes the highlighted row (with none it sends the line, as ever) — and Esc closes the list:
 * it is the Esc after that one which stops the model. False for a key the list has no use for. */
static bool ta_sug_key(int k) {
    ta_sug_sync();   /* (a newline typed with Alt+Enter is in the text without a refresh having seen it) */
    if (!g_ta_sug_for || g_field_focus) return false;
    editor e; memset(&e, 0, sizeof e);   /* the text as an editor's, for sug_tab() and sug_accept() */
    sb_init(&e.buf); sb_puts(&e.buf, g_ta_sug_for); e.cur = e.buf.len;
    if (k == '\t' && !g_sug_n) { sug_fill(e.buf.data, e.cur); g_ta_sug_for = xstrdup(e.buf.data); }   /* closed: Tab asks again */
    bool taken = true, edited = false;
    if (!g_sug_n) taken = false;   /* nothing to choose from */
    else if (k == K_DOWN) { if (g_sug_sel < g_sug_n - 1) g_sug_sel++; }
    else if (k == K_UP && g_sug_sel >= 0) g_sug_sel--;
    else if (k == '\t') edited = sug_tab(&e);
    else if (k == '\r' && g_sug_sel >= 0) { sug_accept(&e, g_sug_sel); edited = true; }
    else if (k == K_ESC) { sug_clear(); g_ta_sug_for = xstrdup(e.buf.data); }   /* closed until the text changes */
    else taken = false;
    if (edited) ta_set_text(e.buf.data);
    sb_free(&e.buf);
    if (taken) term_status_refresh();
    return taken;
}

/* Ctrl-R: incremental reverse search through the history, like bash/fish. The prompt
 * turns into (reverse-i-search)`query`: and the newest matching entry is shown; typing
 * refines, Ctrl-R again finds the next older match, Enter/→/End keeps the match in the
 * editor, Esc/Ctrl-G/Ctrl-C restores what was typed before. */
static bool ci_contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) if (!strncasecmp(p, needle, n)) return true;
    return false;
}
static void ed_hist_search(editor *e) {
    char query[128] = {0}; size_t ql = 0;
    char *orig = xstrndup(e->buf.data, e->buf.len);
    const char *saved_prompt = e->prompt; int saved_plen = e->plen;
    char pbuf[200];
    int idx = g_hist_n;          /* current match, g_hist_n = none */
    bool keep = false;
    for (;;) {
        /* newest match at or below `idx` (when the query changed, from the newest entry again) */
        snprintf(pbuf, sizeof pbuf, C_DIM "(reverse-i-search)" C_RESET "`%s': ", query);
        e->prompt = pbuf; e->plen = vis_width(pbuf);
        if (e->in_field) g_field_prompt = pbuf;
        if (idx >= 0 && idx < g_hist_n) ed_set(e, g_hist[idx]); else if (ql) ed_set(e, ""); else ed_set(e, orig);
        ed_refresh(e);
        int k = read_key();
        if (k == -2) continue;
        if (k == -1 || k == 3 || k == 7 || k == K_ESC) { ed_set(e, orig); break; }          /* cancel */
        if (k == '\r' || k == '\n' || k == K_RIGHT || k == K_LEFT || k == K_END || k == K_HOME || k == '\t') { keep = true; break; }
        if (k == 18) {                                                                    /* next older */
            int i = (idx < g_hist_n ? idx : g_hist_n) - 1;
            while (i >= 0 && !ci_contains(g_hist[i], query)) i--;
            if (i >= 0) idx = i;
            continue;
        }
        if (k == 127 || k == 8) { if (ql) { ql--; while (ql && ((unsigned char)query[ql] & 0xC0) == 0x80) ql--; query[ql] = 0; } }
        else if (k >= 32 && k < 256 && ql < sizeof query - 8) {
            query[ql++] = (char)k; int need = u8_len((unsigned char)k);
            while (need > 1 && ql < sizeof query - 1) { int c = read_byte_timeout(50); if (c < 0) break; query[ql++] = (char)c; need--; }
            query[ql] = 0;
        } else continue;
        int i = g_hist_n - 1;                                                             /* re-search from the newest */
        while (i >= 0 && !ci_contains(g_hist[i], query)) i--;
        idx = i >= 0 ? i : g_hist_n;
    }
    e->prompt = saved_prompt; e->plen = saved_plen;
    g_field_prompt = NULL;
    if (keep && idx < g_hist_n) e->hist_idx = idx;
    free(orig);
}

/* ---------- pasted text ----------
 * More than three lines, or more than 800 bytes, go in as a token; the rest is typed in as it is. */
#define PASTE_LINES 3
#define PASTE_BYTES 800
static void paste_insert(editor *e, const char *t, size_t n) {
    int lines = 0;
    for (size_t i = 0; i < n; i++) if (t[i] == '\n') lines++;
    if (n && t[n - 1] != '\n') lines++;
    if ((lines > PASTE_LINES || n > PASTE_BYTES) && g_paste_n < PASTE_MAX) {
        char tok[64]; snprintf(tok, sizeof tok, "[Pasted text #%d +%d lines]", g_paste_n + 1, lines);
        g_paste[g_paste_n].text = xstrndup(t, n); g_paste[g_paste_n].lines = lines; g_paste_n++;
        ed_insert(e, tok, strlen(tok));
        return;
    }
    ed_insert(e, t, n);
}
/* Ctrl-O unfolds the paste token the caret is on (or just after): its text goes into the field in its place,
 * where it can be read and edited, and is sent as it stands. The token's record stays unreferenced:
 * paste_result() only substitutes tokens still in the field. */
static bool paste_unfold(editor *e) {
    size_t sp[PASTE_MAX][2]; int ks[PASTE_MAX], ns[PASTE_MAX];
    size_t nt = paste_tokens(e->buf.data, e->buf.len, sp, ks, ns, PASTE_MAX);
    for (size_t i = 0; i < nt; i++) {
        if (e->cur < sp[i][0] || e->cur > sp[i][1]) continue;
        int k = ks[i] - 1;
        if (k < 0 || k >= g_paste_n || !g_paste[k].text) return false;
        const char *t = g_paste[k].text; size_t n = strlen(t);
        /* take the token out, then put the text in its place: the caret ends after the text */
        e->cur = sp[i][0];
        ed_delete_range(e, sp[i][0], sp[i][1]);
        ed_insert(e, t, n);
        return true;
    }
    return false;
}
/* Backspace (before the cursor) or Delete (after it) on a whole token takes the whole token. */
static bool paste_del(editor *e, bool before) {
    size_t sp[PASTE_MAX][2];
    size_t n = paste_tokens(e->buf.data, e->buf.len, sp, NULL, NULL, PASTE_MAX);
    for (size_t i = 0; i < n; i++)
        if (before ? sp[i][1] == e->cur : sp[i][0] == e->cur) { ed_delete_range(e, sp[i][0], sp[i][1]); e->cur = sp[i][0]; return true; }
    return false;
}
/* The prompt's text as it is sent: the tokens put back. */
static char *paste_result(const editor *e) {
    const char *t[PASTE_MAX]; int l[PASTE_MAX];
    for (int i = 0; i < g_paste_n; i++) { t[i] = g_paste[i].text; l[i] = g_paste[i].lines; }
    return paste_expand(e->buf.data, e->buf.len, t, l, g_paste_n);
}

/* The modal for one paste: its lines, wrapped to the width, with a title row and a footer row
 * and the rest scrolled with the keys the viewer uses. Esc (or anything else) closes it; the
 * conversation is painted again from the model underneath, and the field is left as it was. */
static void paste_modal_impl(int k, const char *text, int lines);
static void paste_modal(int k, const char *text, int lines) { park_hold(); paste_modal_impl(k, text, lines); park_free(); }
static void paste_modal_impl(int k, const char *text, int lines) {
    if (!g_fs || !g_sb_active) return;
    int region = fs_region();
    if (region < 4) return;
    fflush(stdout);
    bool was = g_sb_pause; sb_pause(true);
    char *t = xstrdup(text);
    for (char *p = t; *p; p++) if (*p == '\t') *p = ' ';
    int width = g_fs_cols > 1 ? g_fs_cols : 1, body = region - 2;
    size_t cap = 256, n = 0; size_t (*rw)[2] = xmalloc(sizeof *rw * cap);   /* visual rows: (offset, length) */
    size_t len = strlen(t);
    for (size_t off = 0; off <= len; ) {
        const char *nl = strchr(t + off, '\n');
        size_t ll = nl ? (size_t)(nl - (t + off)) : len - off, st = 0;
        do {
            size_t e2 = ll ? vis_offset(t + off + st, ll - st, width) + st : 0;
            if (e2 <= st) e2 = ll;   /* (a character wider than the window: one row for it) */
            if (n == cap) { cap *= 2; rw = xrealloc(rw, sizeof *rw * cap); }
            rw[n][0] = off + st; rw[n][1] = e2 - st; n++;
            st = e2;
        } while (st < ll);
        if (!nl) break;
        off += ll + 1;
    }
    size_t top = 0, maxtop = n > (size_t)body ? n - body : 0;
    for (;;) {
        sbuf o; sb_init(&o);
        sb_puts(&o, "\x1b[?7l");
        sb_printf(&o, "\x1b[1;1H\x1b[2K" C_BOLD);
        char title[256];
        snprintf(title, sizeof title, " pasted text #%d · %d lines   ", k, lines);
        put_clipped(&o, title, width, false);
        sb_puts(&o, C_RESET " " C_DIM "↑/↓ PgUp/PgDn Home/End scroll · Esc closes" C_RESET);
        for (int i = 0; i < body; i++) {
            sb_printf(&o, "\x1b[%d;1H\x1b[2K", i + 2);
            size_t idx = top + (size_t)i;
            if (idx < n) sb_append(&o, t + rw[idx][0], rw[idx][1]);
        }
        sb_printf(&o, "\x1b[%d;1H\x1b[2K" C_DIM " rows %zu-%zu of %zu" C_RESET, region,
                  n ? top + 1 : 0, top + (size_t)body < n ? top + (size_t)body : n, n);
        sb_puts(&o, "\x1b[?7h");
        fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);
        int key = read_key();
        if (key == -2) break;   /* resized: the modal is closed; the token is still in the field */
        if (key == K_MOUSE) continue;   /* clicks mean nothing in the modal */
        if (key == K_UP || key == 'k') { if (top) top--; }
        else if (key == K_DOWN || key == 'j') { if (top < maxtop) top++; }
        else if (key == K_PGUP) { top = top > (size_t)body - 1 ? top - ((size_t)body - 1) : 0; }
        else if (key == K_PGDN || key == ' ') { top += (size_t)body - 1; }
        else if (key == K_WHEEL_UP) { top = top > 3 ? top - 3 : 0; }
        else if (key == K_WHEEL_DOWN) { top += 3; }
        else if (key == K_HOME || key == 'g') top = 0;
        else if (key == K_END || key == 'G') top = maxtop;
        else break;
        if (top > maxtop) top = maxtop;
    }
    free(rw); free(t);
    /* back to the conversation: painted again from the model, where the modal stood */
    if (!layout_sync()) { sbuf o; sb_init(&o); conv_repaint(&o); fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o); }
    sb_pause(was);
    term_status_refresh();
}

/* A click in the field: on a paste token it opens the paste. False = nothing was there. */
static bool paste_click(const editor *e) {
    int row = g_click_frow, col = g_click_fcol; g_click_frow = 0;
    if (!row || !e->in_field || !g_fs) return false;
    int top = field_top();
    if (row < top + 1 || row > top + g_field_rows) return false;
    int width = field_width(), plen = vis_width(field_prompt());
    int vrow = g_field_view + (row - (top + 1));
    size_t i = field_row_start(width, plen, vrow);
    int c = vrow == 0 ? plen : 0, want = col - 1 - FIELD_X0;   /* the column the click is on, counted in characters (a token is ASCII) */
    while (i < e->buf.len && e->buf.data[i] != '\n' && c < want) {
        i = u8_next(e->buf.data, i, e->buf.len); c++;
    }
    size_t sp[PASTE_MAX][2]; int ks[PASTE_MAX];
    size_t n = paste_tokens(e->buf.data, e->buf.len, sp, ks, NULL, PASTE_MAX);
    for (size_t j = 0; j < n; j++)
        if (i >= sp[j][0] && i < sp[j][1] && ks[j] >= 1 && ks[j] <= g_paste_n) {
            paste_modal(ks[j], g_paste[ks[j] - 1].text, g_paste[ks[j] - 1].lines);
            return true;
        }
    return false;
}

static char *readline_impl(const char *prompt, bool in_field) {
    paste_reset();   /* the pastes of the last prompt were sent (or dropped) with it */
    editor e; memset(&e, 0, sizeof e);
    sb_init(&e.buf); sb_append(&e.buf, "", 0);
    e.prompt = prompt; e.plen = vis_width(prompt);
    e.in_field = in_field && g_fs;
    e.hist_idx = g_hist_n;
    term_raw(true);
    fputs("\x1b[?2004h", stdout);   /* bracketed paste on */
    /* Still scrolled back from the turn that just ended? A question needs the live screen, and
     * so does someone who has been typing; anyone else goes on reading (scroll_view, below). */
    if (g_view_top >= 0 && (!e.in_field || g_ta_pos < g_ta_len)) view_leave();
    sug_clear();   /* the type-ahead's list, if one was up: the editor works out its own as the keys come in */
    if (e.in_field) {
        layout_sync();
        conv_pos(fs_region(), &g_conv_row, &g_conv_col);
        g_field_focus = true; g_field_view = 0; g_field_prompt = NULL;
    }
    ed_refresh(&e);
    char *result = NULL;
    bool done = false;
    int ctrlc_count = 0;
    bool esc_pending = false;   /* Esc Esc on an empty line = /rewind (like Claude Code) */
    /* Pending background work (main.c: the memory extraction) runs once, if the prompt is
     * still untouched after term_idle_ms — typing anything at all cancels it for this prompt. */
    bool idle_armed = term_idle_hook && term_idle_ms > 0;
    while (!done) {
        if (g_view_top >= 0) { scroll_view(0); e.prev_cursor_row = 0; ed_refresh(&e); continue; }
        int k = read_key_prompt(idle_armed && e.buf.len == 0);
        if (g_strip_rows) {   /* the agents strip: ↓ on an empty line goes into it; in it ↑/↓ move, Enter opens the log, Esc or a typed key returns to the field */
            if (g_strip_sel < 0 && k == K_DOWN && e.buf.len == 0 && !g_sug_rows) { g_strip_sel = 0; term_status_refresh(); continue; }
            if (g_strip_sel >= 0) {
                if (k == K_DOWN) { if (g_strip_sel < g_strip_rows - 1) g_strip_sel++; term_status_refresh(); continue; }
                if (k == K_UP) { g_strip_sel--; term_status_refresh(); continue; }
                if (k == K_ESC) { g_strip_sel = -1; term_status_refresh(); continue; }
                if (k == '\r' || k == '\n') { if (term_agents_open) term_agents_open(g_strip_sel); term_status_refresh(); continue; }
                g_strip_sel = -1; term_status_refresh();   /* any other key: back to the field, and that key is handled as usual */
            }
        }
        if (k == K_MOUSE) { if (!paste_click(&e)) fold_click(); continue; }   /* a click: a paste token opens it; on the head of a fold it opens or closes the fold */
        if (k == K_WHEEL_DOWN) continue;                   /* the wheel with nothing below to scroll to */
        if (k == K_IDLE) {
            idle_armed = false;
            ed_idle_run(&e);
            /* The idle work is a model call of its own (main.c: the memory extraction), and
             * Enter while it runs queues a message rather than reaching the editor. Nobody
             * else looks at the queue until a line is submitted here, so a message queued
             * meanwhile would sit and wait for the user to press Enter a second time:
             * submit the oldest one now instead, and the REPL runs it (and the rest behind
             * it) as if it had just been typed. */
            if (e.in_field && e.buf.len == 0 && g_queue_n) { result = term_queue_pop(); done = true; break; }
            continue;
        }
        if (k == -2) { ed_refresh(&e); continue; }   /* resized */
        idle_armed = false;
        if (k == -1) { result = NULL; break; }
        if (k != 3) ctrlc_count = 0;
        if ((k == K_ESC || k == K_BANNER_X) && g_ov_text) { term_overlay_close(); continue; }   /* Esc, or the banner's close button, closes the overlay before anything else */
        if (k == K_ESC && e.buf.len == 0) {
            if (esc_pending) { result = xstrdup("/rewind"); done = true; break; }
            esc_pending = true;
            ed_conv_out(&e, e.in_field ? C_DIM "(press Esc again to rewind the conversation / files)" C_RESET "\n"
                                       : "\n" C_DIM "(press Esc again to rewind the conversation / files)" C_RESET "\n");
            e.prev_cursor_row = 0;
            ed_refresh(&e);
            continue;
        }
        esc_pending = false;
        if (k == '\t') {   /* complete the word at the cursor (see "suggestions") */
            if (!g_sug_n) { e.sug_off = false; sug_update(&e); }
            if (g_sug_n && sug_tab(&e)) sug_update(&e);
            ed_refresh(&e);
            continue;
        }
        if (g_sug_n > 0) {   /* the list under the field is up: these keys are its own */
            bool taken = true;
            if (k == K_DOWN || k == 14) { if (g_sug_sel < g_sug_n - 1) g_sug_sel++; }
            else if ((k == K_UP || k == 16) && g_sug_sel >= 0) g_sug_sel--;
            else if (k == '\r' && g_sug_sel >= 0) { sug_accept(&e, g_sug_sel); sug_update(&e); }
            else if (k == K_ESC) { e.sug_off = true; sug_update(&e); }
            else taken = false;
            if (taken) { ed_refresh(&e); continue; }
        }
        char *before = xstrndup(e.buf.data, e.buf.len);
        bool recalled = false;   /* the text came out of the history */
        switch (k) {
            case '\r':
                if (e.buf.len && e.buf.data[e.buf.len - 1] == '\\') {   /* trailing backslash = newline */
                    e.buf.data[e.buf.len - 1] = '\n'; e.cur = e.buf.len; break;
                }
                result = paste_result(&e); done = true; break;
            case '\n': case K_ALT_ENTER: ed_insert(&e, "\n", 1); break;
            case 3:   /* Ctrl-C */
                if (e.buf.len) { ed_set(&e, ""); }
                else {
                    ctrlc_count++;
                    if (ctrlc_count >= 2) { result = NULL; done = true; break; }
                    ed_conv_out(&e, e.in_field ? C_DIM "(press Ctrl-C again or Ctrl-D to exit, /help for help)" C_RESET "\n"
                                               : "\n" C_DIM "(press Ctrl-C again or Ctrl-D to exit, /help for help)" C_RESET "\n");
                    e.prev_cursor_row = 0;
                }
                break;
            case 4:   /* Ctrl-D */
                if (e.buf.len == 0) { result = NULL; done = true; }
                else ed_delete_range(&e, e.cur, u8_next(e.buf.data, e.cur, e.buf.len));
                break;
            case 127: case 8:   /* backspace */
                if (paste_del(&e, true)) break;
                if (e.cur > 0) { size_t p = u8_prev(e.buf.data, e.cur); ed_delete_range(&e, p, e.cur); e.cur = p; }
                break;
            case K_DEL: if (paste_del(&e, false)) break; ed_delete_range(&e, e.cur, u8_next(e.buf.data, e.cur, e.buf.len)); break;
            case K_LEFT: case 2:
                if (k == K_LEFT && e.buf.len == 0 && term_agents_menu) { ed_hook_run(&e, term_agents_menu); break; }   /* ← on an empty line: the sub-agents */
                e.cur = u8_prev(e.buf.data, e.cur); break;
            case K_RIGHT: case 6: e.cur = u8_next(e.buf.data, e.cur, e.buf.len); break;
            case K_HOME: case 1: {
                size_t p = e.cur; while (p > 0 && e.buf.data[p-1] != '\n') p--; e.cur = p; break; }
            case 15: paste_unfold(&e); break;   /* Ctrl-O: unfold the paste token at the caret */
            case K_END: case 5: {
                size_t p = e.cur; while (p < e.buf.len && e.buf.data[p] != '\n') p++; e.cur = p; break; }
            case K_ALT_B: case K_CTRL_LEFT: e.cur = word_left(&e); break;
            case K_ALT_F: case K_CTRL_RIGHT: e.cur = word_right(&e); break;
            case 23: case K_ALT_BS: { size_t p = word_left(&e); ed_delete_range(&e, p, e.cur); e.cur = p; break; }  /* Ctrl-W */
            case 11: ed_delete_range(&e, e.cur, e.buf.len); break;   /* Ctrl-K */
            case 21: ed_delete_range(&e, 0, e.cur); e.cur = 0; break;   /* Ctrl-U */
            case 12: term_clear_screen(); e.prev_cursor_row = 0; ed_refresh(&e); break;   /* Ctrl-L */
            case 18: ed_hist_search(&e); recalled = true; break;            /* Ctrl-R */
            case K_SHIFT_TAB: g_cfg.mode = (g_cfg.mode + 1) % MODE_COUNT; break;
            case K_UP: case 16: {   /* history prev (only if single line or at first line) */
                if (memchr(e.buf.data, '\n', e.cur)) { /* move up a line */
                    size_t ls = e.cur; while (ls > 0 && e.buf.data[ls-1] != '\n') ls--;
                    size_t col = e.cur - ls;
                    size_t pls = ls - 1; while (pls > 0 && e.buf.data[pls-1] != '\n') pls--;
                    size_t plen = (ls - 1) - pls;
                    e.cur = pls + (col < plen ? col : plen);
                    break;
                }
                if (e.hist_idx > 0) {
                    if (e.hist_idx == g_hist_n) { free(e.hist_saved); e.hist_saved = xstrndup(e.buf.data, e.buf.len); }
                    e.hist_idx--; ed_set(&e, g_hist[e.hist_idx]);
                    recalled = true;
                }
                break; }
            case K_DOWN: case 14: {
                const char *nl = memchr(e.buf.data + e.cur, '\n', e.buf.len - e.cur);
                if (nl) {
                    size_t ls = e.cur; while (ls > 0 && e.buf.data[ls-1] != '\n') ls--;
                    size_t col = e.cur - ls;
                    size_t nls = (size_t)(nl - e.buf.data) + 1;
                    size_t nle = nls; while (nle < e.buf.len && e.buf.data[nle] != '\n') nle++;
                    size_t nlen = nle - nls;
                    e.cur = nls + (col < nlen ? col : nlen);
                    break;
                }
                if (e.hist_idx < g_hist_n) {
                    e.hist_idx++;
                    if (e.hist_idx == g_hist_n) ed_set(&e, e.hist_saved ? e.hist_saved : "");
                    else ed_set(&e, g_hist[e.hist_idx]);
                    recalled = true;
                }
                break; }
            case K_PASTE_START: {
                /* read raw until ESC[201~ */
                sbuf p; sb_init(&p);
                for (;;) {
                    int c = read_byte_timeout(2000);
                    if (c < 0) break;
                    sb_putc(&p, (char)c);
                    if (p.len >= 6 && !memcmp(p.data + p.len - 6, "\x1b[201~", 6)) { p.len -= 6; break; }
                }
                /* normalise CRLF / CR to LF, drop other control chars except tab */
                sbuf q; sb_init(&q);
                for (size_t i = 0; i < p.len; i++) {
                    char c = p.data[i];
                    if (c == '\r') { if (i + 1 < p.len && p.data[i+1] == '\n') continue; c = '\n'; }
                    if ((unsigned char)c < 32 && c != '\n' && c != '\t') continue;
                    sb_putc(&q, c);
                }
                if (q.len) paste_insert(&e, q.data, q.len);
                sb_free(&p); sb_free(&q);
                break; }
            case K_PGUP: case K_WHEEL_UP: {
                int page = view_rows() - 1;
                scroll_view(k == K_WHEEL_UP ? -WHEEL_ROWS : page > 1 ? -page : -1);
                e.prev_cursor_row = 0;
                break; }
            case K_ESC: case K_PASTE_END: case K_PGDN: break;
            default:
                if (k >= 32 && k < 256) {
                    char u[8]; int n = 1; u[0] = (char)k;
                    int need = u8_len((unsigned char)k);
                    while (n < need) { int c = read_byte_timeout(50); if (c < 0) break; u[n++] = (char)c; }
                    ed_insert(&e, u, (size_t)n);
                }
                break;
        }
        /* Suggestions follow what is typed, not what is recalled: the list coming up over a
         * history entry would take the ↑ that was meant to go on to the one before it. */
        if (recalled) e.sug_off = true;
        else if (strlen(before) != e.buf.len || memcmp(before, e.buf.data, e.buf.len)) e.sug_off = false;
        free(before);
        if (!done) { sug_update(&e); ed_refresh(&e); }
    }
    sug_clear();   /* (the refresh below gives its rows back to the conversation) */
    /* Leave the field empty again and put what was submitted into the conversation, where
     * the transcript continues — the field itself keeps no history of the turn. */
    if (e.in_field) {
        g_field_focus = false;
        g_field_text = ""; g_field_len = g_field_cur = 0; g_field_view = 0; g_field_prompt = NULL;
        sb_pause(true);
        fputs("\x1b[?2004l", stdout);
        printf("\x1b[%d;%dH", g_conv_row, g_conv_col);
        fflush(stdout);
        sb_pause(false);
        if (result) printf("%s%s" C_RESET "\n", prompt, result);
        else printf("\n");
        term_status_refresh();
    } else {   /* drawn inline (a question inside a turn): finish where it stands */
        int width = term_width();
        int erow, ecol, crow, ccol;
        ed_pos(&e, e.buf.len, width, &erow, &ecol);
        ed_pos(&e, e.cur, width, &crow, &ccol);
        sb_pause(true);
        if (erow > crow) printf("\x1b[%dB", erow - crow);
        printf("\n\r\x1b[J");   /* next line, clear anything below (candidates, hints) */
        fputs("\x1b[?2004l", stdout);
        term_status_refresh();   /* "\x1b[J" wiped the bar */
        sb_pause(false);
        if (result) {   /* what stays on screen: the prompt and the submitted text */
            sbuf n; sb_init(&n); sb_puts(&n, prompt); sb_puts(&n, result); sb_puts(&n, C_RESET "\n");
            sb_note(n.data); sb_free(&n);
        } else sb_note("\n");
    }
    fflush(stdout);
    free(e.hist_saved);
    sb_free(&e.buf);
    return result;
}

bool term_is_ready(void) { return g_ready; }

char *term_readline(const char *prompt) {
    g_ready = true; term_status_refresh();
    g_tick_live = g_tick_fn != NULL; g_tick_next = mono_ms() + TICK_MS;
    char *s = g_ui_nc ? nc_readline(prompt, true) : readline_impl(prompt, true);
    if (g_tick_live && g_tick_fn) g_tick_fn(true);   /* the block settles on its last frame */
    /* the first task ends the banner's animation for good: a slash command or a shell line is not work */
    if (s && *s && *s != '/' && *s != '!' && !g_tick_done) { g_tick_fn = NULL; g_tick_done = true; }
    /* the modal is closed by any line that is sent: a reply, or a command's output, must not land under it */
    if (s && *s && term_banner_open()) term_banner_hide();
    g_tick_live = false;
    g_ready = false; term_status_refresh();
    return s;
}
char *term_ask_line(const char *prompt) {
    if (g_ui_nc) return nc_readline(prompt, false);
    park_hold(); char *r = readline_impl(prompt, false); park_free();
    return r;
}

/* ---------- confirmation menu ---------- */
static int confirm_impl(const char *question, const char *always_label, const char *project_label, char **reason);
int term_confirm(const char *question, const char *always_label, const char *project_label, char **reason) {
    if (g_ui_nc) return nc_confirm(question, always_label, project_label, reason);
    park_hold(); int r = confirm_impl(question, always_label, project_label, reason); park_free();
    return r;
}
static int confirm_impl(const char *question, const char *always_label, const char *project_label, char **reason) {
    if (reason) *reason = NULL;
    term_raw(true);
    ta_stash stash = ta_take();
    const char *labels[4] = { "Yes", always_label ? always_label : "Yes, and don't ask again this session",
                              project_label ? project_label : "No, and tell the model what to do instead",
                              "No, and tell the model what to do instead" };
    const char *keys[4] = { "y", "a", project_label ? "p" : "n", "n" };
    int nopt = project_label ? 4 : 3, no_i = nopt - 1;
    int sel = 0, drawn = 0, choice = -1;
    view_leave();     /* a question is asked on the live screen */
    sb_pause(true);   /* the menu is transient; only its collapsed final line is conversation content */
    for (;;) {
        if (layout_sync()) drawn = 0;   /* repainted: the cursor is back where the menu starts */
        int width = term_width();
        sbuf o; sb_init(&o);
        if (drawn) sb_printf(&o, "\x1b[%dA", drawn);
        sb_puts(&o, "\r\x1b[J");
        int lines = 0;
        sb_printf(&o, "  " C_YELLOW C_BOLD "%s" C_RESET "\n", question); lines++;
        for (int i = 0; i < nopt; i++) {
            char line[256];
            snprintf(line, sizeof line, "%d. %s", i + 1, labels[i]);
            int room = width - 4 - 5 - 1;   /* "  ❯ " prefix, "  (x)" suffix, never touch the last column */
            if (room < 8) room = 8;
            if ((int)strlen(line) > room) { line[room - 1] = 0; strcat(line, "…"); }
            if (i == sel) sb_printf(&o, "  " C_ORANGE "❯ " C_BOLD "%s" C_RESET C_DIM "  (%s)" C_RESET "\n", line, keys[i]);
            else          sb_printf(&o, "    %s" C_DIM "  (%s)" C_RESET "\n", line, keys[i]);
            lines++;
        }
        sb_puts(&o, "  " C_GRAY);
        if (nopt == 4)
            sb_puts(&o, width >= 72 ? "↑/↓ or j/k move · enter select · 1-4 / y / a / p / n · esc = no"
                      : width >= 44 ? "↑/↓ · enter · 1-4 / y / a / p / n · esc = no" : "↑/↓ enter y/a/p/n esc");
        else
            sb_puts(&o, width >= 72 ? "↑/↓ or j/k move · enter select · 1-3 / y / a / n · esc = no"
                      : width >= 44 ? "↑/↓ · enter · 1-3 / y / a / n · esc = no" : "↑/↓ enter y/a/n esc");
        sb_puts(&o, C_RESET);
        drawn = lines;   /* cursor sits on the hint line, `lines` rows below the question */
        chrome_append(&o);
        fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);

        int k = read_key();
        if (k == -2) continue;                                   /* resize: redraw */
        if (k == -1 || k == 3) { choice = 0; break; }             /* EOF / Ctrl-C */
        if (k == K_ESC) { choice = no_i; break; }
        if (k == '\r' || k == '\n') { choice = sel; break; }
        if (k == '1' || k == 'y' || k == 'Y') { choice = 0; break; }
        if (k == '2' || k == 'a' || k == 'A') { choice = 1; break; }
        if (nopt == 4 && (k == '3' || k == 'p' || k == 'P')) { choice = 2; break; }
        if (k == '0' + nopt || k == 'n' || k == 'N') { choice = no_i; break; }
        if (k == K_UP || k == 'k' || k == 16) { if (sel > 0) sel--; continue; }
        if (k == K_DOWN || k == 'j' || k == 14) { if (sel < nopt - 1) sel++; continue; }
        if (k == '\t') { sel = (sel + 1) % nopt; continue; }
        if (k == K_HOME) { sel = 0; continue; }
        if (k == K_END) { sel = no_i; continue; }
        /* anything else: ignore, keep the menu up */
    }
    /* collapse the menu into a single line */
    if (drawn) printf("\x1b[%dA", drawn);
    printf("\r\x1b[J");
    sb_pause(false);
    if (!term_confirm_quiet) printf("  " C_YELLOW "%s" C_RESET "  %s\n", question,
           choice == 0 ? C_GREEN "yes" C_RESET : choice == 1 ? C_GREEN "yes, always this session" C_RESET
           : (nopt == 4 && choice == 2) ? C_GREEN "yes, always in this project" C_RESET : C_RED "no" C_RESET);
    term_status_refresh();
    fflush(stdout);
    int rc = choice == 0 ? 1 : choice == 1 ? 2 : (nopt == 4 && choice == 2) ? 3 : 0;
    if (rc == 0 && reason && choice == no_i) {   /* (Ctrl-C / EOF skip the reason prompt) */
        char *r = term_ask_line(C_DIM "  tell the model why / what to do instead (enter to skip): " C_RESET);
        if (r && *r) *reason = r; else free(r);
    }
    ta_restore(stash);
    return rc;
}

/* ---------- markdown streaming printer ---------- */
void md_init(md_state *m) { memset(m, 0, sizeof *m); m->at_line_start = true; sb_init(&m->word); }

/* a code line of a fence is printed when it ends, highlighted by the fence's language */
/* ---------- word wrap of the prose ----------
 * On a full screen the prose breaks at word boundaries, not where the terminal would break a row (in the
 * middle of a word). The word being collected is held until its end (a space, a newline, or any escape
 * the printer writes), then printed; a word that does not fit on the rest of the line starts a new one. Off
 * a full screen (a pipe, -p) the text is printed as it comes. */
static int md_width(void) { return g_fs || g_ui_nc ? (term_width() > 2 ? term_width() - 1 : 0) : 0; }
static void md_word_flush(md_state *m) {
    if (!m->word.len) return;
    int ww = vis_width(m->word.data), width = md_width();
    if (width && m->col > 0 && m->col + ww > width) { fputc('\n', stdout); m->col = 0; }
    fwrite(m->word.data, 1, m->word.len, stdout);
    m->col += ww;
    sb_clear(&m->word);
}
static void md_prose(md_state *m, char c) {
    if (!md_width()) { fputc(c, stdout); return; }
    if (c == ' ' || c == '\t') {
        md_word_flush(m);
        if (m->col > 0) { fputc(' ', stdout); m->col++; }
        return;
    }
    if (c == '\n') { md_word_flush(m); fputc('\n', stdout); m->col = 0; return; }
    sb_putc(&m->word, c);
}
static void md_fputs(md_state *m, const char *s) { md_word_flush(m); fputs(s, stdout); }
static void md_code_line(md_state *m) {
    md_word_flush(m);   /* the word before it goes out first */
    sbuf out; sb_init(&out);
    highlight_line(m->lang, m->line.data ? m->line.data : "", &out, C_CYAN);
    fputs(out.data ? out.data : "", stdout);
    sb_free(&out);
    sb_clear(&m->line);
}

static void md_flush_ticks(md_state *m) {
    int t = m->pending_ticks;
    m->pending_ticks = 0;
    if (t == 0) return;
    md_word_flush(m);   /* the word before the marker goes out first */
    if (t >= 3 && m->at_line_start) {
        m->in_fence = !m->in_fence;
        fputs(m->in_fence ? C_GRAY "```" : "```" C_RESET, stdout);
        if (m->in_fence) {   /* the info string follows: it names the language */
            fputs(C_RESET C_CYAN, stdout);
            m->in_info = true; sb_clear(&m->info); sb_clear(&m->line); m->lang[0] = 0;
        } else { m->in_info = false; }
        for (int i = 3; i < t; i++) fputc('`', stdout);
        return;
    }
    if (m->in_fence && !m->in_info) { for (int i = 0; i < t; i++) sb_putc(&m->line, '`'); return; }
    if (m->in_fence) { for (int i = 0; i < t; i++) fputc('`', stdout); return; }
    if (t == 1) {
        m->in_code = !m->in_code;
        fputs(m->in_code ? C_CYAN "`" : "`" C_RESET, stdout);
        if (!m->in_code && m->in_bold) fputs(C_BOLD, stdout);
        return;
    }
    for (int i = 0; i < t; i++) fputc('`', stdout);
}

static void md_flush_star(md_state *m) {
    if (!m->pending_star) return;
    md_word_flush(m);   /* the word before the marker goes out first */
    fputc('*', stdout); m->pending_star = false;
}

void md_feed(md_state *m, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '`') { md_flush_star(m); m->pending_ticks++; continue; }
        if (m->pending_ticks) { md_flush_ticks(m); m->at_line_start = false; }
        if (m->in_fence && m->in_info) {   /* the info string: printed as it comes, and kept for the language */
            md_prose(m, c);
            if (c != '\n') { sb_putc(&m->info, c); continue; }
            m->in_info = false;
            char *w = m->info.data ? m->info.data : "";
            while (*w == ' ' || *w == '\t') w++;
            size_t k = strcspn(w, " \t");
            if (k >= sizeof m->lang) k = sizeof m->lang - 1;
            for (size_t j = 0; j < k; j++) m->lang[j] = (char)tolower((unsigned char)w[j]);
            m->lang[k] = 0;
            m->at_line_start = true;
            md_fputs(m, C_CYAN);
            continue;
        }
        if (m->in_fence) {   /* the code: a line is printed when it ends, highlighted */
            if (c == '\n') { md_code_line(m); md_word_flush(m); fputc('\n', stdout); m->col = 0; m->at_line_start = true; md_fputs(m, C_CYAN); }
            else { sb_putc(&m->line, c); m->at_line_start = false; }
            continue;
        }
        if (!m->in_fence && !m->in_code) {
            if (c == '*') {
                if (m->pending_star) { m->pending_star = false; m->in_bold = !m->in_bold; md_fputs(m, m->in_bold ? C_BOLD : C_RESET); continue; }
                m->pending_star = true; continue;
            }
            md_flush_star(m);
            if (m->at_line_start && c == '#') { md_fputs(m, C_BOLD); m->in_bold = true; }
        }
        md_prose(m, c);
        if (c == '\n') {
            m->at_line_start = true;
            if (m->in_bold && !m->in_fence) { m->in_bold = false; md_fputs(m, C_RESET); if (m->in_code) md_fputs(m, C_CYAN); }
            if (m->in_fence) md_fputs(m, C_CYAN);
        } else if (c != ' ' && c != '\t') m->at_line_start = false;
    }
    fflush(stdout);
}

void md_finish(md_state *m) {
    md_word_flush(m);
    md_flush_star(m);
    if (m->pending_ticks) md_flush_ticks(m);
    if (m->in_fence && m->line.len) md_code_line(m);   /* a last line that never ended */
    fputs(C_RESET, stdout);
    m->in_fence = m->in_code = m->in_bold = m->in_info = false;
    sb_free(&m->line); sb_free(&m->info);
    fflush(stdout);
}

/* ---------- interactive selector (used by /model) ---------- */
static int select_impl(const char *title, const char **items, const char **descs, int n, int current);
int term_select(const char *title, const char **items, const char **descs, int n, int current) {
    if (g_ui_nc) return nc_select(title, items, descs, n, current);
    park_hold(); int r = select_impl(title, items, descs, n, current); park_free();
    return r;
}
/* s cut to `width` cells into r, the cut ended by … */
static void cells_cut(sbuf *r, const char *s, int width) {
    if (vis_width(s) <= width) { sb_puts(r, s); return; }
    sb_append(r, s, vis_offset(s, strlen(s), width > 1 ? width - 1 : 0));
    sb_puts(r, "…");
}
static int select_impl(const char *title, const char **items, const char **descs, int n, int current) {
    if (n <= 0) return -1;
    term_raw(true);
    int sel = current >= 0 && current < n ? current : 0;
    char filter[128] = {0}; int flen = 0;
    int *vis = xmalloc(sizeof(int) * (size_t)n); int nvis = 0;
    int drawn = 0;            /* lines drawn last time */
    int max_show = 12;
    int top = 0;
    int result = -1;
    view_leave();     /* a menu is drawn on the live screen */
    sb_pause(true);   /* transient: not conversation content */
    for (;;) {
        /* filter */
        nvis = 0;
        for (int i = 0; i < n; i++) if (!flen || strcasestr(items[i], filter)) vis[nvis++] = i;
        /* keep sel valid within visible list */
        int selpos = -1;
        for (int i = 0; i < nvis; i++) if (vis[i] == sel) selpos = i;
        if (selpos < 0) { selpos = 0; sel = nvis ? vis[0] : -1; }
        if (selpos < top) top = selpos;
        if (selpos >= top + max_show) top = selpos - max_show + 1;
        /* draw */
        if (layout_sync()) drawn = 0;   /* repainted: the cursor is back where the menu starts */
        sbuf o; sb_init(&o);
        if (drawn) sb_printf(&o, "\x1b[%dA", drawn);
        sb_puts(&o, "\r\x1b[J");
        int lines = 0;
        int w = term_width();
        /* one row, whatever the width: a title that wraps is still counted as one, and the redraw
         * then climbs a row too few — a stale row per key, and the transcript drawn over afterwards */
        static const char HINT[] = "type to filter · ↑/↓ move · enter select · esc cancel";
        int tw = vis_width(title);
        if (tw > w - 1) sb_printf(&o, C_BOLD "%.*s…" C_RESET "\n", (int)vis_offset(title, strlen(title), w > 2 ? w - 2 : 0), title);
        else if (tw + 2 + vis_width(HINT) > w - 1) sb_printf(&o, C_BOLD "%s" C_RESET "\n", title);
        else sb_printf(&o, C_BOLD "%s" C_RESET "  " C_DIM "%s" C_RESET "\n", title, HINT);
        lines++;
        sb_printf(&o, "  " C_DIM "filter:" C_RESET " %s" C_DIM "▏" C_RESET "\n", filter); lines++;
        for (int i = top; i < nvis && i < top + max_show; i++) {
            int idx = vis[i];
            bool is_sel = (i == selpos);
            /* the label column is 44 cells, or half the row when the row is narrow; the description takes the rest. The row
             * is cut by display width (a byte cut garbles a UTF-8 letter), with the … the cut shows. */
            int room = w - 4, namew = room >= 88 ? 44 : room / 2;
            sbuf txt; sb_init(&txt); sb_puts(&txt, idx == current ? "* " : "  ");
            sbuf nm; sb_init(&nm); cells_cut(&nm, items[idx], namew);
            sb_puts(&txt, nm.data ? nm.data : "");
            for (int c = vis_width(nm.data ? nm.data : ""); c < namew; c++) sb_putc(&txt, ' ');
            sb_putc(&txt, ' ');
            if (descs && descs[idx]) sb_puts(&txt, descs[idx]);
            sbuf row; sb_init(&row); cells_cut(&row, txt.data ? txt.data : "", room);
            sb_printf(&o, "%s%s %s" C_RESET "\n", is_sel ? C_ORANGE "❯" : " ", is_sel ? C_BOLD : "", row.data ? row.data : ""); lines++;
            sb_free(&row); sb_free(&nm); sb_free(&txt);
        }
        if (nvis == 0) { sb_puts(&o, C_DIM "  (no matches)" C_RESET "\n"); lines++; }
        if (nvis > max_show) { sb_printf(&o, C_DIM "  … %d of %d shown" C_RESET "\n", max_show < nvis ? max_show : nvis, nvis); lines++; }
        drawn = lines;
        chrome_append(&o);
        fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);

        int k = read_key();
        if (k == -2) continue;
        if (k == -1 || k == K_ESC || k == 3) { result = -1; break; }
        if (k == '\r') { result = sel; break; }
        if (k == K_UP || k == 16) { if (selpos > 0) sel = vis[selpos - 1]; continue; }
        if (k == K_DOWN || k == 14) { if (selpos + 1 < nvis) sel = vis[selpos + 1]; continue; }
        if (k == K_PGUP) { selpos -= max_show; if (selpos < 0) selpos = 0; if (nvis) sel = vis[selpos]; continue; }
        if (k == K_PGDN) { selpos += max_show; if (selpos >= nvis) selpos = nvis - 1; if (nvis) sel = vis[selpos]; continue; }
        if (k == K_HOME) { if (nvis) sel = vis[0]; continue; }
        if (k == K_END) { if (nvis) sel = vis[nvis - 1]; continue; }
        if (k == 127 || k == 8) { if (flen) filter[--flen] = 0; continue; }
        if (k == 21) { flen = 0; filter[0] = 0; continue; }
        if (k >= 32 && k < 127 && flen < (int)sizeof filter - 1) { filter[flen++] = (char)k; filter[flen] = 0; continue; }
    }
    /* clear menu */
    if (drawn) printf("\x1b[%dA", drawn);
    printf("\r\x1b[J");
    term_status_refresh();
    fflush(stdout);
    sb_pause(false);
    free(vis);
    return result;
}
