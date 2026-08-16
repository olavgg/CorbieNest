/* Terminal handling: raw mode, key decoding, a small multi-line line editor
 * (linenoise-style, UTF-8 aware) with history + slash-command completion,
 * an interactive list picker, and a streaming markdown-ish printer. */
#define _GNU_SOURCE
#include "common.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_orig;
static bool g_have_orig = false, g_raw = false;
static int vis_width(const char *s);   /* display width of a UTF-8 string, ignoring SGR escapes */
static volatile sig_atomic_t g_winch = 0;
static void sb_hook_stdout(void);       /* scrollback capture (see "scrollback" below) */
static void sb_pause(bool on);
static void sb_note(const char *text);
static size_t u8_next(const char *s, size_t pos, size_t len);
static const char *g_bar_override;

static void on_winch(int s) { (void)s; g_winch = 1; }

void term_init(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig) == 0) g_have_orig = true;
    signal(SIGWINCH, on_winch);
    atexit(term_restore);
}

void term_raw(bool on) {
    if (!g_have_orig) return;
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
    term_fullscreen(false);
    if (g_raw) {
        fputs("\x1b[?2004l" C_RESET, stdout);   /* disable bracketed paste */
        fflush(stdout);
        term_raw(false);
    }
}

void term_size(int *rows, int *cols) {
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
    K_CTRL_LEFT, K_CTRL_RIGHT, K_SHIFT_TAB
};

/* ---------- full screen + status bar ---------- */
/* In full-screen mode the app owns the alternate screen; rows 1..N-1 form the
 * scrolling region for the conversation and row N holds the status bar. Every
 * redraw that uses "\x1b[J" (erase to end of screen) also wipes the bar, so the
 * editor / menus append the bar again after each redraw (see bar_append). */
