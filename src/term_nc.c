/* term_nc.c — the notcurses frontend (built when notcurses is installed; see AGENTS.md "The notcurses frontend").
 *
 * The conversation is the scrollback model (term.c): what the program prints is captured, and this
 * file draws it into a pane, parsing the SGR colours the ANSI UI would have written. The field, the
 * status bar and the location line are planes of their own, drawn from the same text the ANSI
 * bar is built from (term_status_text). Menus (confirm, select) are overlays on the same screen.
 *
 * The first half of this file is pure (no notcurses types): the SGR parser, the palette and the
 * cell tokeniser. test_unit links it in both builds. */
#define _GNU_SOURCE   /* notcurses.h declares wcwidth() behind it */
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#ifdef WITH_NOTCURSES
#include <notcurses/notcurses.h>
#endif

/* ---------- pure: palette, SGR, cells ---------- */
/* the 16 base colours (a calm dark palette: the ANSI UI's colours, softened) */
static const unsigned NC_BASE[16] = {
    0x2e3440, 0xbf616a, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead, 0x88c0d0, 0xd8dee9,
    0x4c566a, 0xe06c75, 0xb5d68a, 0xf0c674, 0x81a1c1, 0xc792ea, 0x8fbcbb, 0xeceff4 };

unsigned nc_color256(int n) {
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    if (n < 16) return NC_BASE[n];
    if (n < 232) {
        static const int L[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return (unsigned)((L[n / 36] << 16) | (L[(n / 6) % 6] << 8) | L[n % 6]);
    }
    unsigned v = (unsigned)(8 + 10 * (n - 232));
    return (v << 16) | (v << 8) | v;
}

size_t nc_sgr_parse(const char *s, size_t n, nc_style *st) {
    if (n < 2 || s[0] != 0x1b || s[1] != '[') return 0;
    if (n > 2 && strchr("?<=>", s[2])) {   /* private sequences (modes) are not SGR: skip them */
        size_t i = 3;
        while (i < n && (s[i] < 0x40 || s[i] > 0x7e)) i++;
        return i < n ? i + 1 : n;
    }
    size_t i = 2;
    while (i < n && (s[i] < 0x40 || s[i] > 0x7e)) i++;   /* parameters and intermediates: 0x20..0x3f */
    if (i >= n) return n;                               /* unterminated: the rest is the escape */
    if (s[i] != 'm' || !st) return i + 1;
    int p[32], np = 0, cur = -1;
    for (size_t k = 2; k < i; k++) {
        char c = s[k];
        if (c >= '0' && c <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (c - '0'); if (cur > 9999) cur = 9999; }
        else if (c == ';' || c == ':') { if (np < 32) p[np++] = cur < 0 ? 0 : cur; cur = -1; }
    }
    if (np < 32) p[np++] = cur < 0 ? 0 : cur;
    for (int k = 0; k < np; k++) {
        int v = p[k];
        if (v == 0) { st->fg = st->bg = NC_DEFAULT; st->styles = 0; }
        else if (v == 1) st->styles |= NC_ST_BOLD;
        else if (v == 2) st->styles |= NC_ST_DIM;
        else if (v == 3) st->styles |= NC_ST_ITALIC;
        else if (v == 4) st->styles |= NC_ST_UNDER;
        else if (v == 7) st->styles |= NC_ST_REVERSE;
        else if (v == 22) st->styles &= ~(NC_ST_BOLD | NC_ST_DIM);
        else if (v == 23) st->styles &= ~NC_ST_ITALIC;
        else if (v == 24) st->styles &= ~NC_ST_UNDER;
        else if (v == 27) st->styles &= ~NC_ST_REVERSE;
        else if (v >= 30 && v <= 37) { unsigned rgb; st->fg = theme_fg_code(v, &rgb) ? rgb : NC_BASE[v - 30]; }
        else if (v == 39) st->fg = NC_DEFAULT;
        else if (v >= 40 && v <= 47) st->bg = NC_BASE[v - 40];
        else if (v == 49) st->bg = NC_DEFAULT;
        else if (v >= 90 && v <= 97) { unsigned rgb; st->fg = theme_fg_code(v, &rgb) ? rgb : NC_BASE[v - 90 + 8]; }
        else if (v >= 100 && v <= 107) st->bg = NC_BASE[v - 100 + 8];
        else if (v == 38 || v == 48) {
            unsigned *dst = v == 38 ? &st->fg : &st->bg;
            if (k + 2 < np && p[k + 1] == 5) { *dst = v == 38 && p[k + 2] == 208 ? g_theme->accent : nc_color256(p[k + 2]); k += 2; }
            else if (k + 4 < np && p[k + 1] == 2) {
                *dst = ((unsigned)(p[k + 2] & 255) << 16) | ((unsigned)(p[k + 3] & 255) << 8) | (unsigned)(p[k + 4] & 255);
                k += 4;
            } else break;   /* malformed: the rest is not ours to guess at */
        }
    }
    return i + 1;
}

/* one character of the conversation, as drawn: its UTF-8 (a base character and its combining marks) */
static size_t utf8_len(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
}

/* Columns of a code point: wide East Asian and emoji take two, combining marks none. A table, not
 * wcwidth(): that depends on the process locale, and the parser has to give the same answer in
 * every build and every test. */
static int cp_width(unsigned cp) {
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x20D0 && cp <= 0x20FF)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) return 2;
    return 1;
}
/* the code point at s (len bytes, a whole sequence), or 0xFFFD when it is not valid UTF-8 */
static unsigned utf8_cp(const char *s, size_t len) {
    const unsigned char *p = (const unsigned char *)s;
    if (len == 1) return p[0];
    unsigned cp = len == 2 ? (p[0] & 0x1F) : len == 3 ? (p[0] & 0x0F) : (p[0] & 0x07);
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    return cp;
}

int nc_cells(const char *s, size_t n, nc_cell *out, int max, nc_style *st) {
    int cnt = 0;
    size_t i = 0;
    nc_style local = { NC_DEFAULT, NC_DEFAULT, 0 };
    if (!st) st = &local;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) { size_t u = nc_sgr_parse(s + i, n - i, st); i += u ? u : 1; continue; }
        if (c == '\t') {   /* to the next tab stop, in cells of this line */
            int stop = 8 - (cnt % 8);
            for (int k = 0; k < stop; k++, cnt++) if (out && cnt < max) { memcpy(out[cnt].egc, " ", 2); out[cnt].w = 1; out[cnt].st = *st; }
            i++; continue;
        }
        if (c < 0x20 || c == 0x7f) { i++; continue; }   /* CR, BEL and the rest are not drawn */
        size_t len = utf8_len(c);
        if (i + len > n) len = n - i;
        unsigned cp = utf8_cp(s + i, len);
        int cw = cp == 0xFFFD ? 1 : cp_width(cp);
        if (cw == 0 && cnt > 0) {   /* a combining mark belongs to the cell before it */
            if (out && cnt - 1 < max) { size_t have = strlen(out[cnt - 1].egc); if (have + len < sizeof out[0].egc) memcpy(out[cnt - 1].egc + have, s + i, len), out[cnt - 1].egc[have + len] = 0; }
            i += len; continue;
        }
        if (cw == 0) cw = 1;
        if (out && cnt < max) {
            memcpy(out[cnt].egc, s + i, len); out[cnt].egc[len] = 0;
            out[cnt].w = (unsigned char)cw; out[cnt].st = *st;
        }
        cnt++; i += len;
    }
    return cnt;
}

int nc_line_rows(const nc_cell *c, int n, int width) {
    if (width <= 0) return 1;
    int rows = 1, col = 0;
    for (int i = 0; i < n; i++) {
        if (col + c[i].w > width && col > 0) { rows++; col = 0; }
        col += c[i].w;
    }
    return rows;
}

int nc_text_width(const char *s) {
    int w = 0;
    size_t n = strlen(s);
    nc_cell *tmp = xmalloc(sizeof *tmp * (n + 1));
    int cnt = nc_cells(s, n, tmp, (int)n, NULL);
    for (int i = 0; i < cnt; i++) w += tmp[i].w;
    free(tmp);
    return w;
}

#ifndef WITH_NOTCURSES
/* ---------- the stubs: this build has no notcurses frontend ---------- */
bool   nc_compiled(void) { return false; }
bool   nc_start(void) { fprintf(stderr, "corbienest: this build has no notcurses frontend (notcurses was not installed when it was built)\n"); return false; }
void   nc_stop(void) {}
bool   nc_size(int *rows, int *cols) { (void)rows; (void)cols; return false; }
void   nc_render(void) {}
void   nc_render_soon(void) {}
void   nc_banner_set(const char **lines, int n, int bw, bool on) { (void)lines; (void)n; (void)bw; (void)on; }
bool   nc_banner_open(void) { return false; }
void   nc_overlay_set(const char *text) { (void)text; }
bool   nc_overlay_open(void) { return false; }
void   nc_output(void) {}
void   nc_clear(void) {}
void   nc_prefill(const char *text) { (void)text; }
char  *nc_readline(const char *prompt, bool in_field) { (void)prompt; (void)in_field; return NULL; }
int    nc_poll_interrupt(void) { return 0; }
int    nc_input_fd(void) { return -1; }
int    nc_confirm(const char *question, const char *always_label, const char *project_label, char **reason) {
    (void)question; (void)always_label; (void)project_label; if (reason) *reason = NULL; return 0;
}
int    nc_select(const char *title, const char **items, const char **descs, int n, int current) {
    (void)title; (void)items; (void)descs; (void)n; (void)current; return -1;
}
#else
/* ---------- the notcurses frontend ---------- */
bool nc_compiled(void) { return true; }

