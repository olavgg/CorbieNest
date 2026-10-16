/* The colour themes: one palette, read by both screens.
 *
 * The conversation and the chrome are written with the basic SGR codes (C_RED, C_GREEN, …) as tokens. The
 * ANSI screen maps them to the theme's RGB on the way to the terminal (theme_map_bytes, in sb_write_fn), and
 * the notcurses screen maps them when it parses the same bytes (nc_sgr_parse). So a code means the same colour
 * on both, whatever the terminal's own palette is, and a repaint recolours the scrollback too. */
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* the default theme: the colours the notcurses screen has always drawn, and the ANSI screen's own RGB chrome */
static const theme_t THEMES[] = {
    {
        .name = "default",
        .text = 0xE5E9F0u, .muted = 0x7C8799u, .rule = 0x3B4252u, .menu = 0x1E2330u,
        .bar_a = 0x161A24u, .bar_b = 0x2A1F3Du,
        .field_text = 0xFFF1B8u, .field_bg = 0x151922u,
        .edge_ready = 0x5FD75Fu, .edge_idle = 0x3B4252u,
        .loc = 0xAF5F87u, .select = 0x2A2F3Au,
        .brand = 0xE6FBFFu, .spark_hi = 0x8FE3FFu, .spark_mid = 0x4FB8E0u, .spark_lo = 0x3A8FB8u,
        .crow_dark = 0x4A6A8Au, .crow_body = 0x6B7890u, .crow_light = 0xA9B6CCu, .crow_beak = 0x7A6650u,
        .accent = 0xF0883Eu,
        .ok = 0xA3BE8Cu, .bad = 0xBF616Au, .warn = 0xEBCB8Bu,
        .info = 0x56B6C2u, .blue = 0x81A1C1u, .magenta = 0xB48EADu, .gray = 0x7C8799u,
    },
};
#define THEME_COUNT ((int)(sizeof THEMES / sizeof THEMES[0]))

const theme_t *g_theme = &THEMES[0];

/* "theme=NAME": the theme of that name; false (and the theme stays as it is) when there is none */
bool theme_set(const char *name) {
    for (int i = 0; i < THEME_COUNT; i++)
        if (name && !strcmp(THEMES[i].name, name)) { g_theme = &THEMES[i]; return true; }
    return false;
}

/* the basic fg codes a theme gives a colour to: 30-37 and their bright forms 90-97 */
bool theme_fg_code(int code, unsigned *rgb) {
    const theme_t *t = g_theme;
    int base = code >= 90 && code <= 97 ? code - 60 : code;   /* bright 9x draws as its 3x */
    if (base < 30 || base > 37) return false;
    switch (base) {
    case 30: *rgb = t->bar_a; break;     /* black: the dark of the bar */
    case 31: *rgb = t->bad; break;
    case 32: *rgb = t->ok; break;
    case 33: *rgb = t->warn; break;
    case 34: *rgb = t->blue; break;
    case 35: *rgb = t->magenta; break;
    case 36: *rgb = t->info; break;
    default: *rgb = t->text; break;      /* 37: white */
    }
    if (code >= 90) *rgb = code == 90 ? t->gray : *rgb;   /* 90 is grey; the bright ones keep the theme's hue */
    return true;
}

/* A string with truecolour SGR (38;2;R;G;B) in it, copied with each colour that equals from[i] replaced by to[i].
 * The banner's crow is written with its colours as literals; the theme says what they are. */
void theme_recolor(const char *s, sbuf *out, const unsigned *from, const unsigned *to, int count) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; ) {
        if (!strncmp(s + i, "38;2;", 5)) {
            int v[3], k = 0; size_t j = i + 5;
            while (k < 3 && j < n && s[j] >= '0' && s[j] <= '9') {
                int x = 0;
                while (j < n && s[j] >= '0' && s[j] <= '9') x = x * 10 + (s[j++] - '0');
                v[k++] = x;
                if (k < 3 && j < n && s[j] == ';') j++;
            }
            if (k == 3) {
                unsigned rgb = ((unsigned)v[0] << 16) | ((unsigned)v[1] << 8) | (unsigned)v[2];
                int hit = -1;
                for (int c = 0; c < count && hit < 0; c++) if (from[c] == rgb) hit = c;
                if (hit >= 0) {
                    sb_printf(out, "38;2;%u;%u;%u", (to[hit] >> 16) & 255, (to[hit] >> 8) & 255, to[hit] & 255);
                    i = j; continue;
                }
            }
        }
        sb_putc(out, s[i]); i++;
    }
}