static bool g_fs = false;
static int  g_fs_rows = 0, g_fs_cols = 0;   /* size the scroll region was set up for */
static long g_live_out = 0;                 /* output tokens of the generation in flight */
static struct timeval g_live_t;             /* last live redraw (throttle) */
static const char *g_busy = NULL;           /* activity label shown (animated) in the bar; NULL = idle */
static int  g_busy_frame = 0;
static struct timeval g_busy_t;             /* last spinner advance (throttle) */
static const char *BUSY_SPIN[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
static int g_queue_n;                       /* messages queued while busy (see term_queue_*) */

static char *ta_pending_text(void);         /* text typed while busy, not yet submitted (malloc'd or NULL) */

void fmt_tokens(long n, char *out, size_t sz) {
    if (n < 1000) snprintf(out, sz, "%ld", n);
    else if (n < 10000) snprintf(out, sz, "%.1fk", n / 1000.0);
    else if (n < 1000000) snprintf(out, sz, "%ldk", n / 1000);
    else snprintf(out, sz, "%.1fM", n / 1e6);
}

/* Appends the status bar text, at most `cols - 1` visible columns wide. Segments are
 * dropped from the right when the terminal is narrow. */
static void bar_build(sbuf *o, int cols) {
    if (g_bar_override) {   /* the scrollback viewer owns the bar */
        char *t = xstrdup(g_bar_override);
        int w = vis_width(t);
        if (w > cols - 1) { size_t i = 0; int k = 0; while (t[i] && k < cols - 2) { i = u8_next(t, i, strlen(t)); k++; } t[i] = 0; strcat(t, "…"); }
        sb_printf(o, C_DIM "%s" C_RESET, t); free(t); return;
    }
    const char *icon, *col, *text;
    switch (g_cfg.mode) {
        case MODE_ACCEPT_EDITS: icon = "⏵⏵"; col = C_ORANGE; text = "accept edits on"; break;
        case MODE_PLAN:         icon = "⏸ "; col = C_CYAN;   text = "plan mode on";    break;
        case MODE_AUTO:         icon = "⏵⏵"; col = C_RED;    text = "auto mode on";    break;
        default:                icon = "⏵ "; col = C_DIM;    text = "manual mode";     break;
    }
    const char *model = g_cfg.model ? g_cfg.model : "(no model)";
    char tin[32], tout[32], ttot[32];
    long out_now = g_session.eval_tokens + g_live_out;
    fmt_tokens(g_session.prompt_tokens, tin, sizeof tin);
    fmt_tokens(out_now, tout, sizeof tout);
    fmt_tokens(g_session.prompt_tokens + out_now, ttot, sizeof ttot);
    char ctx[32] = "";
    if (g_cfg.num_ctx > 0 && g_session.last_prompt_tokens > 0)
        snprintf(ctx, sizeof ctx, "ctx %d%%", (int)(100.0 * g_session.last_prompt_tokens / g_cfg.num_ctx));
    /* segments in priority order: mode, model, tokens, ctx, hint */
    char seg_tok[96], seg_tok_long[128];
    snprintf(seg_tok, sizeof seg_tok, "%s tokens", ttot);
    snprintf(seg_tok_long, sizeof seg_tok_long, "%s tokens (↑%s ↓%s)", ttot, tin, tout);
    const char *hint = "(shift+tab to cycle)";
    int room = cols - 1;
    int used = 0;
    #define SEP_W 3   /* " │ " */
    /* activity indicator (right-aligned) is reserved first: it must never be squeezed out */
    char busy[128] = ""; int busy_w = 0;
    if (g_busy) {
        snprintf(busy, sizeof busy, "%s %s", BUSY_SPIN[g_busy_frame % 10], g_busy);
        busy_w = vis_width(busy);
        if (busy_w + SEP_W + 24 <= room) room -= busy_w + SEP_W; else busy_w = 0;
    }
    /* mode segment (icon is 2 cells + space), followed by the how-to-switch hint */
    sb_printf(o, " %s%s %s" C_RESET, col, icon, text);
    used = 1 + 3 + vis_width(text);
    /* text being typed while the model works: echo it here (it is not in the editor
     * yet), in place of the hint / model / token segments, keeping the tail visible */
    char *typing = g_busy ? ta_pending_text() : NULL;
    if (typing) {
        for (char *p = typing; *p; p++) if (*p == '\n') *p = ' ';
        int tw = vis_width(typing), avail = room - used - SEP_W - 4;   /* "› " + "▏" + slack */
        if (avail >= 4) {
            sb_puts(o, C_DIM " │ › " C_RESET);
            const char *p = typing;
            if (tw > avail) {
                int cut = tw - (avail - 1);
                while (cut > 0 && *p) { if (((unsigned char)p[1] & 0xC0) != 0x80) cut--; p++; }
                while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++;
                sb_puts(o, "…"); used += 1;
            }
            sb_puts(o, p); sb_puts(o, C_DIM "▏" C_RESET);
            used += SEP_W + 2 + vis_width(p) + 1;
        }
        free(typing);
    }
    if (!typing) {
        int hw = vis_width(hint);
        if (used + 1 + hw + 20 <= room) { sb_printf(o, C_DIM " %s" C_RESET, hint); used += 1 + hw; }
    }
    /* model (truncate if needed) */
    if (!typing) {
        int mw = vis_width(model), avail = room - used - SEP_W;
        if (avail >= 6) {
            sb_puts(o, C_DIM " │ " C_RESET);
            if (mw <= avail) { sb_puts(o, model); used += SEP_W + mw; }
            else {   /* keep the tail, e.g. ":7b" is more useful than the registry prefix */
                int cut = mw - (avail - 1);
                const char *p = model; while (cut > 0 && *p) { if (((unsigned char)p[1] & 0xC0) != 0x80) cut--; p++; }
                while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++;
                sb_printf(o, "…%s", p); used += SEP_W + avail;
            }
        }
    }
    /* queued messages (typed + Enter while the model was busy) */
    if (g_queue_n) {
        char q[32]; snprintf(q, sizeof q, "%d queued", g_queue_n);
        int qw = vis_width(q), avail = room - used - SEP_W;
        if (qw <= avail) { sb_puts(o, C_DIM " │ " C_RESET); sb_printf(o, C_ORANGE "%s" C_RESET, q); used += SEP_W + qw; }
    }
    /* tokens */
    if (!typing) {
        int lw = vis_width(seg_tok_long), sw = vis_width(seg_tok), avail = room - used - SEP_W;
        const char *pick = lw <= avail ? seg_tok_long : sw <= avail ? seg_tok : NULL;
        if (pick) {
            sb_puts(o, C_DIM " │ " C_RESET);
            if (g_live_out) sb_printf(o, C_ORANGE "%s" C_RESET, pick); else sb_puts(o, pick);
            used += SEP_W + vis_width(pick);
        }
    }
    if (ctx[0] && !typing) {
        int cw = vis_width(ctx), avail = room - used - SEP_W;
        if (cw <= avail) {
            int pct = atoi(ctx + 4);
            sb_puts(o, C_DIM " │ " C_RESET);
            sb_printf(o, "%s%s" C_RESET, pct >= 85 ? C_YELLOW : "", ctx);
            used += SEP_W + cw;
        }
    }
    if (busy_w) {
        for (int i = used; i < room; i++) sb_putc(o, ' ');
        sb_printf(o, C_DIM " │ " C_RESET C_ORANGE "%s" C_RESET, busy);
    }
    #undef SEP_W
}

/* Ask the terminal where the cursor is (CSI 6n). Bytes the user typed meanwhile
 * are kept as type-ahead. Returns false on timeout (terminal did not answer). */
static void ta_put(unsigned char c);
static bool query_cursor(int *row, int *col) {
    if (!g_have_orig) return false;
    fputs("\x1b[6n", stdout); fflush(stdout);
    unsigned char keep[512]; size_t nk = 0;
    char rep[32]; size_t nr = 0; bool in_rep = false, ok = false;
    for (int guard = 0; guard < 4096 && !ok; guard++) {
        fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
        struct timeval tv = { 0, 200 * 1000 };
        if (select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv) <= 0) break;
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        if (!in_rep) {
            if (c == 27) { in_rep = true; nr = 0; rep[nr++] = (char)c; }
            else if (nk < sizeof keep) keep[nk++] = c;
            continue;
        }
        rep[nr++] = (char)c;
        bool valid = (nr == 2) ? c == '[' : ((c >= '0' && c <= '9') || c == ';' || c == 'R');
        if (valid && c == 'R') { ok = sscanf(rep, "\x1b[%d;%dR", row, col) == 2; if (ok) break; }
        if (!valid || c == 'R' || nr >= sizeof rep - 1) {   /* not a report (a key sequence): keep it */
            for (size_t i = 0; i < nr && nk < sizeof keep; i++) keep[nk++] = (unsigned char)rep[i];
            in_rep = false;
        }
    }
    for (size_t i = 0; i < nk; i++) ta_put(keep[i]);
    return ok;
}