/* the frame's colours (0xRRGGBB) */
/* the palette: the current theme's (theme.c); the ANSI screen draws the same colours from the same table */
#define C_TEXT    (g_theme->text)
#define C_MENTION (g_theme->info)     /* a @file that is there, in the field: the cyan the ANSI screen maps 36 to */
#define C_ACCENT  (g_theme->accent)   /* the orange of the prompt, the menus and the busy mark */
#define C_MUTED   (g_theme->muted)
#define C_RULE    (g_theme->rule)
#define C_MENU    (g_theme->menu)     /* a menu's background */
#define C_BAR_A   (g_theme->bar_a)    /* the status bar: a gradient from here … */
#define C_BAR_B   (g_theme->bar_b)    /* … to here */
#define C_FIELD_TEXT (g_theme->field_text)   /* what is typed in the field: a light yellow on a darker ground */
#define C_FIELD_BG   (g_theme->field_bg)
#define C_OK      (g_theme->ok)
#define C_BAD     (g_theme->bad)
#define C_WARN    (g_theme->warn)

#define FIELD_MAX 6            /* most text rows the field grows to */
#define REDRAW_MS 30           /* output that arrives faster than this is drawn once per interval */
#define NK_NONE  (-1)          /* no key within the wait */
#define NK_SKIP  (-2)          /* an event nothing acts on (a release, a mouse report) */
#define NK_UP    (-10)
#define NK_DOWN  (-11)
#define NK_LEFT  (-12)
#define NK_RIGHT (-13)
#define NK_HOME  (-14)
#define NK_END   (-15)
#define NK_DEL   (-16)
#define NK_PGUP  (-17)
#define NK_PGDN  (-18)
#define NK_RESIZE (-19)
#define NK_PASTE (-20)         /* a bracketed paste, its text taken from the paste FIFO (edit_key inserts it) */

static struct notcurses *g_nc = NULL;
static FILE *g_nc_out = NULL;          /* notcurses' own stream: a dup of fd 1, never the captured stdout */
static struct ncplane *g_std = NULL;
static int g_rows = 0, g_cols = 0, g_field_top = 0;
static int g_top = 0;                  /* the pane: rows scrolled back from the newest */
static int *g_pane_line = NULL;        /* per pane row, the scrollback line painted there (-1: none, e.g. a suggestion's row): a click finds its fold */
static int g_pane_cap = 0;
static sbuf *g_bn_lines = NULL;       /* the banner's modal: its lines (term.c's banner_store keeps the same for the ANSI screen) */
static int g_bn_n = 0, g_bn_bw = 0, g_bn_xr = -1, g_bn_xc0 = -1, g_bn_xc1 = -1;   /* its size, and the close button's cell */
static bool g_bn_on = false;
static sbuf *g_ov_lines = NULL;       /* the overlay (term_overlay_show): its text as lines, drawn in a window over the pane */
static int g_ov_n = 0, g_ov_w = 0;
static bool g_ov_on = false;
static void nc_banner_close(void);
static int g_pane_epoch = -1;          /* term_sb_epoch() when g_pane_line was made; -1: not made (no click acts on it) */
static int g_clear_at = 0;             /* lines before this one are not shown (/clear) */
static bool g_pending = false;         /* output arrived that is not drawn yet */
static double g_last_draw = 0;

/* the editor: the line being typed (UTF-8), its cursor (a byte offset) and the history position */
static char *g_ed = NULL;
static size_t g_len = 0, g_cap = 0, g_cur = 0;
static int g_hist_pos = 0;             /* an index into the history; hist_count() = the line being typed */
static char *g_draft = NULL;           /* what was typed before the history was walked */
static char *g_pre = NULL;             /* text for the next prompt (term_editor_prefill) */

