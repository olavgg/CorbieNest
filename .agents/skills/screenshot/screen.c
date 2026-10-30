/* screen: draw captured terminal output into a cell grid and write the last frame as a .ansi transcript.
 *
 *   screen CAPTURE COLS ROWS OUT.ansi
 *
 * The transcript is what tools/ansi2svg.py turns into the README picture: one line per row, with the
 * colours and bold/dim as SGR, the truecolour forms (38;2, 48;2) that ansi2svg reads. The grid is a small
 * model of xterm, enough for a notcurses frame: cursor moves (CUP, CHA, VPA, CUU/CUD/CUF/CUB), erase (EL,
 * ED, ECH), SGR (16, 256 and truecolour), save/restore cursor. DEC private modes, OSC and charset
 * selections are read and ignored. Wide characters (CJK, emoji) take two cells. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGBSET 0x1000000u   /* a colour is set; the low 24 bits are its RGB */

typedef struct { uint32_t cp, fg, bg; unsigned char bold, dim; } cell;

static int COLS, ROWS;
static cell *grid;
static int row, col, save_row, save_col;
static uint32_t fg, bg;
static unsigned char bold, dim;

/* the xterm base colours, in the shades ansi2svg.py uses (30-37, 90-97) */
static const uint32_t BASE[16] = {
    0x484f58, 0xff7b72, 0x3fb950, 0xd29922, 0x58a6ff, 0xbc8cff, 0x39c5cf, 0xb1bac4,
    0x8b949e, 0xffa198, 0x56d364, 0xe3b341, 0x79c0ff, 0xd2a8ff, 0x56d4dd, 0xf0f6fc,
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t idx256(int n) {
    if (n < 16) return RGBSET | BASE[n];
    if (n < 232) {
        static const int st[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return RGBSET | ((uint32_t)st[n / 36] << 16) | ((uint32_t)st[n / 6 % 6] << 8) | (uint32_t)st[n % 6];
    }
    uint32_t v = (uint32_t)(8 + (n - 232) * 10);
    return RGBSET | (v << 16) | (v << 8) | v;
}

static int wide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
           (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
           (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) || (cp >= 0x1f900 && cp <= 0x1f9ff) ||
           (cp >= 0x20000 && cp <= 0x3fffd);
}

static void put(uint32_t cp) {
    int w = wide(cp) ? 2 : 1;
    if (col + w > COLS) { col = 0; if (row < ROWS - 1) row++; }
    if (col < COLS) {
        cell c = { cp, fg, bg, bold, dim };
        grid[row * COLS + col] = c;
        if (w == 2 && col + 1 < COLS) { cell z = { 0, fg, bg, bold, dim }; grid[row * COLS + col + 1] = z; }
    }
    col += w;
}

static void erase_at(int i) { cell c = { ' ', 0, bg, 0, 0 }; grid[i] = c; }

static void sgr(const int *p, int np) {
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0) { fg = bg = 0; bold = dim = 0; }
        else if (v == 1) bold = 1;
        else if (v == 2) dim = 1;
        else if (v == 22) bold = dim = 0;
        else if (v == 39) fg = 0;
        else if (v == 49) bg = 0;
        else if (v >= 30 && v <= 37) fg = RGBSET | BASE[v - 30];
        else if (v >= 90 && v <= 97) fg = RGBSET | BASE[v - 82];
        else if (v >= 40 && v <= 47) bg = RGBSET | BASE[v - 40];
        else if (v >= 100 && v <= 107) bg = RGBSET | BASE[v - 92];
        else if ((v == 38 || v == 48) && i + 2 < np && p[i + 1] == 5) {
            uint32_t c = idx256(p[i + 2]);
            if (v == 38) fg = c; else bg = c;
            i += 2;
        } else if ((v == 38 || v == 48) && i + 4 < np && p[i + 1] == 2) {
            uint32_t c = RGBSET | ((uint32_t)(p[i + 2] & 255) << 16) | ((uint32_t)(p[i + 3] & 255) << 8) | (uint32_t)(p[i + 4] & 255);
            if (v == 38) fg = c; else bg = c;
            i += 4;
        }
    }
}

