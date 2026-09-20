#define _GNU_SOURCE
#include "common.h"
#include <errno.h>
#include <fcntl.h>
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

int write_whole_file_atomic(const char *path, const char *data, size_t len) {
    char tmp[1400];
    if (snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof tmp) return -1;
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 07777) : 0600;   /* keep what was there */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    for (size_t off = 0; off < len; ) {
        ssize_t k = write(fd, data + off, len - off);
        if (k < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        off += (size_t)k;
    }
    /* the data has to be on disk before the name points at it, or a crash between the two
     * leaves a file that is present, named right and empty */
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
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
    char *legacy_level = NULL;   /* think_level=: one level for every model, from before levels were kept per model */
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        /* key=value, split at the first '=' — except effort.<model>=<level>, at the last: a model name may hold one, a level never does */
        char *eq = !strncmp(line, "effort.", 7) ? strrchr(line, '=') : strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "model")) { free(g_cfg.model); g_cfg.model = xstrdup(v); }
        else if (!strcmp(k, "host")) { free(g_cfg.host); g_cfg.host = xstrdup(v); }
        else if (!strcmp(k, "num_ctx")) g_cfg.num_ctx = atoi(v);
        else if (!strcmp(k, "temperature")) g_cfg.temperature = atof(v);
        else if (!strcmp(k, "think")) g_cfg.think = atoi(v);
        else if (!strcmp(k, "think_level")) { free(legacy_level); legacy_level = *v ? xstrdup(v) : NULL; }
        else if (!strncmp(k, "effort.", 7)) { if (k[7] && effort_name_ok(v)) effort_set(k + 7, v); }
        else if (!strcmp(k, "advisor")) { free(g_cfg.advisor); g_cfg.advisor = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "advisor_ctx")) { int n = atoi(v); if (n == 0 || n >= ADVISOR_CTX_MIN) g_cfg.advisor_ctx = n; }
        else if (!strcmp(k, "show_thinking")) g_cfg.show_thinking = atoi(v) != 0;
        else if (!strcmp(k, "yolo")) { if (atoi(v)) g_cfg.mode = MODE_AUTO; }
        else if (!strcmp(k, "mode")) { int m = mode_parse(v); if (m >= 0) g_cfg.mode = m; }
        else if (!strcmp(k, "memory")) g_cfg.memory = atoi(v) != 0;
        else if (!strcmp(k, "web")) g_cfg.web = atoi(v) != 0;
        else if (!strcmp(k, "search_url")) { free(g_cfg.search_url); g_cfg.search_url = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "max_iters")) { int n = atoi(v); if (n > 0) g_cfg.max_iters = n; }
        else if (!strcmp(k, "memory_every")) { int n = atoi(v); if (n > 0) g_cfg.memory_every = n; }
        else if (!strcmp(k, "memory_idle")) { int n = atoi(v); if (n >= 0) g_cfg.memory_idle = n; }
        else if (!strcmp(k, "keep_alive")) { free(g_cfg.keep_alive); g_cfg.keep_alive = *v ? xstrdup(v) : NULL; }
    }
    fclose(f);
    /* the old global level belonged to whatever model was in use when it was set */
    if (legacy_level && effort_name_ok(legacy_level) && g_cfg.model && !effort_get(g_cfg.model)) effort_set(g_cfg.model, legacy_level);
    free(legacy_level);
}

/* The whole file is rewritten on every setting change, and another session may be reading it
 * (or writing it) at that moment — so it is built in memory and swapped in in one step. This
 * makes a torn or truncated config impossible; it does not merge two sessions' settings, and
 * is not meant to: whoever saved last is what the next session starts with. */
void config_save(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/config", config_dir());
    sbuf b; sb_init(&b);
    sb_puts(&b, "# corbienest config (auto-written)\n");
    if (g_cfg.model) sb_printf(&b, "model=%s\n", g_cfg.model);
    if (g_cfg.host) sb_printf(&b, "host=%s\n", g_cfg.host);
    sb_printf(&b, "num_ctx=%d\n", g_cfg.num_ctx);
    if (g_cfg.temperature >= 0) sb_printf(&b, "temperature=%g\n", g_cfg.temperature);
    sb_printf(&b, "think=%d\n", g_cfg.think);
    for (int i = 0; i < g_cfg.n_efforts; i++) sb_printf(&b, "effort.%s=%s\n", g_cfg.efforts[i].model, g_cfg.efforts[i].level);
    if (g_cfg.advisor) sb_printf(&b, "advisor=%s\n", g_cfg.advisor);
    if (g_cfg.advisor_ctx > 0) sb_printf(&b, "advisor_ctx=%d\n", g_cfg.advisor_ctx);
    sb_printf(&b, "show_thinking=%d\n", g_cfg.show_thinking ? 1 : 0);
    sb_printf(&b, "mode=%s\n", mode_name(g_cfg.mode));
    sb_printf(&b, "max_iters=%d\n", g_cfg.max_iters);
    sb_printf(&b, "memory=%d\n", g_cfg.memory ? 1 : 0);
    sb_printf(&b, "web=%d\n", g_cfg.web ? 1 : 0);
    if (g_cfg.search_url) sb_printf(&b, "search_url=%s\n", g_cfg.search_url);
    sb_printf(&b, "memory_every=%d\n", g_cfg.memory_every);
    sb_printf(&b, "memory_idle=%d\n", g_cfg.memory_idle);
    sb_printf(&b, "keep_alive=%s\n", g_cfg.keep_alive ? g_cfg.keep_alive : "");
    write_whole_file_atomic(path, b.data, b.len);
    sb_free(&b);
}