/* a menu over the screen (confirm, select) */
#define MENU_ROWS_MAX 32
typedef struct { char text[512]; unsigned fg; bool sel; } menu_row;
static bool g_menu_on = false;
static char g_menu_title[256], g_menu_hint[256];
static menu_row g_menu[MENU_ROWS_MAX];
static int g_menu_n = 0;

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------- the editor's buffer ---------- */
static void ed_reserve(size_t n) {
    if (n + 1 <= g_cap) return;
    size_t cap = g_cap ? g_cap : 64;
    while (cap < n + 1) cap *= 2;
    g_ed = xrealloc(g_ed, cap);
    g_cap = cap;
}
static void ed_set(const char *s) {
    size_t n = s ? strlen(s) : 0;
    ed_reserve(n);
    if (n) memcpy(g_ed, s, n);
    g_len = g_cur = n;
    g_ed[g_len] = 0;
}
static void ed_insert(const char *s, size_t n) {
    ed_reserve(g_len + n);
    memmove(g_ed + g_cur + n, g_ed + g_cur, g_len - g_cur);
    memcpy(g_ed + g_cur, s, n);
    g_len += n; g_cur += n; g_ed[g_len] = 0;
}
static size_t ed_prev(size_t at) {   /* the start of the character before `at` */
    if (at == 0) return 0;
    at--;
    while (at > 0 && ((unsigned char)g_ed[at] & 0xC0) == 0x80) at--;
    return at;
}
static size_t ed_next(size_t at) {
    if (at >= g_len) return g_len;
    at++;
    while (at < g_len && ((unsigned char)g_ed[at] & 0xC0) == 0x80) at++;
    return at;
}
static void ed_delete(size_t from, size_t to) {
    memmove(g_ed + from, g_ed + to, g_len - to);
    g_len -= to - from; g_cur = from; g_ed[g_len] = 0;
}
static size_t ed_line_start(size_t at) { while (at > 0 && g_ed[at - 1] != '\n') at--; return at; }
static size_t ed_line_end(size_t at) { while (at < g_len && g_ed[at] != '\n') at++; return at; }
static int utf8_put(unsigned cp, char *b) {
    if (cp < 0x80) { b[0] = (char)cp; return 1; }
    if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---------- keys ---------- */
static int nc_key_code(const ncinput *ni) {
    if (ni->evtype == NCTYPE_RELEASE) return NK_SKIP;
    switch (ni->id) {
        case NCKEY_RESIZE:    return NK_RESIZE;
        case NCKEY_UP:        return NK_UP;
        case NCKEY_DOWN:      return NK_DOWN;
        case NCKEY_LEFT:      return NK_LEFT;
        case NCKEY_RIGHT:     return NK_RIGHT;
        case NCKEY_HOME:      return NK_HOME;
        case NCKEY_END:       return NK_END;
        case NCKEY_DEL:       return NK_DEL;
        case NCKEY_PGUP:      return NK_PGUP;
        case NCKEY_PGDOWN:    return NK_PGDN;
        case NCKEY_ENTER:     return 13;
        case NCKEY_BACKSPACE: return 127;
        case NCKEY_ESC:       return 27;
        case NCKEY_TAB:       return 9;
        default: break;
    }
    if (ni->id >= 0x110000u || ni->id == 0) return NK_SKIP;   /* other special keys */
    /* Ctrl+letter: the kitty protocol reports it as the capital (Ctrl+a is id 'A' with the Ctrl modifier),
     * so either case; with Alt too it is a character (AltGr on some layouts), not a control key */
    if ((ni->modifiers & NCKEY_MOD_CTRL) && !(ni->modifiers & NCKEY_MOD_ALT) && ni->id < 128 && isalpha((int)ni->id))
        return (int)(tolower((int)ni->id) - 'a' + 1);
    /* the character the key types, which is not the key's id for a shifted key (shift+1 is id '1',
     * typed as '!'; AltGr gives @ { [ …): notcurses has it in eff_text when the terminal sends it */
    if (ni->eff_text[0] >= 32 && ni->eff_text[0] < 0x110000u && ni->eff_text[0] != 127) return (int)ni->eff_text[0];
    return (int)ni->id;
}

/* One key, or NK_NONE after `ms` milliseconds without one. Input comes through notcurses
 * (its own thread fills the pipe that notcurses_inputready_fd() reports). */
/* the strip's key for a notcurses key (term.c's STRIP_*) */
static int strip_code(int k) {
    if (k == NK_UP) return STRIP_UP;
    if (k == NK_DOWN) return STRIP_DOWN;
    if (k == 27) return STRIP_ESC;
    if (k == 13 || k == 10) return STRIP_ENTER;
    return STRIP_OTHER;
}
static int nc_getkey(int ms);
static void nc_flush_due(void);

/* Keys already taken from notcurses and not yet handed out. notcurses signals readiness once for a
 * burst of input (a paste, or keys typed faster than the loop turns), so every event that has arrived
 * is taken here; left in notcurses, the rest would wait for the next key. */
#define KQ_MAX 512
static int g_kq[KQ_MAX];
static int g_kq_head, g_kq_n;

static int kq_take(void) {
    int k = g_kq[g_kq_head];
    g_kq_head = (g_kq_head + 1) % KQ_MAX;
    g_kq_n--;
    return k;
}

/* The rows of scrollback line `i` as the pane lays it out (the same count draw_pane uses). */
static int sb_line_rows(int i) {
    const char *s = term_sb_line(i);
    size_t len = strlen(s);
    int n = nc_cells(s, len, NULL, 0x7fffffff, NULL);
    nc_cell *c = xmalloc(sizeof *c * (size_t)(n ? n : 1));
    nc_style st = { NC_DEFAULT, NC_DEFAULT, 0 };
    n = nc_cells(s, len, c, n, &st);
    int rows = nc_line_rows(c, n, g_cols);
    free(c);
    return rows;
}

/* the wheel: the pane scrolls (draw_pane clamps the far end) */
static void pane_scroll(int rows) { g_top += rows; if (g_top < 0) g_top = 0; nc_render_soon(); }

/* A mouse report: a left click on a fold's head shows or hides that fold; the wheel scrolls the pane;
 * anything else is dropped. Acted on where the keys are read: a fold only changes the scrollback
 * model and the screen, nothing is sent. A click while a menu is open, or on rows that are not the
 * ones last painted (the lines moved since), does nothing. */
static void nc_mouse(const ncinput *ni) {
    if (ni->evtype != NCTYPE_PRESS) return;
    if (g_bn_on && ni->id == NCKEY_BUTTON1 && ni->y == g_bn_xr && ni->x >= g_bn_xc0 && ni->x <= g_bn_xc1) { nc_banner_close(); return; }
    if (g_menu_on) return;
    if (ni->id == NCKEY_SCROLL_UP) { pane_scroll(3); return; }
    if (ni->id == NCKEY_SCROLL_DOWN) { pane_scroll(-3); return; }
    if (ni->id != NCKEY_BUTTON1 || ni->y < 0 || ni->y >= g_field_top || ni->y >= g_pane_cap) return;
    if (g_pane_epoch != term_sb_epoch()) return;
    int line = g_pane_line[ni->y];
    if (line < 0) return;
    int nb = term_fold_toggle(line);   /* the body lines flipped; -1 when the row is no fold's head */
    if (nb < 0) return;
    if (nb > 0 && g_top > 0) {   /* scrolled back: the head keeps its place on the screen, so the rows the body took (or gave back) move the anchor */
        int d = 0;
        for (int i = line + 1; i <= line + nb; i++) d += sb_line_rows(i);
        g_top += term_sb_hidden(line + 1) ? -d : d;
        if (g_top < 0) g_top = 0;
    }
    nc_render_soon();
}

/* ---------- bracketed paste ----------
 * notcurses has no paste API: the terminal's bracketed paste (ESC [200~ … ESC [201~) comes as key events,
 * ESC then '[', '2', … . They are taken here: the text between the markers is one NK_PASTE, which edit_key
 * inserts as text (a pasted newline is a line break, not Enter). Keys that start like a marker are held
 * until they are told apart; a lone Esc is passed on at the end of the drain. */
static const int PM_START[6] = { 27, '[', '2', '0', '0', '~' };
static const int PM_END[6]   = { 27, '[', '2', '0', '1', '~' };
#define PM_MAX 16
static bool g_pm_in;                   /* inside the pasted text */
static int g_pm_held[6], g_pm_nheld;   /* keys that may be a marker, not yet told apart */
static sbuf g_pm_text;                 /* the text of the paste being read */
static char *g_pq[PM_MAX];             /* the pastes, in the order their NK_PASTE keys were queued */
static int g_pq_head, g_pq_n;

static char *paste_take(void) {
    if (!g_pq_n) return NULL;
    char *t = g_pq[g_pq_head];
    g_pq_head = (g_pq_head + 1) % PM_MAX; g_pq_n--;
    return t;
}

/* a key out of the stream: Esc closes the overlay or the banner first, else it is queued */
static void key_out(int k) {
    if (k == NK_SKIP) return;
    if (k == 27 && g_ov_on) { nc_overlay_set(NULL); return; }   /* Esc closes the overlay */
    if (k == 27 && g_bn_on) { nc_banner_close(); return; }      /* Esc closes the banner's modal before anything else */
    if (g_kq_n < KQ_MAX) g_kq[(g_kq_head + g_kq_n++) % KQ_MAX] = k;
}

/* a key of the pasted text: a newline or a return is a line break, a tab a space, other controls are dropped */
static void pm_text(int k) {
    if (k == 13 || k == 10) { sb_putc(&g_pm_text, '\n'); return; }
    if (k == 9) { sb_putc(&g_pm_text, ' '); return; }
    if (k < 32 || k == 127 || k >= 0x110000) return;
    char b[4]; int n = utf8_put((unsigned)k, b);
    sb_append(&g_pm_text, b, (size_t)n);
}

static void pm_queue_paste(void) {
    if (g_pq_n >= PM_MAX || g_kq_n >= KQ_MAX) return;   /* no room: the paste is dropped */
    g_pq[(g_pq_head + g_pq_n++) % PM_MAX] = xstrdup(g_pm_text.data ? g_pm_text.data : "");
    g_kq[(g_kq_head + g_kq_n++) % KQ_MAX] = NK_PASTE;
}

/* takes key k if the paste markers claim it (true); otherwise the caller queues it */
static bool pm_feed(int k) {
    for (;;) {
        if (!g_pm_in) {
            if (g_pm_nheld < 6 && k == PM_START[g_pm_nheld]) {
                g_pm_held[g_pm_nheld++] = k;
                if (g_pm_nheld == 6) { g_pm_nheld = 0; g_pm_in = true; sb_free(&g_pm_text); sb_init(&g_pm_text); }
                return true;
            }
            if (!g_pm_nheld) return false;
            for (int i = 0; i < g_pm_nheld; i++) key_out(g_pm_held[i]);   /* not a marker after all: keys */
            g_pm_nheld = 0;
            continue;                                   /* k again, from the start of the marker */
        }
        if (g_pm_nheld < 6 && k == PM_END[g_pm_nheld]) {
            g_pm_held[g_pm_nheld++] = k;
            if (g_pm_nheld == 6) { g_pm_nheld = 0; g_pm_in = false; pm_queue_paste(); }
            return true;
        }
        if (g_pm_nheld) {                               /* a part of the end marker that was text after all */
            for (int i = 0; i < g_pm_nheld; i++) pm_text(g_pm_held[i]);
            g_pm_nheld = 0;
            continue;
        }
        pm_text(k);
        return true;
    }
}

static int nc_getkey(int ms) {
    if (!g_nc) return NK_NONE;
    if (g_kq_n) return kq_take();
    nc_flush_due();
    int fd = notcurses_inputready_fd(g_nc);
    if (fd < 0) return NK_NONE;
    fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(fd + 1, &rf, NULL, NULL, &tv);
    if (r <= 0) return NK_NONE;
    for (;;) {
        ncinput ni; memset(&ni, 0, sizeof ni);
        uint32_t id = notcurses_get_nblock(g_nc, &ni);
        if (id == 0 || id == (uint32_t)-1) break;
        if (nckey_mouse_p(ni.id)) { nc_mouse(&ni); continue; }   /* a report, not a key */
        if (ni.id == NCKEY_TAB && (ni.modifiers & NCKEY_MOD_SHIFT)) {   /* Shift+Tab cycles the mode at once, as on the ANSI screen (a key, so it is not queued) */
            g_cfg.mode = (g_cfg.mode + 1) % MODE_COUNT;
            nc_render_soon();
            continue;
        }
        int k = nc_key_code(&ni);
        if (k == NK_SKIP || pm_feed(k)) continue;   /* a paste's markers and text are taken by pm_feed */
        key_out(k);
    }
    if (!g_pm_in && g_pm_nheld) {                   /* an Esc on its own, held for a marker: it is a key after all */
        for (int i = 0; i < g_pm_nheld; i++) key_out(g_pm_held[i]);
        g_pm_nheld = 0;
    }
    return g_kq_n ? kq_take() : NK_SKIP;
}

/* ---------- drawing ---------- */
static uint64_t chans(unsigned fg, unsigned bg) {
    uint64_t ch = 0;
    if (fg == NC_DEFAULT) ncchannels_set_fg_default(&ch);
    else ncchannels_set_fg_rgb8(&ch, (fg >> 16) & 255, (fg >> 8) & 255, fg & 255);
    if (bg == NC_DEFAULT) ncchannels_set_bg_default(&ch);
    else ncchannels_set_bg_rgb8(&ch, (bg >> 16) & 255, (bg >> 8) & 255, bg & 255);
    return ch;
}

/* a → b by t/256 (per channel) */
static unsigned mix(unsigned a, unsigned b, int t) {
    unsigned out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int x = (int)((a >> sh) & 255), y = (int)((b >> sh) & 255);
        out |= (unsigned)(x + (y - x) * t / 256) << sh;
    }
    return out;
}

/* Sets the plane's colours for one cell: its own SGR colours where it has them, `fg_def`/`bg_def`
 * (NC_DEFAULT = the terminal's own) where it has not. Reverse video swaps the two. */