/* Must run before any redraw. If the terminal was resized: re-apply the scroll
 * region for the new size and, when the shrink pushed the cursor onto the reserved
 * bottom row, scroll the region up one line (IND) so that the caller's relative
 * "cursor up N, erase, redraw" still lands inside the region. Writes to stdout. */
static void layout_sync(void) {
    if (!g_fs) return;
    int rows, cols; term_size(&rows, &cols);
    if (rows == g_fs_rows && cols == g_fs_cols) return;
    bool first = g_fs_rows == 0;
    g_fs_rows = rows; g_fs_cols = cols;
    int crow = 1, ccol = 1;
    bool known = first || query_cursor(&crow, &ccol);
    fputs("\x1b" "7", stdout);                                   /* DECSTBM homes the cursor: bracket it */
    if (rows > 1) printf("\x1b[1;%dr", rows - 1); else fputs("\x1b[r", stdout);
    fputs("\x1b" "8", stdout);
    if (!first && known && crow >= rows && rows > 1) printf("\x1b[%d;%dH\x1b" "D", rows - 1, ccol);
    fflush(stdout);
}

/* Draw the bar without disturbing the cursor: DECSC, jump to the last row, draw,
 * erase the rest of the row, DECRC. Callers run layout_sync() first. */
static void bar_append(sbuf *o) {
    if (!g_fs) return;
    sb_printf(o, "\x1b" "7" "\x1b[%d;1H", g_fs_rows);
    bar_build(o, g_fs_cols);
    sb_puts(o, "\x1b[K" "\x1b" "8");
}

void term_status_refresh(void) {
    if (!g_fs) return;
    layout_sync();
    sbuf o; sb_init(&o);
    bar_append(&o);
    fwrite(o.data, 1, o.len, stdout); fflush(stdout);
    sb_free(&o);
    gettimeofday(&g_live_t, NULL);
}

void term_status_live(long out_tokens) {
    g_live_out = out_tokens;
    if (!g_fs) return;
    if (out_tokens == 0) return;   /* the caller refreshes once the final stats are in */
    struct timeval t; gettimeofday(&t, NULL);
    double dt = (double)(t.tv_sec - g_live_t.tv_sec) + (double)(t.tv_usec - g_live_t.tv_usec) / 1e6;
    if (dt >= 0.15 || dt < 0) term_status_refresh();
}

void term_busy(const char *label) {
    g_busy = label;
    if (label) g_busy_frame = 0;
    gettimeofday(&g_busy_t, NULL);
    term_status_refresh();
}

void term_busy_tick(void) {
    if (!g_busy || !g_fs) return;
    struct timeval t; gettimeofday(&t, NULL);
    double dt = (double)(t.tv_sec - g_busy_t.tv_sec) + (double)(t.tv_usec - g_busy_t.tv_usec) / 1e6;
    if (dt < 0.1 && dt >= 0) return;
    g_busy_t = t;
    g_busy_frame++;
    term_status_refresh();
}

void term_fullscreen(bool on) {
    if (on && !g_fs) {
        sb_hook_stdout();   /* start recording the conversation for PgUp */
        g_fs = true; g_fs_rows = g_fs_cols = 0; g_live_out = 0;
        fputs("\x1b[?1049h\x1b[H\x1b[2J", stdout);   /* alternate screen, cleared */
        term_status_refresh();                       /* also sets the scroll region */
    } else if (!on && g_fs) {
        g_fs = false;
        fputs(C_RESET "\x1b[r\x1b[?1049l", stdout);      /* reset margins, back to the main screen */
        fflush(stdout);
    }
}

void term_clear_screen(void) {
    fputs("\x1b[H\x1b[2J", stdout);
    term_status_refresh();
    fflush(stdout);
}

/* ---------- scrollback (PgUp / PgDn) ----------
 * The alternate screen has no scrollback of its own, so the app keeps one: everything the
 * conversation prints to stdout is teed (see sb_hook_stdout) into a list of logical lines,
 * with SGR colours kept and the few cursor movements we use (CR, cursor up/down/right,
 * erase line / erase below, DECSC…DECRC around the status bar) interpreted so the model
 * tracks what is on screen. Drawing that is not conversation content — the line editor, the
 * menus, the viewer itself — runs with the capture paused (sb_pause), and the editor adds
 * the submitted line explicitly (sb_note). PgUp at the prompt opens the viewer
 * (scroll_view): the region shows a window into the buffer, PgDn/End/Esc go back. */