/* ---------- effort: how hard each model thinks ---------- */
bool effort_name_ok(const char *s) {
    if (!s || !*s || strlen(s) >= EFFORT_NAME_MAX) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return false;
    return true;
}

/* "qwen3.8" and "qwen3.8:latest" are one model to the server */
bool model_same(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la > 7 && !strcmp(a + la - 7, ":latest")) la -= 7;
    if (lb > 7 && !strcmp(b + lb - 7, ":latest")) lb -= 7;
    return la == lb && !strncmp(a, b, la);
}

/* One of Ollama's cloud models: the local server relays the call to ollama.com */
bool model_is_cloud(const char *name) {
    size_t l = name ? strlen(name) : 0;
    return l > 6 && (!strcmp(name + l - 6, ":cloud") || !strcmp(name + l - 6, "-cloud"));
}

const char *effort_get(const char *model) {
    for (int i = 0; model && i < g_cfg.n_efforts; i++) if (model_same(g_cfg.efforts[i].model, model)) return g_cfg.efforts[i].level;
    return NULL;
}

void effort_set(const char *model, const char *level) {
    if (!model || !*model || strchr(model, '\n')) return;
    int i = 0;
    while (i < g_cfg.n_efforts && !model_same(g_cfg.efforts[i].model, model)) i++;
    if (!level) {   /* forget it */
        if (i == g_cfg.n_efforts) return;
        free(g_cfg.efforts[i].model); free(g_cfg.efforts[i].level);
        g_cfg.efforts[i] = g_cfg.efforts[--g_cfg.n_efforts];
        return;
    }
    if (i == g_cfg.n_efforts) {
        g_cfg.efforts = xrealloc(g_cfg.efforts, sizeof *g_cfg.efforts * (size_t)(g_cfg.n_efforts + 1));
        g_cfg.efforts[g_cfg.n_efforts++] = (effort_entry){ xstrdup(model), NULL };
    }
    free(g_cfg.efforts[i].level); g_cfg.efforts[i].level = xstrdup(level);
}

int effort_supported(const model_info *mi, const char *level) {
    if (!mi->thinking || !effort_name_ok(level)) return 0;
    if (!strcmp(level, "off")) return mi->think_off;
    if (!strcmp(level, "on")) return mi->think_on;
    for (int i = 0; i < mi->n_think_levels; i++) if (!strcmp(mi->think_levels[i], level)) return 1;
    if (mi->think_declared || mi->n_think_levels) return 0;   /* its levels are known, and this is not one of them */
    /* an on/off model as far as anyone knows: the server still takes the four names it has always known */
    return (!strcmp(level, "low") || !strcmp(level, "medium") || !strcmp(level, "high") || !strcmp(level, "max")) ? -1 : 0;
}

/* What is sent for a saved level: the level itself when the model has it (or may), and for
 * "as hard as it goes" — high, xhigh, max — the strongest one the model does have. The names of
 * the top level differ between models and between server versions (qwen3.8: "high" up to
 * Ollama 0.34.2, "xhigh" from 0.34.3, where "high" quietly means medium), and what the user
 * meant is the same. NULL = nothing the model can be set to. */