static void paint(struct ncplane *p, const nc_style *st, unsigned fg_def, unsigned bg_def) {
    unsigned fg = st->fg == NC_DEFAULT ? fg_def : st->fg;
    unsigned bg = st->bg == NC_DEFAULT ? bg_def : st->bg;
    if (st->styles & NC_ST_REVERSE) { unsigned t = fg; fg = bg; bg = t; }
    if ((st->styles & NC_ST_DIM) && fg != NC_DEFAULT) fg = mix(fg, bg == NC_DEFAULT ? 0 : bg, 128);
    ncplane_set_channels(p, chans(fg, bg));
    unsigned sty = 0;
    if (st->styles & NC_ST_BOLD) sty |= NCSTYLE_BOLD;
    if (st->styles & NC_ST_ITALIC) sty |= NCSTYLE_ITALIC;
    if (st->styles & NC_ST_UNDER) sty |= NCSTYLE_UNDERLINE;
    ncplane_set_styles(p, sty);
}

/* Writes the text `s` (SGR escapes allowed) at (y, x), at most `maxw` columns, with the defaults
 * fg/bg; `grad` paints the background as the status bar's gradient instead. Returns the columns used. */
static int put_text(int y, int x, const char *s, int maxw, unsigned fg, unsigned bg, bool grad, nc_style *carry) {
    size_t n = strlen(s);
    int cnt = nc_cells(s, n, NULL, 0x7fffffff, NULL);
    nc_cell *c = xmalloc(sizeof *c * (size_t)(cnt ? cnt : 1));
    cnt = nc_cells(s, n, c, cnt, carry);
    int col = 0;
    for (int j = 0; j < cnt && col + c[j].w <= maxw; j++) {
        unsigned b = grad ? mix(C_BAR_A, C_BAR_B, (int)((long)(x + col) * 256 / (g_cols > 0 ? g_cols : 1))) : bg;
        paint(g_std, &c[j].st, fg, b);
        ncplane_putstr_yx(g_std, y, x + col, c[j].egc);
        col += c[j].w;
    }
    free(c);
    return col;
}
static void fill_row(int y, int x0, int x1, unsigned bg, bool grad) {
    for (int x = x0; x < x1; x++) {
        unsigned b = grad ? mix(C_BAR_A, C_BAR_B, (int)((long)x * 256 / (g_cols > 0 ? g_cols : 1))) : bg;
        nc_style st = { NC_DEFAULT, NC_DEFAULT, 0 };
        paint(g_std, &st, NC_DEFAULT, b);
        ncplane_putstr_yx(g_std, y, x, " ");
    }
}

/* the conversation pane: the scrollback model's lines, `view` rows high; top-aligned while they fit, the newest at the bottom once they fill it */
static void draw_pane(int view) {
    g_pane_epoch = -1;
    if (view <= 0) return;
    if (view > g_pane_cap) { g_pane_line = xrealloc(g_pane_line, sizeof *g_pane_line * (size_t)view); g_pane_cap = view; }
    for (int y = 0; y < view; y++) g_pane_line[y] = -1;
    int total = term_sb_nlines();
    int start = g_clear_at < total ? g_clear_at : total;
    typedef struct { nc_cell *c; int n; int rows; int idx; } lline;
    lline *ls = NULL;
    int nls = 0, cap = 0, acc = 0;
    int need = view + g_top;
    /* the lines are read from the newest back, until they fill the pane (and the scroll-back) */
    for (int i = total - 1; i >= start && acc < need; i--) {
        if (term_sb_hidden(i)) continue;
        const char *s = term_sb_line(i);
        size_t len = strlen(s);
        int n = nc_cells(s, len, NULL, 0x7fffffff, NULL);
        nc_cell *c = xmalloc(sizeof *c * (size_t)(n ? n : 1));
        nc_style st = { NC_DEFAULT, NC_DEFAULT, 0 };
        n = nc_cells(s, len, c, n, &st);
        int rows = nc_line_rows(c, n, g_cols);
        if (nls == cap) { cap = cap ? cap * 2 : 64; ls = xrealloc(ls, sizeof *ls * (size_t)cap); }
        ls[nls].c = c; ls[nls].n = n; ls[nls].rows = rows; ls[nls].idx = i; nls++;
        acc += rows;
    }
    int T = acc;
    if (g_top > T - view) g_top = T > view ? T - view : 0;   /* the scroll-back stops at the oldest line */
    int top_abs = T - view - g_top;                          /* the content row at the pane's top */
    if (top_abs < 0) top_abs = 0;                            /* short: draw from the top, under the banner; full: the newest lines at the bottom */
    int abs = 0;
    for (int k = nls - 1; k >= 0; k--) {                     /* oldest first */
        lline *L = &ls[k];
        int row = 0, col = 0;
        for (int j = 0; j < L->n; j++) {
            int w = L->c[j].w;
            if (col + w > g_cols && col > 0) { row++; col = 0; }
            int y = abs + row - top_abs;
            if (y >= 0 && y < view && col + w <= g_cols) {
                paint(g_std, &L->c[j].st, C_TEXT, NC_DEFAULT);
                ncplane_putstr_yx(g_std, y, col, L->c[j].egc);
            }
            col += w;
        }
        for (int r = 0; r < L->rows; r++) { int y = abs + r - top_abs; if (y >= 0 && y < view) g_pane_line[y] = L->idx; }
        abs += L->rows;
        free(L->c);
    }
    free(ls);
    g_pane_epoch = term_sb_epoch();
}

/* the editor's text as rows: how many, and where the cursor is */
/* the byte where each row of the field text starts (word wrap: term_wrap, as on the ANSI screen); *nrows rows,
 * the last entry is g_len + 1 */
static size_t *field_starts(int width, int *nrows) {
    int r = 0, c = 0;
    term_wrap(g_ed, g_len, width, 0, g_len, -1, NULL, &r, &c);
    int R = r + 1;
    size_t *st = xmalloc(sizeof *st * (size_t)(R + 1));
    st[0] = 0;
    for (int k = 1; k < R; k++) { size_t b = g_len; int rr, cc; term_wrap(g_ed, g_len, width, 0, g_len, k, &b, &rr, &cc); st[k] = b; }
    st[R] = g_len + 1;
    *nrows = R;
    return st;
}
static void field_layout(int width, int *rows, int *crow, int *ccol) {
    int R = 1, cr = 0, cc = 0;
    size_t *st = field_starts(width, &R);
    free(st);
    term_wrap(g_ed, g_len, width, 0, g_cur, -1, NULL, &cr, &cc);
    *rows = R; *crow = cr; *ccol = cc;
}

/* the field is a box: "│ ❯ " before the text and " │" after it, so its text is g_cols - 6 wide, from column 4 */
static int field_text_width(void) { return g_cols - 6 > 1 ? g_cols - 6 : 1; }

/* the field's height: its text (at most FIELD_MAX rows), and the two rules */
static int field_height(void) {
    int rows, cr, cc;
    field_layout(field_text_width(), &rows, &cr, &cc);
    return (rows > FIELD_MAX ? FIELD_MAX : rows) + 2;
}

/* where the editor's caret cell is drawn this frame (-1: none shows); the hardware cursor is parked there */
static int g_caret_y = -1, g_caret_x = -1;
static bool g_hwcur = false;           /* the hardware cursor is enabled */