#define SB_MAX_LINES 8000
static sbuf *g_sb = NULL;             /* logical lines */
static int   g_sb_n = 0, g_sb_cap = 0;
static int   g_sb_line = 0;           /* cursor: line index ... */
static int   g_sb_col = 0;            /* ... and visual column in it (>= width means a wrapped row) */
static bool  g_sb_pause = false;      /* not conversation output: don't record */
static bool  g_sb_active = false;     /* stdout is hooked */
static int   g_sb_decsc = 0;          /* inside DECSC…DECRC (status bar): ignore */
static char  g_sb_seq[48]; static int g_sb_seqn = -1;   /* escape sequence being collected (-1 = none) */
static bool  g_sb_cr = false;         /* a CR was seen; \r\n must not clear the line */

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
        if (g_sb_n == g_sb_cap) { g_sb_cap = g_sb_cap ? g_sb_cap * 2 : 256; g_sb = xrealloc(g_sb, sizeof *g_sb * (size_t)g_sb_cap); }
        sb_init(&g_sb[g_sb_n]); sb_append(&g_sb[g_sb_n], "", 0); g_sb_n++;
    }
    if (g_sb_n > SB_MAX_LINES) {   /* drop the oldest quarter */
        int drop = SB_MAX_LINES / 4;
        for (int i = 0; i < drop; i++) sb_free(&g_sb[i]);
        memmove(g_sb, g_sb + drop, sizeof *g_sb * (size_t)(g_sb_n - drop));
        g_sb_n -= drop; g_sb_line -= drop; if (g_sb_line < 0) g_sb_line = 0;
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
        if (g_sb_line + 1 >= g_sb_n) return;
        g_sb_line++; g_sb_col = g_sb_col - vrow * width;
    }
}
static void sb_newline(void) {
    sb_ensure_line();
    if (g_sb_line + 1 < g_sb_n) { g_sb_line++; g_sb_col = 0; return; }   /* (a "\n" after cursor-up: move down) */
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
    if (fin == 'm') { sb_ensure_line(); sb_cut_at_cursor(); sbuf *l = &g_sb[g_sb_line]; sb_putc(l, 27); sb_append(l, q, (size_t)n); return; }
    if (q[1] == '?') return;                                  /* private modes (bracketed paste, alt screen) */
    switch (fin) {
        case 'A': sb_cursor_up(a ? a : 1); break;
        case 'B': sb_cursor_down(a ? a : 1); break;
        case 'C': g_sb_col += a ? a : 1; break;
        case 'D': g_sb_col -= a ? a : 1; if (g_sb_col < 0) g_sb_col = 0; break;
        case 'K': if (a == 2) { int width = sb_width(); if (width < 1) width = 1; g_sb_col = ((g_sb_col == 0 ? 0 : (g_sb_col - 1) / width)) * width; }
                  sb_cut_at_cursor(); break;
        case 'J': if (a == 2) { for (int i = 0; i < g_sb_n; i++) sb_free(&g_sb[i]); g_sb_n = 0; g_sb_line = 0; g_sb_col = 0; }
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

/* stdout hook: every byte goes to fd 1 and, unless paused, into the model above */
static ssize_t sb_write_fn(void *cookie, const char *buf, size_t n) {
    (void)cookie;
    size_t off = 0;
    while (off < n) { ssize_t w = write(STDOUT_FILENO, buf + off, n - off); if (w < 0) { if (errno == EINTR) continue; return off ? (ssize_t)off : -1; } off += (size_t)w; }
    if (!g_sb_pause) sb_feed(buf, n);
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
static void sb_pause(bool on) { if (!g_sb_active) return; fflush(stdout); g_sb_pause = on; }
/* record text that was drawn while paused (the editor's submitted line) */
static void sb_note(const char *text) { if (g_sb_active) { fflush(stdout); sb_feed(text, strlen(text)); } }

/* visual rows of the buffer at the given width: (line, byte offset, byte length) triples */
typedef struct { int line; size_t off, len; } sb_row;
static sb_row *sb_rows(int width, int *count) {
    if (width < 1) width = 1;
    int cap = 256, n = 0; sb_row *r = xmalloc(sizeof *r * (size_t)cap);
    for (int i = 0; i < g_sb_n; i++) {
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

static const char *g_bar_override = NULL;   /* the viewer's bar text */
static int read_key(void);
static void scroll_view(void) {
    if (!g_fs || !g_sb_active) return;
    sb_pause(true);
    int top = -1;   /* -1 = not yet placed: start one page above the bottom (that is what PgUp asked for) */
    for (;;) {
        layout_sync();
        int rows = g_fs_rows - 1, cols = g_fs_cols; if (rows < 1) rows = 1;
        int nrows; sb_row *r = sb_rows(cols, &nrows);
        int maxtop = nrows > rows ? nrows - rows : 0;
        if (top < 0) top = maxtop - (rows - 1);
        if (top < 0) top = 0;
        if (top > maxtop) top = maxtop;
        sbuf o; sb_init(&o);
        for (int i = 0; i < rows; i++) {
            sb_printf(&o, "\x1b[%d;1H", i + 1);
            int idx = top + i;
            if (idx < nrows) { sb_append(&o, g_sb[r[idx].line].data + r[idx].off, r[idx].len); sb_puts(&o, C_RESET); }
            sb_puts(&o, "\x1b[K");
        }
        char hint[160];
        snprintf(hint, sizeof hint, "↑ scrollback · rows %d-%d of %d · PgUp/PgDn ↑/↓ scroll · End/Esc/Enter back", nrows ? top + 1 : 0, top + rows < nrows ? top + rows : nrows, nrows);
        g_bar_override = hint;
        bar_append(&o);
        g_bar_override = NULL;
        fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o);
        int k = read_key();
        if (k == -2) { if (g_winch) g_winch = 0; free(r); continue; }
        if (k == K_PGUP) top -= rows - 1;
        else if (k == K_UP || k == 'k') top -= 1;
        else if (k == K_PGDN || k == ' ') top += rows - 1;
        else if (k == K_DOWN || k == 'j') top += 1;
        else if (k == K_HOME || k == 'g') top = 0;
        else top = maxtop;   /* End, Esc, Enter, q, anything else: back to the prompt */
        if (top < 0) top = 0;
        free(r);
        if (top >= maxtop) break;   /* scrolled back down to the live view: leave the viewer */
    }
    /* back: redraw the tail of the buffer from the top of the region and leave the cursor on
     * the row after it — the caller (the editor) redraws its prompt there */
    int rows = g_fs_rows - 1, cols = g_fs_cols; if (rows < 1) rows = 1;
    int nrows; sb_row *r = sb_rows(cols, &nrows);
    if (nrows && g_sb_n && g_sb[g_sb_n - 1].len == 0) nrows--;   /* the cursor sits on an empty last line: that is the prompt's row */
    int show = nrows < rows - 1 ? nrows : rows - 1;   /* leave a row for the prompt */
    sbuf o; sb_init(&o);
    sb_puts(&o, "\x1b[H\x1b[2J");
    for (int i = 0; i < show; i++) {
        int idx = nrows - show + i;
        sb_printf(&o, "\x1b[%d;1H", i + 1);
        sb_append(&o, g_sb[r[idx].line].data + r[idx].off, r[idx].len); sb_puts(&o, C_RESET);
    }
    sb_printf(&o, "\x1b[%d;1H", show + 1);
    bar_append(&o);
    fwrite(o.data, 1, o.len, stdout); fflush(stdout); sb_free(&o); free(r);
    sb_pause(false);
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
 * rounds, or right after the turn ends). Text without Enter stays type-ahead and
 * simply reappears in the editor. */
#define QUEUE_MAX 32
static char *g_queue[QUEUE_MAX];
static bool  g_ta_paste = false;     /* inside a bracketed paste (ESC[200~ … ESC[201~) */
static unsigned char g_ta_tail[6];   /* last bytes appended, to spot the paste brackets */

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

/* Turn raw keystrokes typed while busy into message text: printable bytes are
 * kept, backspace deletes, Ctrl-U clears the line, Ctrl-W deletes a word, escape
 * sequences (arrows, paste brackets, alt+key) are dropped, Alt+Enter / Ctrl-J /
 * CR inside a paste become newlines. Returns malloc'd, trimmed text (may be ""). */
char *term_keys_to_text(const unsigned char *b, size_t n) {
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
    /* trim */
    size_t st = 0; while (st < o.len && (o.data[st] == ' ' || o.data[st] == '\t' || o.data[st] == '\n')) st++;
    size_t en = o.len; while (en > st && (o.data[en-1] == ' ' || o.data[en-1] == '\t' || o.data[en-1] == '\n')) en--;
    char *r = xstrndup(o.data + st, en - st);
    sb_free(&o);
    return r;
}

static char *ta_pending_text(void) {
    if (g_ta_pos >= g_ta_len) return NULL;
    char *t = term_keys_to_text(g_ta + g_ta_pos, g_ta_len - g_ta_pos);
    if (!*t) { free(t); return NULL; }
    return t;
}

/* Enter pressed while busy: the pending type-ahead becomes a queued message. */
static void ta_submit(void) {
    size_t n = g_ta_len - g_ta_pos;
    char *text = term_keys_to_text(g_ta + g_ta_pos, n);
    g_ta_pos = g_ta_len = 0;
    if (*text) { term_queue_push(text); term_status_refresh(); }
    free(text);
}

/* Append a byte typed while busy; Enter (outside a paste, not Alt+Enter, not after
 * a trailing backslash) submits the pending text as a queued message. */
static void ta_type(unsigned char c) {
    memmove(g_ta_tail, g_ta_tail + 1, sizeof g_ta_tail - 1); g_ta_tail[sizeof g_ta_tail - 1] = c;
    if (!memcmp(g_ta_tail, "\x1b[200~", 6)) g_ta_paste = true;
    else if (!memcmp(g_ta_tail, "\x1b[201~", 6)) g_ta_paste = false;
    if (c == '\r' && !g_ta_paste && g_cfg.interactive) {
        size_t n = g_ta_len - g_ta_pos;
        bool alt = n > 0 && g_ta[g_ta_len - 1] == 27;                          /* ESC CR = Alt+Enter */
        bool bs  = n > 0 && g_ta[g_ta_len - 1] == '\\';                        /* trailing \ = newline */
        if (!alt && !bs) { ta_submit(); return; }
        if (bs) { g_ta_len--; c = '\n'; }
    }
    /* Shift+Tab (CSI Z) while busy: cycle the permission mode right away, like in the
     * editor, instead of replaying it after the turn. Applies to the tool confirmations
     * still to come in this turn. */
    if (c == 'Z' && g_ta_len - g_ta_pos >= 2 && g_ta[g_ta_len - 2] == 27 && g_ta[g_ta_len - 1] == '[' && !g_ta_paste) {
        g_ta_len -= 2;
        g_cfg.mode = (g_cfg.mode + 1) % MODE_COUNT;
        term_status_refresh();
        return;
    }
    ta_put(c);
    if (g_fs && g_cfg.interactive && (c >= 32 || c == 127 || c == 8 || c == 21 || c == 23 || c == '\n')) term_status_refresh();
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
    for (const char *p = text; *p; p++) ta_put((unsigned char)*p);
}

int term_poll_interrupt(void) {
    unsigned char kb[256];
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
        struct timeval tv = { 0, 0 };
        if (select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv) <= 0) return 0;
        ssize_t k = read(STDIN_FILENO, kb, sizeof kb);
        if (k <= 0) return 0;
        for (ssize_t i = 0; i < k; i++) {
            if (kb[i] == 3) return 1;                       /* Ctrl-C */
            if (kb[i] == 27 && i == k - 1) {                /* bare Esc, or a split escape sequence? */
                fd_set rf2; FD_ZERO(&rf2); FD_SET(STDIN_FILENO, &rf2);
                struct timeval tv2 = { 0, 40 * 1000 };
                if (select(STDIN_FILENO + 1, &rf2, NULL, NULL, &tv2) <= 0) return 1;
                ta_type(kb[i]);   /* more bytes follow: it is a sequence, keep it */
                continue;
            }
            ta_type(kb[i]);
        }
    }
}

static int read_byte_timeout(int ms) {
    int t = ta_get(); if (t >= 0) return t;
    fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv);
    if (r <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

/* Reads one key. Printable bytes returned as-is (0..255); specials as K_*.
 * Returns -1 on EOF/error, -2 on EINTR (e.g. window resize). */
static int read_key(void) {
    unsigned char c;
    ssize_t n;
    int t = ta_get();
    if (t >= 0) { c = (unsigned char)t; n = 1; }
    else {
        do { n = read(STDIN_FILENO, &c, 1); } while (n < 0 && errno == EINTR && !g_winch);
        if (n < 0 && errno == EINTR) return -2;
        if (n <= 0) return -1;
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
    char params[16] = {0}; int pl = 0; int b;
    for (;;) {
        b = read_byte_timeout(40);
        if (b < 0) return K_ESC;
        if ((b >= '0' && b <= '9') || b == ';') { if (pl < 15) params[pl++] = (char)b; continue; }
        break;
    }
    switch (b) {
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

int term_getkey(void) {
    term_raw(true);
    int k;
    do { k = read_key(); } while (k == -2);
    return k;
}

/* ---------- history ---------- */
#define HIST_MAX 100   /* the latest 100 queries are kept (and persisted) */
static char *g_hist[HIST_MAX];
static int g_hist_n = 0;

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
}

void hist_save(void) {
    FILE *f = fopen(hist_path(), "w");
    if (!f) return;
    for (int i = 0; i < g_hist_n; i++) {
        for (const char *c = g_hist[i]; *c; c++) fputc(*c == '\n' ? 0x1f : *c, f);
        fputc('\n', f);
    }
    fclose(f);
}

/* ---------- slash completion ---------- */
static const char **g_cmds = NULL; static int g_ncmds = 0;
void term_set_slash_commands(const char **cmds, int n) { g_cmds = cmds; g_ncmds = n; }

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

/* ---------- editor ---------- */
typedef struct {
    sbuf buf;
    size_t cur;          /* byte offset of cursor */
    const char *prompt;
    int plen;
    int prev_cursor_row; /* row (relative to first line) where cursor was after last refresh */
    int hist_idx;        /* g_hist_n == not browsing */
    char *hist_saved;    /* current line saved when browsing */
} editor;

/* compute (row,col) at byte offset `upto`, walking from prompt. Returns via out params. */
static void ed_pos(editor *e, size_t upto, int width, int *row, int *col) {
    int r = 0, c = e->plen;
    if (c >= width) { r = c / width; c %= width; }
    for (size_t i = 0; i < upto && i < e->buf.len; ) {
        unsigned char ch = (unsigned char)e->buf.data[i];
        if (ch == '\n') { r++; c = 0; i++; continue; }
        c++;
        if (c >= width) { r++; c = 0; }
        i = u8_next(e->buf.data, i, e->buf.len);
    }
    *row = r; *col = c;
}

static void ed_refresh(editor *e) {
    layout_sync();
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
    bar_append(&o);   /* the "\x1b[J" above wiped the status bar; the mode may have changed too */
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

static void ed_complete(editor *e) {
    if (!e->buf.len || e->buf.data[0] != '/' || memchr(e->buf.data, ' ', e->buf.len)) return;
    const char *match = NULL; int nm = 0;
    sbuf all; sb_init(&all);
    for (int i = 0; i < g_ncmds; i++) {
        if (!strncmp(g_cmds[i], e->buf.data, e->buf.len)) { nm++; if (!match) match = g_cmds[i]; sb_printf(&all, "%s  ", g_cmds[i]); }
    }
    if (nm == 1) { ed_set(e, match); ed_insert(e, " ", 1); }
    else if (nm > 1) {
        /* print candidates below, then redraw */
        printf("\n" C_DIM "%s" C_RESET "\n", all.data);
        e->prev_cursor_row = 0;
        /* extend to common prefix */
        size_t cp = strlen(match);
        for (int i = 0; i < g_ncmds; i++) if (!strncmp(g_cmds[i], e->buf.data, e->buf.len)) {
            size_t k = 0; while (k < cp && g_cmds[i][k] == match[k]) k++; cp = k;
        }
        char *pref = xstrndup(match, cp); ed_set(e, pref); free(pref);
    }
    sb_free(&all);
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
    if (keep && idx < g_hist_n) e->hist_idx = idx;
    free(orig);
}

static char *readline_impl(const char *prompt) {
    editor e; memset(&e, 0, sizeof e);
    sb_init(&e.buf); sb_append(&e.buf, "", 0);
    e.prompt = prompt; e.plen = vis_width(prompt);
    e.hist_idx = g_hist_n;
    term_raw(true);
    fputs("\x1b[?2004h", stdout);   /* bracketed paste on */
    ed_refresh(&e);
    char *result = NULL;
    bool done = false;
    int ctrlc_count = 0;
    bool esc_pending = false;   /* Esc Esc on an empty line = /rewind (like Claude Code) */
    while (!done) {
        int k = read_key();
        if (k == -2) { if (g_winch) { g_winch = 0; ed_refresh(&e); } continue; }
        if (k == -1) { result = NULL; break; }
        if (k != 3) ctrlc_count = 0;
        if (k == K_ESC && e.buf.len == 0) {
            if (esc_pending) { result = xstrdup("/rewind"); done = true; break; }
            esc_pending = true;
            printf("\n" C_DIM "(press Esc again to rewind the conversation / files)" C_RESET "\n");
            e.prev_cursor_row = 0;
            ed_refresh(&e);
            continue;
        }
        esc_pending = false;
        switch (k) {
            case '\r':
                if (e.buf.len && e.buf.data[e.buf.len - 1] == '\\') {   /* trailing backslash = newline */
                    e.buf.data[e.buf.len - 1] = '\n'; e.cur = e.buf.len; break;
                }
                result = xstrndup(e.buf.data, e.buf.len); done = true; break;
            case '\n': case K_ALT_ENTER: ed_insert(&e, "\n", 1); break;
            case 3:   /* Ctrl-C */
                if (e.buf.len) { ed_set(&e, ""); }
                else {
                    ctrlc_count++;
                    if (ctrlc_count >= 2) { result = NULL; done = true; break; }
                    printf("\n" C_DIM "(press Ctrl-C again or Ctrl-D to exit, /help for help)" C_RESET "\n");
                    e.prev_cursor_row = 0;
                }
                break;
            case 4:   /* Ctrl-D */
                if (e.buf.len == 0) { result = NULL; done = true; }
                else ed_delete_range(&e, e.cur, u8_next(e.buf.data, e.cur, e.buf.len));
                break;
            case 127: case 8:   /* backspace */
                if (e.cur > 0) { size_t p = u8_prev(e.buf.data, e.cur); ed_delete_range(&e, p, e.cur); e.cur = p; }
                break;
            case K_DEL: ed_delete_range(&e, e.cur, u8_next(e.buf.data, e.cur, e.buf.len)); break;
            case K_LEFT: case 2: e.cur = u8_prev(e.buf.data, e.cur); break;
            case K_RIGHT: case 6: e.cur = u8_next(e.buf.data, e.cur, e.buf.len); break;
            case K_HOME: case 1: {
                size_t p = e.cur; while (p > 0 && e.buf.data[p-1] != '\n') p--; e.cur = p; break; }
            case K_END: case 5: {
                size_t p = e.cur; while (p < e.buf.len && e.buf.data[p] != '\n') p++; e.cur = p; break; }
            case K_ALT_B: case K_CTRL_LEFT: e.cur = word_left(&e); break;
            case K_ALT_F: case K_CTRL_RIGHT: e.cur = word_right(&e); break;
            case 23: case K_ALT_BS: { size_t p = word_left(&e); ed_delete_range(&e, p, e.cur); e.cur = p; break; }  /* Ctrl-W */
            case 11: ed_delete_range(&e, e.cur, e.buf.len); break;   /* Ctrl-K */
            case 21: ed_delete_range(&e, 0, e.cur); e.cur = 0; break;   /* Ctrl-U */
            case 12: term_clear_screen(); e.prev_cursor_row = 0; break;   /* Ctrl-L */
            case 18: ed_hist_search(&e); break;                             /* Ctrl-R */
            case '\t': ed_complete(&e); break;
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
                if (q.len) ed_insert(&e, q.data, q.len);
                sb_free(&p); sb_free(&q);
                break; }
            case K_PGUP: scroll_view(); e.prev_cursor_row = 0; break;
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
        if (!done) ed_refresh(&e);
    }
    /* move cursor to end of input and newline */
    {
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

char *term_readline(const char *prompt) { return readline_impl(prompt); }
char *term_ask_line(const char *prompt) { return readline_impl(prompt); }

/* ---------- confirmation menu ---------- */
int term_confirm(const char *question, const char *always_label, const char *project_label, char **reason) {
    if (reason) *reason = NULL;
    term_raw(true);
    ta_stash stash = ta_take();
    const char *labels[4] = { "Yes", always_label ? always_label : "Yes, and don't ask again this session",
                              project_label ? project_label : "No, and tell the model what to do instead",
                              "No, and tell the model what to do instead" };
    const char *keys[4] = { "y", "a", project_label ? "p" : "n", "n" };
    int nopt = project_label ? 4 : 3, no_i = nopt - 1;
    int sel = 0, drawn = 0, choice = -1;
    sb_pause(true);   /* the menu is transient; only its collapsed final line is conversation content */
    for (;;) {
        layout_sync();
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
        bar_append(&o);
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
    printf("  " C_YELLOW "%s" C_RESET "  %s\n", question,
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
void md_init(md_state *m) { memset(m, 0, sizeof *m); m->at_line_start = true; }

static void md_flush_ticks(md_state *m) {
    int t = m->pending_ticks;
    m->pending_ticks = 0;
    if (t == 0) return;
    if (t >= 3 && m->at_line_start) {
        m->in_fence = !m->in_fence;
        fputs(m->in_fence ? C_GRAY "```" : "```" C_RESET, stdout);
        if (m->in_fence) fputs(C_RESET C_CYAN, stdout);
        for (int i = 3; i < t; i++) fputc('`', stdout);
        return;
    }
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
    if (m->pending_star) { fputc('*', stdout); m->pending_star = false; }
}

void md_feed(md_state *m, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '`') { md_flush_star(m); m->pending_ticks++; continue; }
        if (m->pending_ticks) { md_flush_ticks(m); m->at_line_start = false; }
        if (!m->in_fence && !m->in_code) {
            if (c == '*') {
                if (m->pending_star) { m->pending_star = false; m->in_bold = !m->in_bold; fputs(m->in_bold ? C_BOLD : C_RESET, stdout); continue; }
                m->pending_star = true; continue;
            }
            md_flush_star(m);
            if (m->at_line_start && c == '#') { fputs(C_BOLD, stdout); m->in_bold = true; }
        }
        fputc(c, stdout);
        if (c == '\n') {
            m->at_line_start = true;
            if (m->in_bold && !m->in_fence) { m->in_bold = false; fputs(C_RESET, stdout); if (m->in_code) fputs(C_CYAN, stdout); }
            if (m->in_fence) fputs(C_CYAN, stdout);
        } else if (c != ' ' && c != '\t') m->at_line_start = false;
    }
    fflush(stdout);
}

void md_finish(md_state *m) {
    md_flush_star(m);
    if (m->pending_ticks) md_flush_ticks(m);
    fputs(C_RESET, stdout);
    m->in_fence = m->in_code = m->in_bold = false;
    fflush(stdout);
}

/* ---------- interactive selector (used by /model) ---------- */
int term_select(const char *title, const char **items, const char **descs, int n, int current) {
    if (n <= 0) return -1;
    term_raw(true);
    int sel = current >= 0 && current < n ? current : 0;
    char filter[128] = {0}; int flen = 0;
    int *vis = xmalloc(sizeof(int) * (size_t)n); int nvis = 0;
    int drawn = 0;            /* lines drawn last time */
    int max_show = 12;
    int top = 0;
    int result = -1;
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
        layout_sync();
        sbuf o; sb_init(&o);
        if (drawn) sb_printf(&o, "\x1b[%dA", drawn);
        sb_puts(&o, "\r\x1b[J");
        int lines = 0;
        sb_printf(&o, C_BOLD "%s" C_RESET "  " C_DIM "type to filter · ↑/↓ move · enter select · esc cancel" C_RESET "\n", title); lines++;
        sb_printf(&o, "  " C_DIM "filter:" C_RESET " %s" C_DIM "▏" C_RESET "\n", filter); lines++;
        int w = term_width();
        for (int i = top; i < nvis && i < top + max_show; i++) {
            int idx = vis[i];
            bool is_sel = (i == selpos);
            char line[512];
            snprintf(line, sizeof line, "%s%-44s %s", idx == current ? "* " : "  ", items[idx], descs && descs[idx] ? descs[idx] : "");
            if ((int)strlen(line) > w - 4) line[w - 4] = 0;
            sb_printf(&o, "%s%s %s" C_RESET "\n", is_sel ? C_ORANGE "❯" : " ", is_sel ? C_BOLD : "", line); lines++;
        }
        if (nvis == 0) { sb_puts(&o, C_DIM "  (no matches)" C_RESET "\n"); lines++; }
        if (nvis > max_show) { sb_printf(&o, C_DIM "  … %d of %d shown" C_RESET "\n", max_show < nvis ? max_show : nvis, nvis); lines++; }
        drawn = lines;
        bar_append(&o);
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