const char *effort_resolve(const model_info *mi, const char *want, bool *mapped) {
    if (mapped) *mapped = false;
    if (!want) return NULL;
    if (effort_supported(mi, want) != 0) return want;
    static const char *TOP[] = { "high", "xhigh", "max" };
    const char *strongest = mi->thinking && mi->n_think_levels ? mi->think_levels[mi->n_think_levels - 1] : NULL;
    bool want_top = false, has_top = false;
    for (int i = 0; i < 3; i++) { if (!strcmp(want, TOP[i])) want_top = true; if (strongest && !strcmp(strongest, TOP[i])) has_top = true; }
    if (!want_top || !has_top) return NULL;
    if (mapped) *mapped = true;
    return strongest;
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

/* ---------- URLs and HTML (for the web_fetch tool) ----------
 * Kept here rather than as statics in tools.c so the unit tests can reach them: test_unit
 * links every object but main.o and sees only what common.h declares. */

/* Host (with :port) of an http(s) URL, lowercased. false if there is none. */
bool url_host(const char *url, char *out, size_t n) {
    if (!url || !n) return false;
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    size_t i = 0;
    for (; *p && *p != '/' && *p != '?' && *p != '#' && i + 1 < n; p++, i++)
        out[i] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    out[i] = 0;
    /* strip user@ if any — the host is what a permission rule is about */
    char *at = strrchr(out, '@');
    if (at) memmove(out, at + 1, strlen(at + 1) + 1);
    return out[0] != 0;
}

/* Is this a URL we are willing to hand to curl? http(s) only, sane length, no control
 * characters or whitespace (it goes through sh_quote, but a URL with a newline in it is
 * never what the user meant). The cloud metadata addresses are refused outright: the model
 * reads files it does not control, and that is the one target where a talked-into fetch
 * hands out credentials. The user can still reach them with bash if they mean to. */
bool url_ok(const char *url) {
    if (!url) return false;
    size_t n = strlen(url);
    if (n < 11 || n > 2048) return false;
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return false;
    for (const char *p = url; *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7f) return false;
    char host[512];
    if (!url_host(url, host, sizeof host)) return false;
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
    if (!host[0]) return false;
    if (!strcmp(host, "169.254.169.254") || !strcmp(host, "metadata.google.internal") ||
        !strcmp(host, "metadata.goog") || !strcmp(host, "[fd00:ec2::254]")) return false;
    return true;
}

/* ---------- HTML -> text ---------- */
static bool tag_is(const char *p, const char *name, size_t nlen) {
    /* p points just past '<' (or '</'); does the tag name match? */
    if (strncasecmp(p, name, nlen)) return false;
    char c = p[nlen];
    return c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/';
}

/* Named entities worth knowing; everything else numeric is decoded, the rest passed through. */
static const struct { const char *name; const char *utf8; } ENTITIES[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
    { "nbsp", " " }, { "mdash", "—" }, { "ndash", "–" }, { "hellip", "…" },
    { "rsquo", "’" }, { "lsquo", "‘" }, { "ldquo", "“" }, { "rdquo", "”" },
    { "middot", "·" }, { "bull", "•" }, { "copy", "©" }, { "reg", "®" }, { "trade", "™" },
    { "laquo", "«" }, { "raquo", "»" }, { "times", "×" }, { "deg", "°" }, { "hyphen", "-" },
    { NULL, NULL }
};

static void put_codepoint(sbuf *b, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF) return;
    if (cp < 0x80) sb_putc(b, (char)cp);
    else if (cp < 0x800) { sb_putc(b, (char)(0xC0 | (cp >> 6))); sb_putc(b, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { sb_putc(b, (char)(0xE0 | (cp >> 12))); sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F))); sb_putc(b, (char)(0x80 | (cp & 0x3F))); }
    else { sb_putc(b, (char)(0xF0 | (cp >> 18))); sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F))); sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F))); sb_putc(b, (char)(0x80 | (cp & 0x3F))); }
}

/* Decode one entity at *p (which points at '&'); advances *p past it and appends. */
static void decode_entity(sbuf *b, const char **p) {
    const char *s = *p + 1;
    const char *semi = NULL;
    for (const char *q = s; *q && q - s < 12; q++) if (*q == ';') { semi = q; break; }
    if (!semi) { sb_putc(b, '&'); (*p)++; return; }
    size_t n = (size_t)(semi - s);
    if (*s == '#') {
        unsigned long cp = (s[1] == 'x' || s[1] == 'X') ? strtoul(s + 2, NULL, 16) : strtoul(s + 1, NULL, 10);
        put_codepoint(b, cp);
        *p = semi + 1;
        return;
    }
    for (int i = 0; ENTITIES[i].name; i++)
        if (strlen(ENTITIES[i].name) == n && !strncasecmp(ENTITIES[i].name, s, n)) {
            sb_puts(b, ENTITIES[i].utf8);
            *p = semi + 1;
            return;
        }
    sb_putc(b, '&'); (*p)++;
}