static void put_rgb_params(sbuf *o, unsigned rgb) {
    sb_printf(o, "38;2;%u;%u;%u", (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

/* one finished CSI sequence: an SGR (…m) is rewritten where it names a themed colour; anything else as it came */
static void map_csi(sbuf *out, const char *q, size_t n) {
    if (q[n - 1] != 'm' || memchr(q, ':', n)) { sb_append(out, q, n); return; }   /* sub-parameters: not ours */
    int p[32], np = 0; int cur = 0; bool have = false;
    for (size_t i = 1; i + 1 < n; i++) {
        if (q[i] >= '0' && q[i] <= '9') { cur = cur * 10 + (q[i] - '0'); have = true; if (cur > 9999) cur = 9999; }
        else if (q[i] == ';') { if (np < 32) p[np++] = have ? cur : 0; cur = 0; have = false; }
    }
    if (np < 32) p[np++] = have ? cur : 0;
    sbuf o; sb_init(&o);
    sb_puts(&o, "\x1b[");
    bool changed = false;
    for (int k = 0; k < np; k++) {
        unsigned rgb = 0;
        if (k) sb_putc(&o, ';');
        if (p[k] == 38 && k + 2 < np && p[k + 1] == 5 && p[k + 2] == 208) {   /* orange: the theme's accent */
            put_rgb_params(&o, g_theme->accent); k += 2; changed = true;
        } else if ((p[k] == 38 || p[k] == 48) && k + 1 < np && (p[k + 1] == 2 || p[k + 1] == 5)) {
            /* an extended colour: its parameters are numbers of the colour, not codes (38;2;95;215;95 is no magenta) */
            int extra = p[k + 1] == 2 ? 4 : 2;
            sb_printf(&o, "%d", p[k]);
            for (int j = 1; j <= extra && k + j < np; j++) sb_printf(&o, ";%d", p[k + j]);
            k += extra < np - k ? extra : np - k - 1;
        } else if (theme_fg_code(p[k], &rgb)) {
            put_rgb_params(&o, rgb); changed = true;
        } else sb_printf(&o, "%d", p[k]);
    }
    sb_putc(&o, 'm');
    if (changed) sb_append(out, o.data, o.len); else sb_append(out, q, n);
    sb_free(&o);
}

bool theme_map_pending(const theme_map *m) { return m->np > 0; }

/* The bytes of one write, mapped. A sequence the write ends inside is kept in m and finished by the next write. */
void theme_map_bytes(theme_map *m, const char *in, size_t n, sbuf *out) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (m->np) {
            if (m->np == 1) {                                  /* ESC: a CSI, or a two-byte escape */
                if (c == '[') { m->pend[m->np++] = (char)c; continue; }
                sb_append(out, m->pend, 1); m->np = 0;          /* not a CSI: the ESC goes as it came */
            } else if (c >= 0x20 && c <= 0x3f && m->np < (int)sizeof m->pend - 1) {   /* parameters, intermediates */
                m->pend[m->np++] = (char)c; continue;
            } else if (c >= 0x40 && c <= 0x7e && m->np < (int)sizeof m->pend) {       /* the final byte */
                m->pend[m->np++] = (char)c;
                map_csi(out, m->pend, (size_t)m->np);
                m->np = 0; continue;
            } else {                                            /* a malformed sequence: all of it as it came */
                sb_append(out, m->pend, (size_t)m->np); m->np = 0;
            }
        }
        if (c == 0x1b) { m->pend[0] = 0x1b; m->np = 1; continue; }
        sb_putc(out, (char)c);
    }
}

/* the escape for a colour: a ring of buffers, so one sb_printf can take two of them */
static const char *th_sgr(unsigned rgb, int bg) {
    static char ring[8][32]; static int k;
    char *b = ring[k++ & 7];
    snprintf(b, sizeof ring[0], "\x1b[%d;2;%u;%u;%um", bg ? 48 : 38, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    return b;
}
const char *th_fg(unsigned rgb) { return th_sgr(rgb, 0); }
const char *th_bg(unsigned rgb) { return th_sgr(rgb, 1); }