static void draw_field(int top) {
    int width = field_text_width();
    g_caret_y = -1;
    int rows, cr, cc;
    field_layout(width, &rows, &cr, &cc);
    int nrows = 0; size_t *starts = field_starts(width, &nrows);
    /* the @files in the text that are there: the same spans the ANSI field colours (a / or ! line has none) */
    size_t ms[16][2];
    int nm = g_len && g_ed[0] != '/' && g_ed[0] != '!' ? mention_spans(g_ed, g_len, ms, 16) : 0;
    int shown = rows > FIELD_MAX ? FIELD_MAX : rows;
    int first = cr - shown + 1 > 0 ? cr - shown + 1 : 0;      /* keep the cursor's row in view */
    if (first > rows - shown) first = rows - shown > 0 ? rows - shown : 0;
    unsigned rc = term_is_ready() ? g_theme->edge_ready : g_theme->edge_idle;   /* the edges, as the ANSI screen draws them */
    for (int x = 1; x < g_cols - 1; x++) {                   /* the top and bottom edges, rounded at the corners */
        put_text(top, x, "─", 1, rc, NC_DEFAULT, false, NULL);
        put_text(top + shown + 1, x, "─", 1, rc, NC_DEFAULT, false, NULL);
    }
    put_text(top, 0, "╭", 1, rc, NC_DEFAULT, false, NULL);
    put_text(top, g_cols - 1, "╮", 1, rc, NC_DEFAULT, false, NULL);
    put_text(top + shown + 1, 0, "╰", 1, rc, NC_DEFAULT, false, NULL);
    put_text(top + shown + 1, g_cols - 1, "╯", 1, rc, NC_DEFAULT, false, NULL);
    for (int r = 0; r < shown; r++) {                        /* the side borders of the text rows; the rows between them are the field's ground */
        fill_row(top + 1 + r, 2, g_cols - 2, C_FIELD_BG, false);
        put_text(top + 1 + r, 0, "│", 1, rc, NC_DEFAULT, false, NULL);
        put_text(top + 1 + r, g_cols - 1, "│", 1, rc, NC_DEFAULT, false, NULL);
    }
    if (term_is_ready() && g_cols >= 10) {   /* [ready], upper right, as on the ANSI screen */
        nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 };
        put_text(top, g_cols - 8, "[ready]", 7, g_theme->edge_ready, NC_DEFAULT, false, &carry);
    }
    if (first == 0) put_text(top + 1, 2, "❯", 1, C_ACCENT, C_FIELD_BG, false, NULL);   /* the prompt, inside the border */
    {   /* the mode, on the lower rule, its last cell two columns in from the edge (as the ANSI screen puts it) */
        sbuf m; sb_init(&m);
        int mw = term_mode_text(&m, g_cols), x0 = g_cols - mw - 1;
        if (mw && x0 >= 1) { nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 }; put_text(top + shown + 1, x0, m.data, mw, NC_DEFAULT, NC_DEFAULT, false, &carry); }
        sb_free(&m);
    }
    int row = 0, col = 0;
    size_t at = 0;
    for (;;) {
        bool here = at == g_cur;
        int y = top + 1 + row - first, x = 4 + col;
        bool shows = row >= first && row < first + shown;
        if (at >= g_len) {                                   /* the end: the cursor on a blank */
            if (here && shows) { nc_style st = { C_BAR_A, C_TEXT, 0 }; paint(g_std, &st, C_BAR_A, C_TEXT); ncplane_putstr_yx(g_std, y, x, " "); }
            break;
        }
        unsigned char ch = (unsigned char)g_ed[at];
        if (ch == '\n') {
            if (here && shows) { nc_style st = { C_BAR_A, C_TEXT, 0 }; paint(g_std, &st, C_BAR_A, C_TEXT); ncplane_putstr_yx(g_std, y, x, " "); }
            at++; continue;   /* the row changes where the walker says: the next row starts after it */
        }
        while (row + 1 < nrows && at >= starts[row + 1]) { row++; col = 0; y = top + 1 + row - first; x = 4; shows = row >= first && row < first + shown; }
        size_t len = utf8_len(ch);
        if (at + len > g_len) len = g_len - at;
        nc_cell cell; int w = nc_cells(g_ed + at, len, &cell, 1, NULL) ? cell.w : 1;
        bool inmention = false;
        for (int q = 0; q < nm; q++) if (at >= ms[q][0] && at < ms[q][1]) { inmention = true; break; }
        if (shows) {
            /* the caret is a light cell with a dark letter (reverse video does not show here, and the hardware cursor is not moved: see nc_render) */
            nc_style st = here ? (nc_style){ C_BAR_A, C_TEXT, 0 } : (nc_style){ inmention ? C_MENTION : C_FIELD_TEXT, NC_DEFAULT, 0 };
            paint(g_std, &st, C_FIELD_TEXT, C_FIELD_BG);
            ncplane_putstr_yx(g_std, y, x, cell.egc);
        }
        col += w; at += len;
    }
    free(starts);
}

/* the status bar and the location line: the ANSI UI's own text, parsed into cells */
static void draw_bar(int row) {
    sbuf t; sb_init(&t);
    term_status_text(&t, g_cols);
    fill_row(row, 0, g_cols, NC_DEFAULT, false);   /* the window's own background, as the ANSI bar has */
    nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 };
    put_text(row, 0, t.data ? t.data : "", g_cols, C_TEXT, NC_DEFAULT, false, &carry);   /* the text starts with its own space: from column 0, all of the row (a column less cut off the last letter of "ready") */
    sb_free(&t);
}
/* a menu, centred over the field: a rounded frame, the title, the rows, the keys */
/* The banner's modal: drawn on the screen over the pane, centred on it, with its own background (as the menus
 * are); its close button is the top right cell of the box. */
void nc_banner_set(const char **lines, int n, int bw, bool on) {
    for (int i = 0; i < g_bn_n; i++) sb_free(&g_bn_lines[i]);
    free(g_bn_lines); g_bn_lines = NULL; g_bn_n = 0;
    if (n > 0 && lines) {
        g_bn_lines = xmalloc(sizeof *g_bn_lines * (size_t)n); g_bn_n = n; g_bn_bw = bw;
        for (int i = 0; i < n; i++) { sb_init(&g_bn_lines[i]); sb_puts(&g_bn_lines[i], lines[i]); }
    }
    g_bn_on = on && g_bn_n > 0;
    if (!g_bn_on) g_bn_xr = g_bn_xc0 = g_bn_xc1 = -1;
    nc_render_soon();
}
bool nc_banner_open(void) { return g_bn_on; }
static void nc_banner_close(void) { nc_banner_set(NULL, 0, 0, false); }
static void draw_banner(void) {
    int region = g_field_top, top = (region - g_bn_n) / 2, left = (g_cols - g_bn_bw) / 2;
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    g_bn_xr = top; g_bn_xc0 = left + g_bn_bw - BANNER_X_FROM_RIGHT - BANNER_X_W / 2; g_bn_xc1 = g_bn_xc0 + BANNER_X_W - 1;
    for (int i = 0; i < g_bn_n && top + i < region; i++) {
        fill_row(top + i, left, left + g_bn_bw, C_BAR_A, false);   /* (fill_row takes the end column) */
        nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 };
        put_text(top + i, left, g_bn_lines[i].data ? g_bn_lines[i].data : "", g_bn_bw, NC_DEFAULT, C_BAR_A, false, &carry);
    }
}

/* The overlay (/usage and the like): the text in a window, centred on the pane, with its own background. Esc
 * closes it; typing still goes to the prompt, as on the ANSI screen. */
void nc_overlay_set(const char *text) {
    for (int i = 0; i < g_ov_n; i++) sb_free(&g_ov_lines[i]);
    free(g_ov_lines); g_ov_lines = NULL; g_ov_n = 0; g_ov_w = 0;
    g_ov_on = false;
    if (text) {
        int n = 1;
        for (const char *p = text; *p; p++) if (*p == '\n') n++;
        g_ov_lines = xmalloc(sizeof *g_ov_lines * (size_t)n);
        const char *p = text;
        for (int i = 0; i < n; i++) {
            const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
            sb_init(&g_ov_lines[i]); sb_append(&g_ov_lines[i], p, len);
            int w = nc_cells(p, len, NULL, 0x7fffffff, NULL);
            if (w > g_ov_w) g_ov_w = w;
            g_ov_n++;
            if (!e) break;
            p = e + 1;
        }
        int cap = g_cols - 8; if (g_ov_w > cap) g_ov_w = cap > 1 ? cap : 1;
        g_ov_on = g_ov_n > 0;
    }
    nc_render_soon();
}
bool nc_overlay_open(void) { return g_ov_on; }
static void draw_overlay(void) {
    int H = g_ov_n + 2, W = g_ov_w + 4, region = g_field_top;
    if (W > g_cols || H > region) return;
    int top = (region - H) / 2, left = (g_cols - W) / 2;
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    for (int r = 0; r < H; r++) fill_row(top + r, left, left + W, C_BAR_A, false);
    nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 };
    char edge[600] = "";
    snprintf(edge, sizeof edge, "╭");
    for (int i = 0; i < W - 2; i++) strcat(edge, "─");
    strcat(edge, "╮");
    put_text(top, left, edge, W, C_RULE, C_BAR_A, false, &carry);
    for (int i = 0; i < g_ov_n; i++) {
        int y = top + 1 + i;
        put_text(y, left, "│ ", 2, C_RULE, C_BAR_A, false, &carry);
        nc_style c2 = { NC_DEFAULT, NC_DEFAULT, 0 };
        put_text(y, left + 2, g_ov_lines[i].data ? g_ov_lines[i].data : "", g_ov_w, NC_DEFAULT, C_BAR_A, false, &c2);
        put_text(y, left + W - 2, " │", 2, C_RULE, C_BAR_A, false, &carry);
    }
    edge[0] = 0;
    strcat(edge, "╰");
    for (int i = 0; i < W - 2; i++) strcat(edge, "─");
    strcat(edge, "╯");
    put_text(top + H - 1, left, edge, W, C_RULE, C_BAR_A, false, &carry);
}

/* the agents strip: one row per running sub-agent, the selected one on a dark band */
static void draw_strip(int first, int rows, const char *text) {
    const char *p = text ? text : "";
    int sel = term_strip_sel();
    for (int i = 0; i < rows && *p; i++) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        sbuf line; sb_init(&line); sb_append(&line, p, len);
        unsigned bg = i == sel ? g_theme->select : NC_DEFAULT;
        fill_row(first + i, 0, g_cols, bg, false);
        nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 };
        put_text(first + i, 0, line.data ? line.data : "", g_cols, NC_DEFAULT, bg, false, &carry);
        sb_free(&line);
        p = e ? e + 1 : p + len;
    }
}