/* Value of an attribute inside a tag body (p .. end), malloc'd, or NULL. */
static char *attr_value(const char *start, const char *end, const char *name) {
    size_t nlen = strlen(name);
    for (const char *p = start; p + nlen < end; p++) {
        if (strncasecmp(p, name, nlen)) continue;
        if (p > start && !(p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r')) continue;
        const char *q = p + nlen;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != '=') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        char quote = (q < end && (*q == '"' || *q == '\'')) ? *q : 0;
        if (quote) q++;
        const char *v = q;
        while (q < end && (quote ? *q != quote : (*q != ' ' && *q != '\t' && *q != '>'))) q++;
        if (q == v) return NULL;
        sbuf b; sb_init(&b);
        for (const char *c = v; c < q; ) { if (*c == '&') decode_entity(&b, &c); else sb_putc(&b, *c++); }
        return sb_detach(&b);
    }
    return NULL;
}

/* Collapse the assembled text: trailing spaces off, runs of blank lines down to two. */
static char *tidy_text(sbuf *raw) {
    sbuf out; sb_init(&out);
    const char *p = raw->data ? raw->data : "";
    int blanks = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && (p[n-1] == ' ' || p[n-1] == '\t' || p[n-1] == '\r')) n--;
        size_t lead = 0;
        while (lead < n && (p[lead] == ' ' || p[lead] == '\t')) lead++;
        if (lead == n) {
            if (++blanks <= 1) sb_putc(&out, '\n');
        } else {
            blanks = 0;
            sb_append(&out, p, n);
            sb_putc(&out, '\n');
        }
        if (!e) break;
        p = e + 1;
    }
    sb_free(raw);
    return sb_detach(&out);
}

/* A readable-text rendering of an HTML page: what the model should see instead of markup.
 * <script>/<style>/comments go away, block elements become line breaks, list items get a
 * "- ", <pre> keeps its whitespace inside a ``` fence (API docs are mostly code samples),
 * and links keep their target so the model can follow the docs to the next page — a rooted
 * href is resolved against `base` ("scheme://host") so what it sees is fetchable as it is. */
char *html_to_text(const char *html, size_t len, const char *base) {
    sbuf b; sb_init(&b);
    const char *p = html, *end = html + len;
    int pre = 0;            /* inside <pre>: keep whitespace verbatim */
    bool sol = true;        /* at the start of a line (for collapsing whitespace) */
    char *link = NULL;      /* href of the <a> we are inside */
    size_t link_at = 0;     /* where its text started */

    while (p < end) {
        if (*p != '<') {
            if (*p == '&') { const char *q = p; decode_entity(&b, &q); p = q; sol = false; continue; }
            if (pre) { sb_putc(&b, *p); if (*p == '\n') sol = true; else sol = false; p++; continue; }
            if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') {
                if (!sol) { sb_putc(&b, ' '); sol = true; }   /* one space stands for any run */
                p++;
                continue;
            }
            sb_putc(&b, *p++);
            sol = false;
            continue;
        }
        /* a tag (or something that only looks like one) */
        if (p + 3 < end && !strncmp(p, "<!--", 4)) {
            const char *e = NULL;
            for (const char *q = p + 4; q + 2 < end; q++) if (!strncmp(q, "-->", 3)) { e = q + 3; break; }
            p = e ? e : end;
            continue;
        }
        const char *name = p + 1;
        bool closing = (name < end && *name == '/');
        if (closing) name++;
        if (name >= end || !((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') || *name == '!')) {
            sb_putc(&b, *p++);   /* a bare '<' in the text */
            sol = false;
            continue;
        }
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        const char *body_end = gt;

        /* elements whose entire content is dropped */
        if (!closing && (tag_is(name, "script", 6) || tag_is(name, "style", 5) || tag_is(name, "head", 4) ||
                         tag_is(name, "noscript", 8) || tag_is(name, "svg", 3))) {
            const char *close = NULL;
            size_t nlen = tag_is(name, "script", 6) ? 6 : tag_is(name, "style", 5) ? 5 :
                          tag_is(name, "head", 4) ? 4 : tag_is(name, "noscript", 8) ? 8 : 3;
            /* <head> is skipped except for its <title>, which is worth the first line */
            if (nlen == 4) {
                const char *t = NULL;
                for (const char *q = gt; q + 7 < end; q++)
                    if (*q == '<' && tag_is(q + 1, "title", 5)) { t = memchr(q, '>', (size_t)(end - q)); break; }
                if (t) {
                    for (const char *q = t + 1; q < end && *q != '<'; ) {
                        if (*q == '&') decode_entity(&b, &q);
                        else if (*q == '\n' || *q == '\t' || *q == '\r') { sb_putc(&b, ' '); q++; }
                        else sb_putc(&b, *q++);
                    }
                    sb_puts(&b, "\n\n");
                    sol = true;
                }
            }
            for (const char *q = gt; q + nlen + 2 < end; q++)
                if (*q == '<' && q[1] == '/' && !strncasecmp(q + 2, name, nlen)) { close = memchr(q, '>', (size_t)(end - q)); break; }
            p = close ? close + 1 : end;
            continue;
        }
        if (tag_is(name, "pre", 3)) {
            if (closing) { if (pre > 0 && --pre == 0) { sb_puts(&b, "\n```\n"); sol = true; } }
            else { if (pre++ == 0) { sb_puts(&b, "\n```\n"); sol = true; } }
            p = gt + 1;
            continue;
        }
        if (tag_is(name, "li", 2) && !closing) {
            sb_puts(&b, "\n- ");
            sol = true;
            p = gt + 1;
            continue;
        }
        if (tag_is(name, "a", 1)) {
            if (closing) {
                if (link) {
                    /* "text (href)" — but not when the text already is the URL */
                    size_t tl = b.len > link_at ? b.len - link_at : 0;
                    if (tl && (tl != strlen(link) || strncmp(b.data + link_at, link, tl)))
                        sb_printf(&b, " (%s)", link);
                    free(link); link = NULL;
                }
            } else {
                free(link);
                link = attr_value(name, body_end, "href");
                if (link && (link[0] == '#' || (strncasecmp(link, "http", 4) && link[0] != '/'))) { free(link); link = NULL; }
                if (link && link[0] == '/' && base && *base) {   /* "/docs/x" -> "https://host/docs/x" */
                    sbuf u; sb_init(&u); sb_puts(&u, base); sb_puts(&u, link);
                    free(link); link = sb_detach(&u);
                }
                link_at = b.len;
            }
            p = gt + 1;
            continue;
        }
        if (!pre) {
            static const char *BLOCK[] = { "p", "div", "br", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
                                           "ul", "ol", "section", "article", "header", "footer", "table",
                                           "blockquote", "dt", "dd", "hr", "nav", "form", NULL };
            for (int i = 0; BLOCK[i]; i++)
                if (tag_is(name, BLOCK[i], strlen(BLOCK[i]))) {
                    if (!sol || b.len) sb_putc(&b, '\n');
                    sol = true;
                    break;
                }
            if (tag_is(name, "td", 2) || tag_is(name, "th", 2)) { if (!sol) { sb_putc(&b, '\t'); sol = true; } }
        }
        p = gt + 1;
    }
    free(link);
    return tidy_text(&b);
}

/* ---------- search results ----------
 * A search engine's result page, turned into the few lines the model actually needs. The
 * shape is generic on purpose (title link, then the same href again for the display URL and
 * the snippet — which is how DuckDuckGo's HTML endpoint, SearXNG and most others render):
 * collect <a> targets in order, keep the first text as the title, the longest prose repeat
 * as the snippet, and drop everything pointing back at the engine itself. */

/* Percent-encode for a query string value. */
char *url_encode(const char *s) {
    static const char *HEX = "0123456789ABCDEF";
    sbuf b; sb_init(&b);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') sb_putc(&b, (char)*p);
        else if (*p == ' ') sb_putc(&b, '+');
        else { sb_putc(&b, '%'); sb_putc(&b, HEX[*p >> 4]); sb_putc(&b, HEX[*p & 15]); }
    }
    return sb_detach(&b);
}

