#define _GNU_SOURCE
#include "common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

config_t g_cfg;
session_stats g_session;

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }
char *xstrdup(const char *s) { if (!s) return NULL; char *d = strdup(s); if (!d) die("out of memory"); return d; }
char *xstrndup(const char *s, size_t n) { char *d = xmalloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }

void die(const char *fmt, ...) {
    term_restore();
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "corbienest: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---------- sbuf ---------- */
void sb_init(sbuf *b) { b->data = NULL; b->len = b->cap = 0; }
void sb_free(sbuf *b) { free(b->data); sb_init(b); }
void sb_clear(sbuf *b) { b->len = 0; if (b->data) b->data[0] = 0; }
static void sb_reserve(sbuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + extra + 1) nc *= 2;
        b->data = xrealloc(b->data, nc);
        b->cap = nc;
    }
}
void sb_append(sbuf *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
void sb_puts(sbuf *b, const char *s) { sb_append(b, s, strlen(s)); }
void sb_putc(sbuf *b, char c) { sb_append(b, &c, 1); }
void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sb_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}
char *sb_detach(sbuf *b) {
    char *d = b->data ? b->data : xstrdup("");
    sb_init(b);
    return d;
}

/* ---------- files ---------- */
char *read_whole_file(const char *path, size_t *len_out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    sbuf b; sb_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (cap && b.len + n > cap) n = cap - b.len;
        sb_append(&b, tmp, n);
        if (cap && b.len >= cap) break;
    }
    fclose(f);
    if (len_out) *len_out = b.len;
    if (!b.data) return xstrdup("");
    return b.data;
}

int write_whole_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

int mkdir_p(const char *path) {
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) != 0 && errno != EEXIST) { free(p); return -1; }
            *s = '/';
        }
    }
    int r = (mkdir(p, 0755) != 0 && errno != EEXIST) ? -1 : 0;
    free(p);
    return r;
}

char *expand_home(const char *path) {
    if (path[0] == '~' && (path[1] == '/' || path[1] == 0)) {
        const char *home = getenv("HOME");
        if (!home) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : "/"; }
        sbuf b; sb_init(&b);
        sb_puts(&b, home); sb_puts(&b, path + 1);
        return sb_detach(&b);
    }
    return xstrdup(path);
}

int is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
int is_file(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISREG(st.st_mode); }

/* ---------- config ---------- */
const char *config_dir(void) {
    static char dir[1024];
    if (!dir[0]) {
        const char *x = getenv("XDG_CONFIG_HOME");
        if (x && *x) snprintf(dir, sizeof dir, "%s/corbienest", x);
        else {
            char *h = expand_home("~/.config/corbienest");
            snprintf(dir, sizeof dir, "%s", h);
            free(h);
        }
        mkdir_p(dir);
    }
    return dir;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}

void config_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/config", config_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "model")) { free(g_cfg.model); g_cfg.model = xstrdup(v); }
        else if (!strcmp(k, "host")) { free(g_cfg.host); g_cfg.host = xstrdup(v); }
        else if (!strcmp(k, "num_ctx")) g_cfg.num_ctx = atoi(v);
        else if (!strcmp(k, "temperature")) g_cfg.temperature = atof(v);
        else if (!strcmp(k, "think")) g_cfg.think = atoi(v);
        else if (!strcmp(k, "think_level")) { free(g_cfg.think_level); g_cfg.think_level = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "show_thinking")) g_cfg.show_thinking = atoi(v) != 0;
        else if (!strcmp(k, "yolo")) { if (atoi(v)) g_cfg.mode = MODE_AUTO; }
        else if (!strcmp(k, "mode")) { int m = mode_parse(v); if (m >= 0) g_cfg.mode = m; }
        else if (!strcmp(k, "memory")) g_cfg.memory = atoi(v) != 0;
        else if (!strcmp(k, "memory_every")) { int n = atoi(v); if (n > 0) g_cfg.memory_every = n; }
        else if (!strcmp(k, "memory_idle")) { int n = atoi(v); if (n >= 0) g_cfg.memory_idle = n; }
        else if (!strcmp(k, "keep_alive")) { free(g_cfg.keep_alive); g_cfg.keep_alive = *v ? xstrdup(v) : NULL; }
    }
    fclose(f);
}

void config_save(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/config", config_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# corbienest config (auto-written)\n");
    if (g_cfg.model) fprintf(f, "model=%s\n", g_cfg.model);
    if (g_cfg.host) fprintf(f, "host=%s\n", g_cfg.host);
    fprintf(f, "num_ctx=%d\n", g_cfg.num_ctx);
    if (g_cfg.temperature >= 0) fprintf(f, "temperature=%g\n", g_cfg.temperature);
    fprintf(f, "think=%d\n", g_cfg.think);
    fprintf(f, "think_level=%s\n", g_cfg.think_level ? g_cfg.think_level : "");
    fprintf(f, "show_thinking=%d\n", g_cfg.show_thinking ? 1 : 0);
    fprintf(f, "mode=%s\n", mode_name(g_cfg.mode));
    fprintf(f, "memory=%d\n", g_cfg.memory ? 1 : 0);
    fprintf(f, "memory_every=%d\n", g_cfg.memory_every);
    fprintf(f, "memory_idle=%d\n", g_cfg.memory_idle);
    fprintf(f, "keep_alive=%s\n", g_cfg.keep_alive ? g_cfg.keep_alive : "");
    fclose(f);
}

/* ---------- permission modes ---------- */
static const char *MODE_NAMES[MODE_COUNT]  = { "manual", "accept-edits", "plan", "auto" };
static const char *MODE_LABELS[MODE_COUNT] = {
    "manual — confirm every edit and command",
    "accept edits — file edits auto-approved, commands still confirmed",
    "plan — read-only: explore and propose a plan, no edits",
    "auto — every tool call auto-approved (yolo)",
};
const char *mode_name(int mode)  { return mode >= 0 && mode < MODE_COUNT ? MODE_NAMES[mode] : "?"; }
const char *mode_label(int mode) { return mode >= 0 && mode < MODE_COUNT ? MODE_LABELS[mode] : "?"; }
int mode_parse(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < MODE_COUNT; i++) if (!strcasecmp(s, MODE_NAMES[i])) return i;
    if (!strcasecmp(s, "yolo") || !strcasecmp(s, "bypass")) return MODE_AUTO;
    if (!strcasecmp(s, "edits") || !strcasecmp(s, "accept") || !strcasecmp(s, "accept_edits") || !strcasecmp(s, "acceptEdits")) return MODE_ACCEPT_EDITS;
    if (!strcasecmp(s, "default") || !strcasecmp(s, "normal") || !strcasecmp(s, "off")) return MODE_MANUAL;
    return -1;
}