static void draw_menu(void) {
    int widest = nc_text_width(g_menu_title) + 2, hint_w = nc_text_width(g_menu_hint);
    if (hint_w > widest) widest = hint_w;
    for (int i = 0; i < g_menu_n; i++) { int w = nc_text_width(g_menu[i].text) + 4; if (w > widest) widest = w; }
    int w = widest + 6;
    if (w < 40) w = 40;
    if (w > g_cols - 4) w = g_cols - 4;
    if (w < 10) w = 10;
    int h = g_menu_n + 4;
    int y = g_field_top - h; if (y < 0) y = 0;
    int x = (g_cols - w) / 2; if (x < 0) x = 0;
    for (int r = 0; r < h; r++) fill_row(y + r, x, x + w, C_MENU, false);
    put_text(y, x, "╭", 1, C_ACCENT, C_MENU, false, NULL);
    put_text(y, x + w - 1, "╮", 1, C_ACCENT, C_MENU, false, NULL);
    put_text(y + h - 1, x, "╰", 1, C_ACCENT, C_MENU, false, NULL);
    put_text(y + h - 1, x + w - 1, "╯", 1, C_ACCENT, C_MENU, false, NULL);
    for (int i = 1; i < w - 1; i++) { put_text(y, x + i, "─", 1, C_ACCENT, C_MENU, false, NULL); put_text(y + h - 1, x + i, "─", 1, C_ACCENT, C_MENU, false, NULL); }
    for (int r = 1; r < h - 1; r++) { put_text(y + r, x, "│", 1, C_ACCENT, C_MENU, false, NULL); put_text(y + r, x + w - 1, "│", 1, C_ACCENT, C_MENU, false, NULL); }
    put_text(y + 1, x + 2, g_menu_title, w - 4, C_WARN, C_MENU, false, NULL);
    for (int i = 0; i < g_menu_n; i++) {
        int ry = y + 2 + i;
        if (g_menu[i].sel) { put_text(ry, x + 2, "❯", 1, C_ACCENT, C_MENU, false, NULL); }
        put_text(ry, x + 4, g_menu[i].text, w - 6, g_menu[i].sel ? C_TEXT : g_menu[i].fg, C_MENU, false, NULL);
    }
    put_text(y + h - 2, x + 2, g_menu_hint, w - 4, C_MUTED, C_MENU, false, NULL);
}

/* ---------- suggestions above the field (term_suggest) ----------
 * The list the ANSI UI offers for the word at the cursor: worked out again when the line or the
 * cursor changes, drawn over the conversation just above the field. Its keys are sug_key(). */
#define NSUG_ROWS 8
static term_sug *g_sug_items = NULL;
static int g_sug_n = 0, g_sug_sel = -1, g_sug_top = 0;   /* sel -1: only a preview; ↓ steps into it */
static size_t g_sug_from = 0;          /* where the word they would replace starts */
static bool g_sug_off = false;         /* Esc closed the list: it comes back when the line changes */
static char *g_sug_for = NULL;         /* the line the list was worked out for … */
static size_t g_sug_for_cur = 0;       /* … and its cursor */

static void sug_clear(void) {
    for (int i = 0; i < g_sug_n; i++) { free(g_sug_items[i].text); free(g_sug_items[i].desc); }
    free(g_sug_items); g_sug_items = NULL; g_sug_n = 0; g_sug_sel = -1; g_sug_top = 0;
}
static void sug_fill(void) {
    sug_clear();
    if (!term_suggest || g_sug_off) return;
    term_sug *items = NULL; size_t from = 0;
    int n = term_suggest(g_ed ? g_ed : "", g_cur, &from, &items);
    if (n > 0 && items && from <= g_cur) { g_sug_items = items; g_sug_n = n; g_sug_from = from; return; }
    for (int i = 0; items && i < n; i++) { free(items[i].text); free(items[i].desc); }
    free(items);
}
/* the list follows the line: worked out once per change, not per key that changes nothing */
static void sug_sync(void) {
    const char *t = g_ed ? g_ed : "";
    if (g_sug_for && !strcmp(g_sug_for, t) && g_sug_for_cur == g_cur) return;
    free(g_sug_for); g_sug_for = xstrdup(t); g_sug_for_cur = g_cur;
    g_sug_off = false;
    sug_fill();
}
/* the line is handed to someone else (a question asked while typing): its list is not ours */
static void sug_forget(void) { free(g_sug_for); g_sug_for = NULL; sug_clear(); }

static void draw_suggest(void) {
    if (!g_sug_n || g_menu_on || g_field_top < 2) return;
    int show = g_sug_n < NSUG_ROWS ? g_sug_n : NSUG_ROWS;
    if (g_sug_sel >= 0 && g_sug_sel < g_sug_top) g_sug_top = g_sug_sel;
    if (g_sug_sel >= g_sug_top + show) g_sug_top = g_sug_sel - show + 1;
    if (g_sug_top > g_sug_n - show) g_sug_top = g_sug_n - show;
    if (g_sug_top < 0) g_sug_top = 0;
    int h = show + 1;                                  /* the rows, and the line of keys under them */
    int y = g_field_top - h; if (y < 0) y = 0;
    for (int r = 0; r < h; r++) if (y + r < g_pane_cap) g_pane_line[y + r] = -1;   /* a click on the list is no click on the conversation */
    for (int r = 0; r < h; r++) fill_row(y + r, 0, g_cols, C_MENU, false);
    int namew = 0;
    for (int i = g_sug_top; i < g_sug_top + show; i++) {
        int l = (int)strlen(g_sug_items[i].text);
        if (l > namew) namew = l;
    }
    if (namew > g_cols / 2) namew = g_cols / 2;
    for (int i = 0; i < show; i++) {
        int idx = g_sug_top + i, ry = y + i;
        bool sel = idx == g_sug_sel;
        if (sel) put_text(ry, 1, "❯", 1, C_ACCENT, C_MENU, false, NULL);
        put_text(ry, 3, g_sug_items[idx].text, namew, sel ? C_TEXT : C_WARN, C_MENU, false, NULL);
        if (g_sug_items[idx].desc && *g_sug_items[idx].desc && g_cols - 5 - namew > 4)
            put_text(ry, 5 + namew, g_sug_items[idx].desc, g_cols - 6 - namew, C_MUTED, C_MENU, false, NULL);
    }
    put_text(y + h - 1, 3, g_sug_sel >= 0 ? "enter take it · ↑/↓ move · esc close" : "tab complete · ↓ choose · esc close",
             g_cols - 4, C_MUTED, C_MENU, false, NULL);
}

void nc_render(void) {
    if (!g_nc || !g_std) return;
    g_pending = false; g_last_draw = now_s();
    unsigned r = 0, c = 0;
    ncplane_dim_yx(g_std, &r, &c);
    g_rows = (int)r; g_cols = (int)c;
    g_caret_y = -1;
    ncplane_erase(g_std);
    if (g_rows >= 6 && g_cols >= 10) {
        int fh = field_height();
        int bar = g_rows - 1;   /* the bar is the last row: the folder and branch are in it */
        const char *strip_text = NULL;
        int srows = term_strip_update(&strip_text);   /* the agents strip: between the field and the bar */
        int top = bar - fh - srows; if (top < 1) top = 1;
        g_field_top = top;
        draw_pane(top);
        draw_field(top);
        draw_suggest();
        draw_strip(bar - srows, srows, strip_text);
        draw_bar(bar);
        if (g_ov_on) draw_overlay();
        if (g_bn_on) draw_banner();
        if (g_menu_on) draw_menu();
    }
    /* the hardware cursor sits on the caret, so it blinks where the input is — also while the model
     * works (the field keeps the text typed then). A menu has its own keys: no cursor there. */
    if (g_caret_y >= 0 && !g_menu_on && g_caret_y < g_rows && g_caret_x < g_cols) {
        notcurses_cursor_enable(g_nc, g_caret_y, g_caret_x); g_hwcur = true;
    } else if (g_hwcur) { notcurses_cursor_disable(g_nc); g_hwcur = false; }
    notcurses_render(g_nc);
}

/* output that arrives often is drawn at most once per REDRAW_MS; a pending one goes out with the next wait */
void nc_render_soon(void) {
    if (!g_nc) return;
    if (now_s() - g_last_draw >= REDRAW_MS / 1000.0) nc_render();
    else g_pending = true;
}
static void nc_flush_due(void) {
    if (g_pending && now_s() - g_last_draw >= REDRAW_MS / 1000.0) nc_render();
}
void nc_output(void) { nc_render_soon(); }

/* ---------- the screen's life ---------- */
bool nc_start(void) {
    if (g_nc) return true;
    int fd = dup(STDOUT_FILENO);
    if (fd < 0) { fprintf(stderr, "corbienest: cannot dup the terminal for notcurses; using the ANSI UI\n"); return false; }
    g_nc_out = fdopen(fd, "w");
    if (!g_nc_out) { close(fd); fprintf(stderr, "corbienest: cannot open the terminal for notcurses; using the ANSI UI\n"); return false; }
    notcurses_options o; memset(&o, 0, sizeof o);
    o.flags = NCOPTION_NO_QUIT_SIGHANDLERS | NCOPTION_SUPPRESS_BANNERS;   /* atexit() restores the terminal */
    o.loglevel = NCLOGLEVEL_SILENT;
    g_nc = notcurses_init(&o, g_nc_out);
    if (!g_nc) {
        fclose(g_nc_out); g_nc_out = NULL;
        fprintf(stderr, "corbienest: notcurses could not start on this terminal; using the ANSI UI\n");
        return false;
    }
    g_std = notcurses_stdplane(g_nc);
    (void)notcurses_mice_enable(g_nc, NCMICE_BUTTON_EVENT);   /* clicks (folds) and the wheel (the pane); without it the terminal keeps its own */
    g_top = 0; g_clear_at = 0; g_pending = false; g_kq_n = 0;
    nc_render();
    return true;
}