static int param(const int *p, int np, int k, int d) { return k < np && p[k] ? p[k] : d; }

static void csi(char fin, const int *p, int np) {
    int n = param(p, np, 0, 1);
    switch (fin) {
    case 'H': case 'f':
        row = clampi(param(p, np, 0, 1) - 1, 0, ROWS - 1);
        col = clampi(param(p, np, 1, 1) - 1, 0, COLS - 1);
        break;
    case 'G': col = clampi(n - 1, 0, COLS - 1); break;
    case 'd': row = clampi(n - 1, 0, ROWS - 1); break;
    case 'A': row = row - n < 0 ? 0 : row - n; break;
    case 'B': row = row + n > ROWS - 1 ? ROWS - 1 : row + n; break;
    case 'C': col = col + n > COLS - 1 ? COLS - 1 : col + n; break;
    case 'D': col = col - n < 0 ? 0 : col - n; break;
    case 'K': {   /* 0: to the end of the line, 1: to the start, 2: the line */
        int k = np ? p[0] : 0;
        int from = k == 1 ? 0 : col, to = k == 0 ? COLS - 1 : k == 1 ? col : COLS - 1;
        for (int x = from; x <= to; x++) erase_at(row * COLS + x);
        break;
    }
    case 'J': {   /* 0: to the end of the screen, 1: to the cursor, 2: all */
        int k = np ? p[0] : 0, here = row * COLS + col, total = ROWS * COLS;
        int from = k == 1 ? 0 : here, to = k == 0 ? total - 1 : k == 1 ? here : total - 1;
        for (int i = from; i <= to; i++) erase_at(i);
        break;
    }
    case 'X':   /* ECH: n cells from the cursor */
        for (int x = col; x < col + n && x < COLS; x++) erase_at(row * COLS + x);
        break;
    case 'm': {
        int zero = 0;
        if (np == 0) { p = &zero; np = 1; }
        sgr(p, np);
        break;
    }
    default: break;   /* the rest (DEC modes, scroll regions, marks) does not change a frame drawn once */
    }
}

static size_t u8len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b >> 5) == 6) return 2;
    if ((b >> 4) == 14) return 3;
    if ((b >> 3) == 30) return 4;
    return 1;
}

static void run(const unsigned char *d, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char ch = d[i];
        if (ch == 0x1b) {
            if (i + 1 >= n) break;
            unsigned char c2 = d[i + 1];
            if (c2 == '[') {   /* CSI: parameters, intermediates, a final byte */
                size_t j = i + 2;
                int priv = j < n && (d[j] == '?' || d[j] == '>' || d[j] == '=');
                if (priv) j++;
                int p[32], np = 0, cur = 0, have = 0;
                while (j < n && ((d[j] >= '0' && d[j] <= '9') || d[j] == ';' || d[j] == ':')) {
                    if (d[j] == ';') { if (np < 32) p[np++] = have ? cur : 0; cur = 0; have = 0; }
                    else if (d[j] == ':') { cur = 0; }
                    else { cur = cur * 10 + (d[j] - '0'); have = 1; }
                    j++;
                }
                if (np < 32) p[np++] = have ? cur : 0;
                if (np == 1 && !have && j == i + 2 + priv) np = 0;   /* ESC [ m: no parameters at all */
                while (j < n && d[j] >= 0x20 && d[j] <= 0x2f) j++;   /* intermediates */
                if (j >= n) break;
                if (!priv) csi((char)d[j], p, np);
                i = j + 1;
                continue;
            }
            if (c2 == ']') {   /* OSC: up to BEL or ESC \ */
                size_t j = i + 2;
                while (j < n && d[j] != 7 && !(d[j] == 0x1b && j + 1 < n && d[j + 1] == '\\')) j++;
                i = j < n ? (d[j] == 7 ? j + 1 : j + 2) : n;
                continue;
            }
            if (c2 == '(' || c2 == ')') { i += 3; continue; }   /* charset designation */
            if (c2 == '7') { save_row = row; save_col = col; }
            else if (c2 == '8') { row = save_row; col = save_col; }
            i += 2;
            continue;
        }
        if (ch == '\r') col = 0;
        else if (ch == '\n') { if (row < ROWS - 1) row++; }
        else if (ch == '\b') { if (col > 0) col--; }
        else if (ch < 0x20 || ch == 0x7f) { /* other controls: nothing to draw */ }
        else {
            size_t len = u8len(ch);
            if (i + len > n) break;
            uint32_t cp = len == 1 ? ch : (uint32_t)(ch & (0xff >> (len + 1)));
            for (size_t k = 1; k < len; k++) cp = (cp << 6) | (d[i + k] & 0x3f);
            put(cp);
            i += len;
            continue;
        }
        i++;
    }
}