/* The reverse, for the redirect wrappers engines put around result links. */
char *url_decode(const char *s, size_t n) {
    sbuf b; sb_init(&b);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = -1, lo = -1;
            for (int k = 0; k < 16; k++) {
                if ("0123456789abcdef"[k] == (s[i+1] | 32)) hi = k;
                if ("0123456789abcdef"[k] == (s[i+2] | 32)) lo = k;
            }
            if (hi >= 0 && lo >= 0) { sb_putc(&b, (char)(hi * 16 + lo)); i += 2; continue; }
        }
        sb_putc(&b, s[i] == '+' ? ' ' : s[i]);
    }
    return sb_detach(&b);
}

/* "…/l/?uddg=https%3A%2F%2Fx" -> "https://x"; also Google/SearXNG's "?url=" and "?q=". */
static char *unwrap_redirect(char *href) {
    static const char *KEYS[] = { "uddg=", "url=", "q=", NULL };
    for (int i = 0; KEYS[i]; i++) {
        const char *p = strstr(href, KEYS[i]);
        if (!p || (p != href && p[-1] != '?' && p[-1] != '&')) continue;
        p += strlen(KEYS[i]);
        size_t n = strcspn(p, "&");
        if (!n) continue;
        char *dec = url_decode(p, n);
        if (!strncasecmp(dec, "http", 4)) { free(href); return dec; }   /* a real target, not the query text */
        free(dec);
    }
    return href;
}