void nc_stop(void) {
    if (!g_nc) return;
    notcurses_stop(g_nc);
    g_nc = NULL; g_std = NULL; g_hwcur = false;
    if (g_nc_out) { fclose(g_nc_out); g_nc_out = NULL; }
    free(g_draft); g_draft = NULL;
}

/* notcurses reads stdin on its own thread, so a busy wait watches the pipe it fills, not stdin */
int nc_input_fd(void) { return g_nc ? notcurses_inputready_fd(g_nc) : -1; }

bool nc_size(int *rows, int *cols) {
    if (!g_nc || !g_std) return false;
    unsigned r = 0, c = 0;
    ncplane_dim_yx(g_std, &r, &c);
    if (rows) *rows = (int)r;
    if (cols) *cols = (int)c;
    return true;
}

void nc_clear(void) {
    g_clear_at = term_sb_nlines();
    g_top = 0;
    nc_render();
}

void nc_prefill(const char *text) {
    free(g_pre);
    g_pre = text ? xstrdup(text) : NULL;
}

/* ---------- keys while a line is typed, or while the model works ---------- */
static int pane_rows(void) { return g_field_top > 1 ? g_field_top : 1; }

/* the keys that edit the line; the scroll keys move the pane (the wheel's are PgUp/PgDn here) */
static void edit_key(int k) {
    char b[4];
    switch (k) {
        case NK_LEFT:  g_cur = ed_prev(g_cur); break;
        case NK_RIGHT: g_cur = ed_next(g_cur); break;
        case NK_HOME:
        case 1:        g_cur = g_len ? ed_line_start(g_cur) : 0; break;   /* Home, Ctrl-A */
        case NK_END:
        case 5:        g_cur = ed_line_end(g_cur); break;                  /* End, Ctrl-E */
        case 127:
        case 8:        if (g_cur > 0) ed_delete(ed_prev(g_cur), g_cur); break;
        case NK_DEL:   if (g_cur < g_len) ed_delete(g_cur, ed_next(g_cur)); break;
        case 11: { size_t e = ed_line_end(g_cur); if (e > g_cur) ed_delete(g_cur, e); break; }   /* Ctrl-K */
        case 21: { size_t s = ed_line_start(g_cur); if (s < g_cur) ed_delete(s, g_cur); break; } /* Ctrl-U */
        case 10:       ed_insert("\n", 1); break;                            /* Ctrl-J: a newline in the line */
        case NK_PASTE: { char *t = paste_take(); if (t) { ed_insert(t, strlen(t)); free(t); } break; }   /* a bracketed paste: its text, newlines kept */
        case NK_PGUP:  g_top += pane_rows() / 2; nc_render(); break;
        case NK_PGDN:  g_top -= pane_rows() / 2; if (g_top < 0) g_top = 0; nc_render(); break;
        default:
            if (k >= 32 && k < 0x110000) { int n = utf8_put((unsigned)k, b); ed_insert(b, (size_t)n); }
            break;
    }
}

/* Put suggestion `i` in place of the word at the cursor (the rest of the word goes too). */
static void sug_accept(int i) {
    if (i < 0 || i >= g_sug_n) return;
    size_t end = g_cur;
    while (end < g_len && g_ed[end] != ' ' && g_ed[end] != '\n' && g_ed[end] != '\t') end++;
    ed_delete(g_sug_from, end);                       /* leaves the cursor at g_sug_from */
    ed_insert(g_sug_items[i].text, strlen(g_sug_items[i].text));
    if (g_sug_items[i].open) return;                  /* a directory: what is in it comes next */
    if (g_cur < g_len && g_ed[g_cur] == ' ') g_cur++; else ed_insert(" ", 1);
}

/* Tab: complete what all the candidates agree on, or, when there is nothing more, start choosing */
static void sug_tab(void) {
    if (g_sug_sel >= 0 || g_sug_n == 1) { sug_accept(g_sug_sel >= 0 ? g_sug_sel : 0); return; }
    size_t tl = g_cur - g_sug_from;
    const char *tok = g_ed + g_sug_from;
    size_t cp = strlen(g_sug_items[0].text);
    for (int i = 0; i < g_sug_n && cp > tl; i++) {
        if (strncmp(g_sug_items[i].text, tok, tl)) { cp = 0; break; }
        size_t k = 0; while (k < cp && g_sug_items[i].text[k] == g_sug_items[0].text[k]) k++;
        cp = k;
    }
    if (cp <= tl) { g_sug_sel = 0; return; }
    char *pre = xstrndup(g_sug_items[0].text, cp);
    ed_delete(g_sug_from, g_cur);
    ed_insert(pre, cp);
    free(pre);
}

/* The list's keys (the same ones as the ANSI UI): true when it took the key. Enter with nothing
 * highlighted is not taken, so it still sends the line; Esc closes the list before anything else. */
static bool sug_key(int k) {
    if (k == 9) {                                     /* Tab */
        if (!g_sug_n) { g_sug_off = false; sug_fill(); }   /* closed: Tab asks again */
        else sug_tab();
        return true;
    }
    if (!g_sug_n) return false;
    if ((k == NK_UP || k == NK_DOWN) && g_hist_pos < hist_count()) return false;   /* browsing the history: ↑/↓ scroll it */
    if (k == NK_DOWN) { if (g_sug_sel < g_sug_n - 1) g_sug_sel++; return true; }
    if (k == NK_UP && g_sug_sel >= 0) { g_sug_sel--; return true; }
    if (k == 13 && g_sug_sel >= 0) { sug_accept(g_sug_sel); return true; }
    if (k == 27) { g_sug_off = true; sug_clear(); return true; }
    return false;
}

/* ↑/↓ on the line: the history, as at the prompt; while busy the queued messages come back into the field first
 * (queue true), as the ANSI screen does. The line typed before a browse is kept in g_draft for ↓. */
static void nc_hist(int k, bool queue) {
    if (k == NK_UP && queue && term_queue_count()) {
        sbuf b; sb_init(&b);
        while (term_queue_count()) {
            char *m = term_queue_pop();
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, m, strlen(m));
            free(m);
        }
        if (g_len) { sb_putc(&b, '\n'); sb_append(&b, g_ed, g_len); }
        ed_set(b.data ? b.data : "");
        sb_free(&b);
        return;
    }
    if (k == NK_UP) {
        if (g_hist_pos > 0) {
            if (g_hist_pos == hist_count()) { free(g_draft); g_draft = xstrdup(g_ed ? g_ed : ""); }
            g_hist_pos--;
            ed_set(hist_get(g_hist_pos));
        }
    } else if (g_hist_pos < hist_count()) {
        g_hist_pos++;
        if (g_hist_pos == hist_count()) ed_set(g_draft ? g_draft : "");
        else ed_set(hist_get(g_hist_pos));
    }
}

int nc_poll_interrupt(void) {
    fflush(stdout);   /* output a command printed here (stdout is fully buffered) reaches the pane now */
    for (;;) {
        int k = nc_getkey(0);
        if (k == NK_NONE) break;
        if (k == NK_SKIP) break;                         /* drained, nothing usable: never spin on the readiness fd */
        sug_sync();                                      /* the list is for the line as it is before this key */
        if (term_strip_key(strip_code(k), g_len == 0 && !g_sug_n)) continue;   /* the agents strip: ↓ from an empty line, ↑/↓, Enter opens the log, Esc returns */
        if (sug_key(k)) continue;                        /* the list's keys: Tab, ↓/↑, Enter on a row, Esc closes it */
        if (k == NK_UP || k == NK_DOWN) { nc_hist(k, true); continue; }   /* the queue first, then the history */
        if (k == 3 || k == 27) return 1;                 /* Ctrl-C and Esc stop the work, as in the ANSI UI */
        if (k == 13) {                                   /* Enter while busy: the line is queued for the model */
            if (g_len) {
                /* a slash command main.c can run right there (/status, /mode, …) runs now, as in the ANSI UI */
                char *text = xstrdup(g_ed);
                if (!(text[0] == '/' && term_run_while_busy && term_run_while_busy(text))) term_queue_push(text);
                free(text);
            }
            ed_set("");
            continue;
        }
        edit_key(k);
    }
    sug_sync();
    nc_render_soon();
    return 0;
}