static void utf8(FILE *f, uint32_t cp) {
    if (cp < 0x80) { fputc((int)cp, f); return; }
    if (cp < 0x800) { fputc(0xc0 | (int)(cp >> 6), f); fputc(0x80 | (int)(cp & 0x3f), f); return; }
    if (cp < 0x10000) {
        fputc(0xe0 | (int)(cp >> 12), f); fputc(0x80 | (int)((cp >> 6) & 0x3f), f); fputc(0x80 | (int)(cp & 0x3f), f);
        return;
    }
    fputc(0xf0 | (int)(cp >> 18), f); fputc(0x80 | (int)((cp >> 12) & 0x3f), f);
    fputc(0x80 | (int)((cp >> 6) & 0x3f), f); fputc(0x80 | (int)(cp & 0x3f), f);
}

static void emit_rgb(FILE *f, uint32_t c) {
    fprintf(f, "%u;%u;%u", (c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static void write_frame(FILE *f) {
    for (int r = 0; r < ROWS; r++) {
        int end = COLS;   /* trailing blanks with no colour are not written */
        while (end > 0) {
            cell c = grid[r * COLS + end - 1];
            if ((c.cp == ' ' || c.cp == 0) && !c.fg && !c.bg) end--;
            else break;
        }
        uint32_t cfg = 0, cbg = 0; unsigned char cb = 0, cd = 0;
        for (int x = 0; x < end; x++) {
            cell c = grid[r * COLS + x];
            if (c.cp == 0) continue;   /* the second cell of a wide character */
            if (c.fg != cfg || c.bg != cbg || c.bold != cb || c.dim != cd) {
                fputs("\x1b[0m", f);
                if (c.fg) { fputs("\x1b[38;2;", f); emit_rgb(f, c.fg); fputc('m', f); }
                if (c.bg) { fputs("\x1b[48;2;", f); emit_rgb(f, c.bg); fputc('m', f); }
                if (c.bold) fputs("\x1b[1m", f);
                if (c.dim) fputs("\x1b[2m", f);
                cfg = c.fg; cbg = c.bg; cb = c.bold; cd = c.dim;
            }
            utf8(f, c.cp);
        }
        fputs("\x1b[0m\n", f);
    }
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: screen CAPTURE COLS ROWS OUT.ansi\n");
        return 2;
    }
    COLS = atoi(argv[2]);
    ROWS = atoi(argv[3]);
    if (COLS < 1 || ROWS < 1) { fprintf(stderr, "bad size\n"); return 2; }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    size_t cap = 1 << 16, len = 0;
    unsigned char *d = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; d = realloc(d, cap); }
        size_t got = fread(d + len, 1, cap - len, in);
        if (got == 0) break;
        len += got;
    }
    fclose(in);

    grid = calloc((size_t)COLS * (size_t)ROWS, sizeof *grid);
    for (int i = 0; i < COLS * ROWS; i++) { grid[i].cp = ' '; }
    run(d, len);
    free(d);

    FILE *out = fopen(argv[4], "w");
    if (!out) { perror(argv[4]); return 1; }
    write_frame(out);
    fclose(out);
    printf("%d rows -> %s\n", ROWS, argv[4]);
    free(grid);
    return 0;
}