/* Tag-free, entity-decoded, whitespace-collapsed text of an HTML fragment. */
static char *flatten(const char *s, const char *end) {
    sbuf b; sb_init(&b);
    bool space = true;
    while (s < end) {
        if (*s == '<') { const char *gt = memchr(s, '>', (size_t)(end - s)); if (!gt) break; s = gt + 1; continue; }
        if (*s == '&') { const char *q = s; decode_entity(&b, &q); s = q; space = false; continue; }
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') { if (!space) { sb_putc(&b, ' '); space = true; } s++; continue; }
        sb_putc(&b, *s++);
        space = false;
    }
    while (b.len && b.data[b.len-1] == ' ') b.data[--b.len] = 0;
    return sb_detach(&b);
}

#define SEARCH_MAX_HITS 40
typedef struct { char *url, *title, *snippet; } hit_t;

char *search_results_text(const char *html, size_t len, const char *engine_url, int max, int *count) {
    char ehost[512] = "";
    url_host(engine_url, ehost, sizeof ehost);
    char base[600] = "";
    { const char *sl = engine_url ? strchr(engine_url + 8, '/') : NULL;
      if (engine_url) snprintf(base, sizeof base, "%.*s", sl ? (int)(sl - engine_url) : (int)strlen(engine_url), engine_url); }
    if (max < 1) max = 1;
    if (max > SEARCH_MAX_HITS) max = SEARCH_MAX_HITS;

    hit_t hits[SEARCH_MAX_HITS];
    int n = 0;
    const char *p = html, *end = html + len;
    while (p < end && (p = memchr(p, '<', (size_t)(end - p)))) {
        if (!(p + 2 < end && (p[1] == 'a' || p[1] == 'A') && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n'))) { p++; continue; }
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        char *href = attr_value(p + 2, gt, "href");
        const char *close = NULL;
        for (const char *q = gt; q + 3 < end; q++)
            if (q[0] == '<' && q[1] == '/' && (q[2] == 'a' || q[2] == 'A')) { close = q; break; }
        const char *text_end = close ? close : gt + 1;
        char *text = flatten(gt + 1, text_end);
        p = close ? close + 3 : gt + 1;
        if (!href) { free(text); continue; }

        if (!strncmp(href, "//", 2)) { sbuf u; sb_init(&u); sb_printf(&u, "https:%s", href); free(href); href = sb_detach(&u); }
        else if (href[0] == '/' && base[0]) { sbuf u; sb_init(&u); sb_printf(&u, "%s%s", base, href); free(href); href = sb_detach(&u); }
        href = unwrap_redirect(href);

        char hhost[512] = "";
        if (strncasecmp(href, "http", 4) || !url_host(href, hhost, sizeof hhost) ||
            (ehost[0] && !strcmp(hhost, ehost))) { free(href); free(text); continue; }

        int at = -1;
        for (int i = 0; i < n; i++) if (!strcmp(hits[i].url, href)) { at = i; break; }
        if (at < 0) {
            if (n == max || n == SEARCH_MAX_HITS) { free(href); free(text); continue; }
            hits[n].url = href; hits[n].title = text; hits[n].snippet = NULL;
            n++;
            continue;
        }
        /* a repeat of a result we have: prose (has spaces, has length) is its snippet */
        if (strchr(text, ' ') && strlen(text) > 40 && (!hits[at].snippet || strlen(text) > strlen(hits[at].snippet))) {
            free(hits[at].snippet); hits[at].snippet = text;
        } else free(text);
        free(href);
    }

    sbuf out; sb_init(&out);
    for (int i = 0; i < n; i++) {
        sb_printf(&out, "%d. %s\n   %s\n", i + 1, hits[i].title && *hits[i].title ? hits[i].title : "(no title)", hits[i].url);
        if (hits[i].snippet) {
            const char *s = hits[i].snippet;
            size_t sl = strlen(s);
            if (sl > 300) { size_t cut = 300; while (cut && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--; sb_printf(&out, "   %.*s…\n", (int)cut, s); }
            else sb_printf(&out, "   %s\n", s);
        }
        free(hits[i].url); free(hits[i].title); free(hits[i].snippet);
    }
    if (count) *count = n;
    return sb_detach(&out);
}

/* ---------- a conversation as quoted text (for the advisor) ----------
 * The advisor is shown the conversation, it is not part of it: handing it the messages as they
 * are would make it carry on in the agent's voice — and answer tool results it has no tools
 * for. So the conversation becomes one text, a block per message. Long messages lose their
 * middle (both ends of a file or a log are what locates it); when the whole does not fit the
 * budget the newest blocks win, since that is where the agent is stuck, and `keep` — the
 * request being worked on — stays whatever happens. */
#define TRANSCRIPT_USER_CAP   6000
#define TRANSCRIPT_AGENT_CAP  4000
#define TRANSCRIPT_RESULT_CAP 3000
#define TRANSCRIPT_ARGS_CAP   600

void sb_put_cut(sbuf *b, const char *s, size_t cap) {
    size_t len = strlen(s);
    if (len <= cap) { sb_append(b, s, len); return; }
    size_t head = cap * 2 / 3, tail = cap - head;
    while (head && ((unsigned char)s[head] & 0xC0) == 0x80) head--;            /* UTF-8 boundaries */
    const char *t = s + len - tail;
    while (tail && ((unsigned char)*t & 0xC0) == 0x80) { t++; tail--; }
    sb_append(b, s, head);
    sb_printf(b, "\n[… %zu bytes cut …]\n", len - head - tail);
    sb_append(b, t, tail);
}

/* one message as a block, or NULL when it says nothing (an assistant turn that only called the advisor) */
static char *transcript_block(cJSON *m, size_t budget) {
    cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
    const char *r = cJSON_IsString(role) ? role->valuestring : "user";
    const char *text = cJSON_IsString(c) ? c->valuestring : "";
    size_t share = budget / 4;   /* no single message may crowd the others out */
    #define CAP(n) ((size_t)(n) < share ? (size_t)(n) : share)
    sbuf b; sb_init(&b);
    if (!strcmp(r, "tool")) {
        cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name");
        sb_printf(&b, "[result of %s]\n", cJSON_IsString(tn) ? tn->valuestring : "tool");
        sb_put_cut(&b, *text ? text : "(empty)", CAP(TRANSCRIPT_RESULT_CAP));
    } else if (!strcmp(r, "assistant")) {
        sbuf calls; sb_init(&calls);
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls"), *call;
        cJSON_ArrayForEach(call, tc) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            if (!cJSON_IsString(nm) || !strcmp(nm->valuestring, "advisor")) continue;   /* the question is handed over separately */
            char *a = cJSON_IsString(args) ? xstrdup(args->valuestring) : args ? cJSON_PrintUnformatted(args) : xstrdup("{}");
            size_t al = strlen(a), cut = al > TRANSCRIPT_ARGS_CAP ? TRANSCRIPT_ARGS_CAP : al;
            while (cut && cut < al && ((unsigned char)a[cut] & 0xC0) == 0x80) cut--;
            sb_printf(&calls, "\n→ %s(%.*s%s)", nm->valuestring, (int)cut, a, cut < al ? "…" : "");
            free(a);
        }
        if (!*text && !calls.len) { sb_free(&calls); return NULL; }
        sb_puts(&b, "[agent]\n");
        sb_put_cut(&b, text, CAP(TRANSCRIPT_AGENT_CAP));
        if (calls.len) sb_puts(&b, *text ? calls.data : calls.data + 1);
        sb_free(&calls);
    } else {
        sb_puts(&b, "[user]\n");
        sb_put_cut(&b, *text ? text : "(empty)", CAP(TRANSCRIPT_USER_CAP));
    }
    #undef CAP
    sb_puts(&b, "\n\n");
    return sb_detach(&b);
}

char *transcript_text(cJSON *msgs, int keep, size_t budget) {
    int n = cJSON_GetArraySize(msgs), i = 0;
    char **blk = xmalloc(sizeof(char*) * (size_t)n);
    bool *in = xmalloc(sizeof(bool) * (size_t)n);
    cJSON *m;
    cJSON_ArrayForEach(m, msgs) { blk[i] = transcript_block(m, budget); in[i] = false; i++; }
    size_t used = 0;
    if (keep >= 0 && keep < n && blk[keep]) { in[keep] = true; used = strlen(blk[keep]); }
    for (i = n - 1; i >= 0; i--) {   /* newest first, and no holes: stop at the first that does not fit */
        if (in[i] || !blk[i]) continue;
        size_t l = strlen(blk[i]);
        if (used + l > budget) break;
        in[i] = true; used += l;
    }
    sbuf o; sb_init(&o); sb_append(&o, "", 0);
    int gap = 0;
    for (i = 0; i < n; i++) {
        if (!blk[i]) continue;
        if (!in[i]) { gap++; continue; }
        if (gap) { sb_printf(&o, "[… %d earlier message%s left out to fit …]\n\n", gap, gap == 1 ? "" : "s"); gap = 0; }
        sb_puts(&o, blk[i]);
    }
    for (i = 0; i < n; i++) free(blk[i]);
    free(blk); free(in);
    return sb_detach(&o);
}

/* ---------- the advisor: where it runs, and what it is shown ----------
 * Three placements, and they want opposite things. The main model as its own advisor must be
 * called exactly as the main model is: another num_ctx makes the server reload it, and
 * keep_alive 0 would unload it — either way the conversation's cache is gone. A cloud model
 * runs at ollama.com with its own window and no memory of ours to give back, so neither key
 * means anything. Another local model gets a window of its own (a 70B model pays several times
 * the memory per token of context that a small one does) and keep_alive 0, so that its memory is
 * free again when the main model wants it back. */
void advisor_plan_for(const char *advisor, int trained_ctx, advisor_plan *p) {
    memset(p, 0, sizeof *p);
    p->same = model_same(advisor, g_cfg.model);
    p->cloud = !p->same && model_is_cloud(advisor);
    if (p->same) { p->window = g_cfg.num_ctx > 0 ? g_cfg.num_ctx : ADVISOR_CTX_UNSET; }
    else if (p->cloud) {
        p->window = trained_ctx > 0 && trained_ctx < ADVISOR_CTX_CLOUD ? trained_ctx : ADVISOR_CTX_CLOUD;
        p->send_ctx = -1; p->keep_alive = "";
    } else {
        int n = g_cfg.advisor_ctx > 0 ? g_cfg.advisor_ctx : g_cfg.num_ctx > ADVISOR_CTX_AUTO ? ADVISOR_CTX_AUTO : g_cfg.num_ctx;
        if (n > 0 && trained_ctx > 0 && n > trained_ctx) n = trained_ctx;
        p->window = n > 0 ? n : ADVISOR_CTX_UNSET;   /* left to the server: plan for the little it gives */
        p->send_ctx = n; p->keep_alive = "0";
    }
    p->reply = p->window / 3 < ADVISOR_REPLY_MAX ? p->window / 3 : ADVISOR_REPLY_MAX;
    /* bytes for the whole prompt at a careful 3 bytes a token (it is another tokenizer than the
     * one the context estimate is calibrated on), and no more than ADVISOR_BRIEF_MAX however
     * large the window: what the advisor has to read first is the wait */
    int tokens = p->window - p->reply - 256;
    size_t room = tokens > 0 ? (size_t)tokens * 3 : 0;
    if (room < ADVISOR_BRIEF_MIN) room = ADVISOR_BRIEF_MIN;   /* a window too small to plan for (-c 512): a floor, and the server cuts what it must */
    p->budget = room < ADVISOR_BRIEF_MAX ? room : ADVISOR_BRIEF_MAX;
}

/* The one user message of an advisor call. What matters most stands last — the question, and
 * who the reader is: a server that has to cut an over-long prompt keeps the end of it. */
char *advisor_brief(cJSON *msgs, int keep, size_t budget, const char *env, bool plan_mode, const char *rules, const char *question) {
    sbuf b; sb_init(&b);
    sb_printf(&b, "# Environment\n%s\n", env ? env : "");
    if (plan_mode) sb_puts(&b, "The agent is in plan mode: it may only read, and has to present a plan instead of changing files.\n");
    if (rules && *rules) {
        size_t cap = budget / 4 < ADVISOR_RULES_MAX ? budget / 4 : ADVISOR_RULES_MAX;
        sb_puts(&b, "\n# The project's rules (the agent works under them; so must your advice)\n");
        sb_put_cut(&b, rules, cap);
        sb_putc(&b, '\n');
    }
    if (!question) question = "";
    while (*question == ' ' || *question == '\n') question++;
    size_t qlen = strlen(question), qcap = qlen > ADVISOR_QUESTION_MAX ? ADVISOR_QUESTION_MAX : qlen;
    while (qcap && qcap < qlen && ((unsigned char)question[qcap] & 0xC0) == 0x80) qcap--;
    size_t fixed = b.len + qcap + 600;   /* the headings and the closing lines */
    char *tr = transcript_text(msgs, keep, budget > fixed + 1024 ? budget - fixed : 1024);
    sb_printf(&b, "\n# The agent's conversation so far\n"
                  "[user] is the human, [agent] the agent (→ marks a tool call), [result of …] what a tool returned; long texts lose their middle.\n\n%s", tr);
    free(tr);
    sb_puts(&b, "# What the agent asks you\n");
    if (qcap) sb_printf(&b, "%.*s%s\n", (int)qcap, question, qcap < qlen ? "…" : "");
    else sb_puts(&b, "(It did not say. Review where it stands and tell it what to do next.)\n");
    sb_puts(&b, "\nAnswer the agent now — verdict first, then the steps, under about 400 words. "
                "You are the advisor, not the agent: do not continue the conversation above and do not call tools.\n");
    return sb_detach(&b);
}

/* A model whose thinking the server does not split off writes it into the answer as
 * <think>…</think>; the agent is to get the advice, not the way there. */
const char *strip_think_block(const char *s) {
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    if (strncmp(s, "<think>", 7)) return s;
    const char *e = strstr(s, "</think>");
    if (!e) return s + strlen(s);   /* it never finished thinking: there is no answer */
    e += 8;
    while (*e == ' ' || *e == '\n' || *e == '\r' || *e == '\t') e++;
    return e;
}