/* the prompt's loop: returns the line (possibly ""), or NULL at the end (Ctrl-D, or Ctrl-C twice on an empty line) */
static char *readline_loop(void) {
    double idle_at = (term_idle_ms > 0 && term_idle_hook && g_len == 0) ? now_s() + term_idle_ms / 1000.0 : -1;   /* idle means an empty field */
    int ctrl_c = 0;
    for (;;) {
        sug_sync();       /* the list follows the line, whatever changed it (a key, a recalled line) */
        fflush(stdout);   /* what the last line printed (a slash command's report) is in the pane before the next key */
        nc_render();
        int wait = 33;
        int tw = term_tick_wait();   /* the banner's tick (term.c): the wait ends when it is due */
        if (tw >= 0 && tw < wait) wait = tw;
        if (idle_at > 0) {   /* the idle work runs once the prompt has been untouched for term_idle_ms */
            double left = idle_at - now_s();
            if (left <= 0) {
                idle_at = -1;
                term_idle_hook();
                char *q = term_queue_pop();
                if (q) return q;                       /* a message queued meanwhile is submitted now */
                continue;
            }
            if (left * 1000 < wait) wait = (int)(left * 1000) + 1;
        }
        int k = nc_getkey(wait);
        term_tick_run();
        if (k == NK_NONE || k == NK_SKIP) continue;
        idle_at = -1;                                  /* typing cancels the idle work for this prompt */
        if (term_strip_key(strip_code(k), g_len == 0 && !g_sug_n)) continue;   /* the agents strip: ↓ from an empty line, ↑/↓, Enter opens the log, Esc returns */
        if (sug_key(k)) continue;                      /* the list's keys come first (see sug_key) */
        if (k == 13) {
            char *line = xstrdup(g_ed ? g_ed : "");
            ed_set("");
            g_hist_pos = hist_count();
            return line;
        }
        if (k == 4 && g_len == 0) return NULL;         /* Ctrl-D on an empty line */
        if (k == 3) {
            if (g_len == 0) { if (++ctrl_c >= 2) return NULL; }
            else { ctrl_c = 0; ed_set(""); g_hist_pos = hist_count(); }
            continue;
        }
        ctrl_c = 0;
        if (k == 27) { ed_set(""); g_hist_pos = hist_count(); continue; }
        if (k == NK_UP || k == NK_DOWN) { nc_hist(k, false); continue; }
        edit_key(k);
    }
}

char *nc_readline(const char *prompt, bool in_field) {
    (void)prompt;
    if (!g_nc) return NULL;
    if (in_field && !g_pre) {   /* the text typed while the model worked is still in the field: the prompt goes on from it */
        sug_forget();           /* (the list is worked out again for it by readline_loop) */
        g_hist_pos = hist_count();
        return readline_loop();
    }
    /* a question asked while a line is typed (the type-ahead) keeps that line for after it */
    char *s_ed = g_ed, *s_draft = g_draft; size_t s_len = g_len, s_cap = g_cap, s_cur = g_cur; int s_pos = g_hist_pos;
    g_ed = NULL; g_len = g_cap = g_cur = 0; g_draft = NULL; g_hist_pos = hist_count();
    sug_forget();   /* the list of the line set aside is not this one's */
    if (in_field && g_pre) { ed_set(g_pre); free(g_pre); g_pre = NULL; }
    char *out = readline_loop();
    sug_forget();
    free(g_ed); free(g_draft);
    g_ed = s_ed; g_draft = s_draft; g_len = s_len; g_cap = s_cap; g_cur = s_cur; g_hist_pos = s_pos;
    return out;
}

/* ---------- menus ---------- */
static void menu_open(const char *title, const char *hint) {
    snprintf(g_menu_title, sizeof g_menu_title, "%s", title ? title : "");
    snprintf(g_menu_hint, sizeof g_menu_hint, "%s", hint ? hint : "");
    g_menu_n = 0; g_menu_on = true;
}
static void menu_add(const char *text, unsigned fg, bool sel) {
    if (g_menu_n >= MENU_ROWS_MAX) return;
    snprintf(g_menu[g_menu_n].text, sizeof g_menu[g_menu_n].text, "%s", text);
    g_menu[g_menu_n].fg = fg; g_menu[g_menu_n].sel = sel; g_menu_n++;
}
static void menu_close(void) { g_menu_on = false; fflush(stdout); nc_render(); }

int nc_confirm(const char *question, const char *always_label, const char *project_label, char **reason) {
    if (reason) *reason = NULL;
    const char *labels[4] = { "Yes", always_label ? always_label : "Yes, and don't ask again this session",
                              project_label ? project_label : "No, and tell the model what to do instead",
                              "No, and tell the model what to do instead" };
    const char *keys[4] = { "y", "a", project_label ? "p" : "n", "n" };
    int nopt = project_label ? 4 : 3, no_i = nopt - 1;
    int sel = 0, choice = -1;
    if (!g_nc) { if (reason) *reason = NULL; return 0; }
    while (choice < 0) {
        menu_open(question, "↑/↓ move · Enter choose · y/a/p/n · Esc: no");
        for (int i = 0; i < nopt; i++) {
            char line[256];
            snprintf(line, sizeof line, "%d.  %s   (%s)", i + 1, labels[i], keys[i]);
            menu_add(line, i == no_i ? C_BAD : C_TEXT, i == sel);
        }
        nc_render();
        int k = nc_getkey(33);
        if (k == NK_NONE || k == NK_SKIP) continue;
        if (k == NK_UP || k == 'k') { sel = sel > 0 ? sel - 1 : nopt - 1; continue; }
        if (k == NK_DOWN || k == 'j' || k == 9) { sel = (sel + 1) % nopt; continue; }
        if (k == 13) choice = sel;
        else if (k == '1' || k == 'y' || k == 'Y') choice = 0;
        else if (k == '2' || k == 'a' || k == 'A') choice = 1;
        else if (nopt == 4 && (k == '3' || k == 'p' || k == 'P')) choice = 2;
        else if (k == '0' + nopt || k == 'n' || k == 'N') choice = no_i;
        else if (k == 27 || k == 3 || k == 4) choice = no_i;   /* Esc, Ctrl-C, Ctrl-D: no (never yes) */
    }
    menu_close();
    int rc = choice == 0 ? 1 : choice == 1 ? 2 : (nopt == 4 && choice == 2) ? 3 : 0;
    if (!term_confirm_quiet) printf("  ⎿  " "%s" C_RESET "\n",
           choice == 0 ? "\x1b[32myes" : choice == 1 ? "\x1b[32myes, always this session"
           : (nopt == 4 && choice == 2) ? "\x1b[32myes, always in this project" : "\x1b[31mno");
    if (rc == 0 && reason && choice == no_i) {
        char *r = nc_readline("tell the model what to do instead", false);
        if (r && *r) *reason = r; else free(r);
    }
    return rc;
}

int nc_select(const char *title, const char **items, const char **descs, int n, int current) {
    if (n <= 0 || !g_nc) return -1;
    char filter[128] = {0};
    int flen = 0, sel = current >= 0 && current < n ? current : 0, result = -1;
    for (;;) {
        /* the items that match the filter (case-insensitive substring), as menu rows */
        int vis[256], nvis = 0;
        for (int i = 0; i < n && nvis < 256; i++) {
            if (flen == 0) { vis[nvis++] = i; continue; }
            char a[256], b[128]; size_t j = 0;
            for (size_t k = 0; items[i][k] && j < sizeof a - 1; k++) a[j++] = (char)tolower((unsigned char)items[i][k]);
            a[j] = 0; j = 0;
            for (size_t k = 0; filter[k] && j < sizeof b - 1; k++) b[j++] = (char)tolower((unsigned char)filter[k]);
            b[j] = 0;
            if (strstr(a, b)) vis[nvis++] = i;
        }
        if (sel >= nvis) sel = nvis > 0 ? nvis - 1 : 0;
        char head[256];
        snprintf(head, sizeof head, "%s%s%s", title ? title : "", flen ? "  · filter: " : "", filter);
        menu_open(head, "↑/↓ move · Enter choose · type to filter · Esc cancel");
        int first = sel - 8 > 0 ? sel - 8 : 0;
        for (int v = first; v < nvis && v < first + 12; v++) {
            char line[384];
            int i = vis[v];
            if (descs && descs[i] && *descs[i]) snprintf(line, sizeof line, "%s  \x1b[2m%s\x1b[0m", items[i], descs[i]);
            else snprintf(line, sizeof line, "%s", items[i]);
            menu_add(line, C_TEXT, v == sel);
        }
        if (nvis == 0) menu_add("no match", C_MUTED, false);
        nc_render();
        int k = nc_getkey(33);
        if (k == NK_NONE || k == NK_SKIP) continue;
        if (k == 27 || k == 3) { result = -1; break; }
        if (k == 13) { result = nvis ? vis[sel] : -1; break; }
        if (k == NK_UP) { if (sel > 0) sel--; continue; }
        if (k == NK_DOWN) { if (sel + 1 < nvis) sel++; continue; }
        if (k == 127 || k == 8) { if (flen > 0) { do flen--; while (flen > 0 && ((unsigned char)filter[flen] & 0xC0) == 0x80); filter[flen] = 0; sel = 0; } continue; }
        if (k >= 32 && k < 0x110000 && flen < (int)sizeof filter - 5) { flen += utf8_put((unsigned)k, filter + flen); filter[flen] = 0; sel = 0; }
    }
    menu_close();
    return result;
}
#endif
