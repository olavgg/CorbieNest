#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#ifdef WITH_ZLIB
#include <zlib.h>
#endif

config_t g_cfg;
/* The mode the config file keeps. It is what /mode saves and what the file says: a mode a session has for its run only
 * (--yolo, --mode, Shift+Tab, /yolo) is live in g_cfg.mode and never written, so a later save does not keep it. */
static int g_mode_saved = MODE_MANUAL;
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

/* ---------- finding the file a half-typed name means (@file, /cd) ----------
 * A name is rarely typed from the directory it lives in, so what is typed is taken as a hint
 * rather than as a path: "@term" finds src/term.c. The tree below the directory typed so far
 * (the working directory when there is none: "@../lib/pa" looks below ../lib) is read once,
 * breadth first — what is near comes first, and the bound on how much is read cuts off what is
 * deep — and kept for a moment, since the next letter asks about the same tree. */
/* The same words expand_mentions() (main.c) attaches, short of its search for a bare name: this
 * runs with every redraw of the input field, so it costs a stat per mention and nothing more. */
int mention_spans(const char *buf, size_t len, size_t (*spans)[2], int max) {
    int n = 0;
    for (size_t i = 0; i < len && n < max; i++) {
        if (buf[i] != '@' || (i > 0 && !isspace((unsigned char)buf[i - 1]))) continue;
        size_t s = i + 1, e = s;
        while (e < len && !isspace((unsigned char)buf[e])) e++;
        while (e > s && strchr(",.;:)!?", buf[e - 1])) e--;
        if (e == s) continue;
        char *path = xstrndup(buf + s, e - s), *fp = expand_home(path);
        if (is_file(fp) || is_dir(fp)) { spans[n][0] = i; spans[n][1] = e; n++; }
        free(path); free(fp);
        i = e - 1;
    }
    return n;
}

long mention_start(const char *buf, size_t cur) {
    size_t i = cur;
    while (i > 0 && !isspace((unsigned char)buf[i - 1])) i--;   /* start of the word the cursor is in */
    return i < cur && buf[i] == '@' ? (long)i + 1 : -1;
}

#define WALK_MAX   20000   /* entries read: what one keystroke may cost */
#define WALK_DEPTH 12
#define WALK_KEEP  2       /* seconds the listing is good for */
typedef struct { char *rel; bool dir, link; int depth; } walk_ent;
static walk_ent *g_walk = NULL;
static int g_walk_n = 0, g_walk_cap = 0;
static char *g_walk_root = NULL;
static time_t g_walk_at = 0;

static bool walk_skip(const char *name) {   /* (the directories the grep tool leaves out too) */
    static const char *SKIP[] = { ".", "..", ".git", "node_modules", ".venv", "__pycache__", ".hg", ".svn" };
    for (size_t i = 0; i < sizeof SKIP / sizeof *SKIP; i++) if (!strcmp(name, SKIP[i])) return true;
    return name[strcspn(name, " \t\r\n")] != 0;   /* a mention ends at whitespace: it could not be written */
}
static int walk_cmp(const void *a, const void *b) { return strcmp(((const walk_ent *)a)->rel, ((const walk_ent *)b)->rel); }

static void walk_tree(const char *root) {
    time_t now = time(NULL);
    char *abs = realpath(root, NULL);   /* by its real name: "." is another place after a /cd */
    if (abs && g_walk_root && !strcmp(g_walk_root, abs) && now - g_walk_at < WALK_KEEP && now >= g_walk_at) { free(abs); return; }
    for (int i = 0; i < g_walk_n; i++) free(g_walk[i].rel);
    g_walk_n = 0;
    free(g_walk_root); g_walk_root = abs; g_walk_at = now;
    if (!abs) return;   /* no such directory */
    /* the list is its own queue: entry i is read after everything that was found before it */
    for (int i = -1; i < g_walk_n && g_walk_n < WALK_MAX; i++) {
        if (i >= 0 && (!g_walk[i].dir || g_walk[i].link || g_walk[i].depth >= WALK_DEPTH)) continue;
        const char *rel = i < 0 ? "" : g_walk[i].rel;   /* (its own allocation: growing the list does not move it) */
        int depth = i < 0 ? 0 : g_walk[i].depth + 1, from = g_walk_n;
        sbuf dp; sb_init(&dp); sb_printf(&dp, "%s%s%s", abs, *rel ? "/" : "", rel);
        DIR *d = opendir(dp.data);
        struct dirent *de;
        while (d && (de = readdir(d)) && g_walk_n < WALK_MAX) {
            if (walk_skip(de->d_name)) continue;
            bool dir = de->d_type == DT_DIR, link = false;
            if (de->d_type != DT_DIR && de->d_type != DT_REG) {   /* a link, or a filesystem that does not say */
                sbuf fp; sb_init(&fp); sb_printf(&fp, "%s/%s", dp.data, de->d_name);
                struct stat st;
                bool ok = lstat(fp.data, &st) == 0;
                if (ok && S_ISLNK(st.st_mode)) { link = true; ok = stat(fp.data, &st) == 0; }   /* listed, never followed: it may point back up */
                sb_free(&fp);
                if (!ok || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
                dir = S_ISDIR(st.st_mode);
            }
            if (g_walk_n == g_walk_cap) { g_walk_cap = g_walk_cap ? g_walk_cap * 2 : 512; g_walk = xrealloc(g_walk, sizeof *g_walk * (size_t)g_walk_cap); }
            sbuf r; sb_init(&r); sb_printf(&r, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
            g_walk[g_walk_n++] = (walk_ent){ sb_detach(&r), dir, link, depth };
        }
        if (d) closedir(d);
        sb_free(&dp);
        qsort(g_walk + from, (size_t)(g_walk_n - from), sizeof *g_walk, walk_cmp);   /* readdir's order is nobody's */
    }
}

static bool subseq(const char *hay, const char *needle) {   /* the letters of needle, in order, somewhere in hay */
    for (; *needle; needle++, hay++) {
        while (*hay && tolower((unsigned char)*hay) != tolower((unsigned char)*needle)) hay++;
        if (!*hay) return false;
    }
    return true;
}

void file_matches_free(file_match *m, int n) {
    for (int i = 0; i < n; i++) free(m[i].path);
    free(m);
}

int file_complete(const char *typed, bool dirs_only, int max, file_match **out) {
    const char *slash = strrchr(typed, '/'), *needle = slash ? slash + 1 : typed;
    char *prefix = xstrndup(typed, (size_t)(needle - typed));   /* as typed ("../lib/", "~/"): it goes back in front of what is found */
    char *root = *prefix ? expand_home(prefix) : xstrdup(".");
    size_t nl = strlen(needle);
    file_match *m = xmalloc(sizeof *m * (size_t)(max > 0 ? max : 1));
    int n = 0;
    /* What was typed is a directory as it stands ("..", "src", "~"): going into it comes first. */
    if (nl && max > 0) {
        char *self = expand_home(typed);
        if (is_dir(self)) { sbuf b; sb_init(&b); sb_printf(&b, "%s/", typed); m[n++] = (file_match){ sb_detach(&b), true }; }
        free(self);
    }
    walk_tree(root);
    /* Best first: a name in the directory itself that starts with what was typed (plain path
     * completion), a file name anywhere below that starts with it, one that contains it, and
     * last one that merely has its letters in that order. Nothing typed yet lists the directory. */
    for (int tier = 0; tier < 4 && n < max; tier++)
        for (int i = 0; i < g_walk_n && n < max; i++) {
            const walk_ent *e = &g_walk[i];
            if (dirs_only && !e->dir) continue;
            const char *base = strrchr(e->rel, '/'); base = base ? base + 1 : e->rel;
            int t = -1;
            if (!nl) { if (e->depth == 0 && base[0] != '.') t = 0; }
            else if (!strncasecmp(base, needle, nl)) t = e->depth == 0 ? 0 : 1;
            else if (strcasestr(base, needle)) t = 2;
            else if (!e->dir && nl >= 2 && subseq(base, needle)) t = 3;
            if (t != tier) continue;
            sbuf b; sb_init(&b); sb_printf(&b, "%s%s%s", prefix, e->rel, e->dir ? "/" : "");
            if (n && !strcmp(m[0].path, b.data)) { sb_free(&b); continue; }   /* the directory offered above */
            m[n++] = (file_match){ sb_detach(&b), e->dir };
        }
    free(prefix); free(root);
    *out = m;
    return n;
}

char *file_find_named(const char *name, int *count, sbuf *names) {
    *count = 0;
    while (!strncmp(name, "./", 2)) name += 2;
    if (!*name || name[0] == '/' || name[0] == '~' || !strncmp(name, "../", 3)) return NULL;   /* a place was named, and it is not there */
    size_t nl = strlen(name);
    const char *found = NULL;
    walk_tree(".");
    for (int i = 0; i < g_walk_n; i++) {
        const char *rel = g_walk[i].rel; size_t rl = strlen(rel);
        if (g_walk[i].dir || rl < nl || strcmp(rel + rl - nl, name) || (rl > nl && rel[rl - nl - 1] != '/')) continue;
        if (!found) found = rel;
        if (names && *count < 5) sb_printf(names, "%s%s", *count ? ", " : "", rel);
        ++*count;
    }
    return *count == 1 ? xstrdup(found) : NULL;
}

/* ---------- what a word of a "!line" could become ----------
 * A shell line has two kinds of word: where a command goes — the start of the line, or after
 * | ; & ( — it is one of the programs in $PATH, anywhere else it may be a path. Inside quotes it
 * is text, and nothing is offered for that. */
#define SHELL_BREAKS "|;&(<>="   /* besides whitespace: what a word ends at */
long shell_word(const char *buf, size_t cur, bool *command) {
    if (cur < 1) return -1;
    char q = 0;
    for (size_t i = 1; i < cur; i++) {
        char c = buf[i];
        if (c == '\\' && q != '\'') i++;   /* the character after it is just that character */
        else if (q) { if (c == q) q = 0; }
        else if (c == '\'' || c == '"') q = c;
    }
    if (q) return -1;
    size_t ws = cur;
    while (ws > 1) {
        char c = buf[ws - 1];
        if (!(ws > 2 && buf[ws - 2] == '\\') && (isspace((unsigned char)c) || strchr(SHELL_BREAKS, c))) break;
        ws--;
    }
    size_t p = ws;
    while (p > 1 && (buf[p - 1] == ' ' || buf[p - 1] == '\t')) p--;
    *command = p == 1 || strchr("|;&(\n", buf[p - 1]) != NULL;
    return (long)ws;
}

char *shell_escape(const char *s) {
    sbuf b; sb_init(&b); sb_append(&b, "", 0);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && c < 0x80 && !strchr("_-./~+:@%,", c)) sb_putc(&b, '\\');
        sb_putc(&b, (char)c);
    }
    return sb_detach(&b);
}

static int cmd_cmp(const void *a, const void *b) {   /* the short names first: "git" is meant more often than "gi-compile-repository" */
    const char *x = ((const file_match *)a)->path, *y = ((const file_match *)b)->path;
    size_t xl = strlen(x), yl = strlen(y);
    return xl != yl ? (xl < yl ? -1 : 1) : strcmp(x, y);
}

int command_complete(const char *typed, int max, file_match **out) {
    size_t tl = strlen(typed);
    file_match *m = NULL;
    int n = 0, cap = 0, k = 0;
    const char *path = getenv("PATH");
    for (const char *p = path ? path : ""; *p; p += strspn(p, ":")) {
        size_t dl = strcspn(p, ":");
        char *dir = xstrndup(p, dl);
        p += dl;
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.' || strncmp(de->d_name, typed, tl)) continue;
            sbuf fp; sb_init(&fp); sb_printf(&fp, "%s/%s", dir, de->d_name);
            struct stat st;
            bool ok = stat(fp.data, &st) == 0 && S_ISREG(st.st_mode) && access(fp.data, X_OK) == 0;
            sb_free(&fp);
            if (!ok) continue;
            if (n == cap) { cap = cap ? cap * 2 : 64; m = xrealloc(m, sizeof *m * (size_t)cap); }
            m[n++] = (file_match){ xstrdup(de->d_name), false };
        }
        if (d) closedir(d);
        free(dir);
    }
    if (n) qsort(m, (size_t)n, sizeof *m, cmd_cmp);
    for (int i = 0; i < n; i++) {   /* each name once (it is in /bin and in /usr/bin), and no more than asked for */
        if (k < max && (!k || strcmp(m[k - 1].path, m[i].path))) m[k++] = m[i];
        else free(m[i].path);
    }
    *out = m;
    return k;
}

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
        char *eq = !strncmp(line, "effort.", 7) || !strncmp(line, "price.", 6) ? strrchr(line, '=') : strchr(line, '=');
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
        else if (!strncmp(k, "gzip.", 5)) { if (k[5] && (!strcmp(v, "0") || !strcmp(v, "1"))) gzip_set(k + 5, !strcmp(v, "1")); }
        else if (!strncmp(k, "price.", 6)) { double pi, po; if (k[6] && sscanf(v, "%lf/%lf", &pi, &po) == 2 && pi >= 0 && po >= 0) price_set(k + 6, pi, po); }
        else if (!strncmp(k, "host.", 5)) {   /* host.NAME=URL|KEY_ENV (/model add); a host that no longer passes the rules is said and skipped */
            char buf[1024]; snprintf(buf, sizeof buf, "%s", v);
            char *bar = strchr(buf, '|'); if (bar) *bar++ = 0;
            char err[160] = "";
            if (provider_host_add(k + 5, buf, bar, err, sizeof err) != 0) fprintf(stderr, "corbienest: host %s in the config is not used: %s\n", k + 5, err);
        }
        else if (!strcmp(k, "advisor")) { free(g_cfg.advisor); g_cfg.advisor = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "advisor_ctx")) { int n = atoi(v); if (n == 0 || n >= ADVISOR_CTX_MIN) g_cfg.advisor_ctx = n; }
        else if (!strcmp(k, "subagent")) { free(g_cfg.subagent); g_cfg.subagent = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "workers")) { int n = atoi(v); if (n >= 1 && n <= ORCH_PARALLEL_MAX) g_cfg.workers = n; }
        else if (!strcmp(k, "parallel")) g_cfg.parallel_tasks = !strcmp(v, "on") || atoi(v) != 0;   /* parallel=on, as /effort parallel saves it */
        else if (!strcmp(k, "build")) { free(g_cfg.build); g_cfg.build = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "advisor_window")) { double m = atof(v); if (m >= 0 && m <= 24 * 60) g_cfg.advisor_window = (int)(m * 60 + 0.5); }
        else if (!strcmp(k, "advisor_guidance")) { int n = advisor_guidance_parse(v); if (n >= 0) g_cfg.advisor_guidance = n; }
        else if (!strcmp(k, "show_thinking")) g_cfg.show_thinking = atoi(v) != 0;
        else if (!strcmp(k, "ui")) { free(g_cfg.ui); g_cfg.ui = !strcmp(v, "notcurses") ? xstrdup("notcurses") : NULL; }
        else if (!strcmp(k, "theme")) {   /* a name no theme has is said, and the default is used */
            if (theme_set(v)) { free(g_cfg.theme); g_cfg.theme = xstrdup(v); }
            else fprintf(stderr, "corbienest: no theme named \"%s\" in the config: the default is used\n", v);
        }
        else if (!strcmp(k, "yolo")) { if (atoi(v)) { g_cfg.mode = MODE_AUTO; g_mode_saved = MODE_AUTO; } }
        else if (!strcmp(k, "mode")) { int m = mode_parse(v); if (m >= 0) { g_cfg.mode = m; g_mode_saved = m; } }
        else if (!strcmp(k, "memory")) g_cfg.memory = atoi(v) != 0;
        else if (!strcmp(k, "web")) g_cfg.web = atoi(v) != 0;
        else if (!strcmp(k, "sandbox")) g_cfg.sandbox = !strcmp(v, "bwrap") || atoi(v) != 0;
        else if (!strcmp(k, "search_url")) { free(g_cfg.search_url); g_cfg.search_url = *v ? xstrdup(v) : NULL; }
        else if (!strcmp(k, "max_iters")) { int n = atoi(v); if (n > 0) g_cfg.max_iters = n; }
        else if (!strcmp(k, "memory_every")) { int n = atoi(v); if (n > 0) g_cfg.memory_every = n; }
        else if (!strcmp(k, "memory_idle")) { int n = atoi(v); if (n >= 0) g_cfg.memory_idle = n; }
        else if (!strcmp(k, "suggest")) g_cfg.suggest = atoi(v) != 0;
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
void config_set_mode(int mode) { g_cfg.mode = mode; g_mode_saved = mode; }

void config_save(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/config", config_dir());
    sbuf b; sb_init(&b);
    sb_puts(&b, "# corbienest config (auto-written)\n");
    if (g_cfg.model && *g_cfg.model) sb_printf(&b, "model=%s\n", g_cfg.model);
    if (g_cfg.host) sb_printf(&b, "host=%s\n", g_cfg.host);
    sb_printf(&b, "num_ctx=%d\n", g_cfg.num_ctx);
    if (g_cfg.temperature >= 0) sb_printf(&b, "temperature=%g\n", g_cfg.temperature);
    sb_printf(&b, "think=%d\n", g_cfg.think);
    for (int i = 0; i < g_cfg.n_efforts; i++) sb_printf(&b, "effort.%s=%s\n", g_cfg.efforts[i].model, g_cfg.efforts[i].level);
    for (int i = 0; i < g_cfg.n_prices; i++) sb_printf(&b, "price.%s=%g/%g\n", g_cfg.prices[i].model, g_cfg.prices[i].in, g_cfg.prices[i].out);
    for (int i = 0; i < g_cfg.n_gzips; i++) sb_printf(&b, "gzip.%s=%d\n", g_cfg.gzips[i].name, g_cfg.gzips[i].on ? 1 : 0);
    for (int i = 0; provider_at(i); i++) if (provider_at(i)->host) sb_printf(&b, "host.%s=%s|%s\n", provider_at(i)->name, provider_at(i)->url, provider_at(i)->key_env);
    if (g_cfg.advisor) sb_printf(&b, "advisor=%s\n", g_cfg.advisor);
    if (g_cfg.advisor_ctx > 0) sb_printf(&b, "advisor_ctx=%d\n", g_cfg.advisor_ctx);
    if (g_cfg.subagent && *g_cfg.subagent) sb_printf(&b, "subagent=%s\n", g_cfg.subagent);
    if (g_cfg.workers > 0) sb_printf(&b, "workers=%d\n", g_cfg.workers);
    if (g_cfg.parallel_tasks) sb_puts(&b, "parallel=on\n");
    if (g_cfg.build && *g_cfg.build) sb_printf(&b, "build=%s\n", g_cfg.build);
    if (g_cfg.advisor_window != ADVISOR_WINDOW_DEFAULT) sb_printf(&b, "advisor_window=%g\n", g_cfg.advisor_window / 60.0);   /* in minutes, as it is typed */
    if (g_cfg.advisor_guidance != GUIDANCE_NORMAL) sb_printf(&b, "advisor_guidance=%s\n", advisor_guidance()->name);
    sb_printf(&b, "show_thinking=%d\n", g_cfg.show_thinking ? 1 : 0);
    sb_printf(&b, "mode=%s\n", mode_name(g_mode_saved));
    sb_printf(&b, "max_iters=%d\n", g_cfg.max_iters);
    sb_printf(&b, "memory=%d\n", g_cfg.memory ? 1 : 0);
    sb_printf(&b, "web=%d\n", g_cfg.web ? 1 : 0);
    sb_printf(&b, "sandbox=%s\n", g_cfg.sandbox ? "bwrap" : "off");
    if (g_cfg.search_url) sb_printf(&b, "search_url=%s\n", g_cfg.search_url);
    sb_printf(&b, "memory_every=%d\n", g_cfg.memory_every);
    sb_printf(&b, "memory_idle=%d\n", g_cfg.memory_idle);
    sb_printf(&b, "suggest=%d\n", g_cfg.suggest ? 1 : 0);
    sb_printf(&b, "keep_alive=%s\n", g_cfg.keep_alive ? g_cfg.keep_alive : "");
    if (g_cfg.ui) sb_printf(&b, "ui=%s\n", g_cfg.ui);
    if (g_cfg.theme) sb_printf(&b, "theme=%s\n", g_cfg.theme);
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

/* ---------- /orchestrate: reading the plan ---------- */
/* "TASK 3: title" at the start of a line, with whatever markdown a model puts around it ("## Task 3 —",
 * "**TASK 3.** title"): returns the title's start, NULL for any other line. A sentence of the
 * instructions that begins "Task 3 depends on …" is not one: the number must end in a separator. */
static const char *plan_header(const char *line, size_t len, size_t *title_len) {
    const char *p = line, *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '#' || *p == '*' || *p == '_' || *p == '-' || *p == '>')) p++;
    if (end - p < 5 || strncasecmp(p, "task", 4)) return NULL;
    p += 4;
    while (p < end && *p == ' ') p++;
    if (p == end || !isdigit((unsigned char)*p)) return NULL;
    while (p < end && isdigit((unsigned char)*p)) p++;
    while (p < end && (*p == ' ' || *p == '*' || *p == '_')) p++;
    if (p < end) {
        if (*p == ':' || *p == '.' || *p == ')' || *p == '-') p++;
        else if (!strncmp(p, "—", 3) || !strncmp(p, "–", 3)) p += 3;
        else return NULL;
    }
    while (p < end && (*p == ' ' || *p == '*' || *p == '_')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '*' || end[-1] == '_' || end[-1] == '\r' || end[-1] == ':')) end--;
    *title_len = (size_t)(end - p);
    return p;
}

/* "AFTER: 1, 3" / "AFTER: none" among a task's instructions: which tasks it waits for, by the
 * numbers the plan gave them. Returns false for any other line; *none says the list was empty. */
static bool plan_after(const char *line, size_t len, int *nums, int *n, bool *none) {
    const char *p = line, *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '*' || *p == '_' || *p == '-' || *p == '>')) p++;
    if (end - p < 5 || strncasecmp(p, "after", 5)) return false;
    p += 5;
    while (p < end && (*p == ' ' || *p == '*' || *p == '_')) p++;
    if (p == end || *p != ':') return false;
    *n = 0;
    for (p++; p < end; ) {
        if (isdigit((unsigned char)*p)) { int v = 0; while (p < end && isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0'); if (*n < ORCH_MAX_TASKS) nums[(*n)++] = v; }
        else p++;
    }
    *none = *n == 0;
    return true;
}

/* "KEY: value" among a plan's lines (a task's, or the plan's): the value's span. KEY is a whole word, matched
 * without case and past the markers a model puts in front of it (bullets, bold); false for any other line. */
static bool plan_field(const char *line, size_t len, const char *key, const char **val, size_t *vlen) {
    const char *p = line, *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '*' || *p == '_' || *p == '-' || *p == '>' || *p == '#')) p++;
    size_t kl = strlen(key);
    if ((size_t)(end - p) < kl || strncasecmp(p, key, kl)) return false;
    p += kl;
    while (p < end && (*p == ' ' || *p == '*' || *p == '_')) p++;
    if (p == end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '`' || *p == '*' || *p == '_')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '`' || end[-1] == '*' || end[-1] == '_')) end--;
    *val = p; *vlen = (size_t)(end - p);
    return true;
}

/* "OWNS: a, b": the paths a task may write, appended to its list (a literal file, or a directory prefix) */
static void plan_owns(orch_task *t, const char *v, size_t vl) {
    for (size_t i = 0; i < vl; ) {
        size_t j = i; while (j < vl && v[j] != ',') j++;
        size_t a = i, b = j;
        while (a < b && (v[a] == ' ' || v[a] == '\t')) a++;
        while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t')) b--;
        if (b > a && t->nowns < ORCH_OWNS_MAX) {
            t->owns = xrealloc(t->owns, sizeof *t->owns * (size_t)(t->nowns + 1));
            t->owns[t->nowns++] = xstrndup(v + a, b - a);
        }
        i = j + 1;
    }
}

/* An OWNS: path without its "./" in front and its "/" behind: "" is the whole project */
static char *owns_norm(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    size_t l = strlen(p);
    while (l && p[l - 1] == '/') l--;
    if (l == 1 && p[0] == '.') l = 0;
    return xstrndup(p, l);
}

/* Two OWNS: paths overlap when they are the same file, or one is a directory (a prefix up to a '/') holding the other.
 * The whole project ("." or "./") overlaps everything. */
bool owns_overlap_one(const char *a, const char *b) {
    char *x = owns_norm(a), *y = owns_norm(b);
    bool hit = false;
    if (!x[0] || !y[0]) hit = true;
    else {
        size_t lx = strlen(x), ly = strlen(y);
        if (lx > ly) { char *t = x; x = y; y = t; size_t tl = lx; lx = ly; ly = tl; }   /* x is the shorter */
        hit = !strcmp(x, y) || (!strncmp(x, y, lx) && y[lx] == '/');
    }
    free(x); free(y);
    return hit;
}

bool owns_overlap(char **a, int na, char **b, int nb) {
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (owns_overlap_one(a[i], b[j])) return true;
    return false;
}

int orch_parse_plan(const char *text, orch_task **tasks) {
    return orch_parse_plan_ex(text, tasks, NULL);
}

int orch_parse_plan_ex(const char *text, orch_task **tasks, char **contract) {
    *tasks = NULL;
    if (contract) *contract = NULL;
    int n = 0; bool in_contract = false;   /* in_contract: the CONTRACT: block before the first task */
    int num[ORCH_MAX_TASKS + 1] = { 0 };                 /* the number the plan gave each task */
    int dep[ORCH_MAX_TASKS + 1][ORCH_MAX_TASKS], ndep[ORCH_MAX_TASKS + 1]; bool said[ORCH_MAX_TASKS + 1];
    memset(ndep, 0, sizeof ndep); memset(said, 0, sizeof said);
    sbuf body, csb; sb_init(&body); sb_init(&csb);
    for (const char *p = text ? text : ""; *p; ) {
        size_t len = strcspn(p, "\n"), tl = 0;
        const char *title = plan_header(p, len, &tl);
        bool none; const char *v = NULL; size_t vl = 0;
        if (title) {
            in_contract = false;
            if (n) { (*tasks)[n - 1].body = xstrdup(body.data ? body.data : ""); trim((*tasks)[n - 1].body); }
            body.len = 0; if (body.data) body.data[0] = 0;
            if (n == ORCH_MAX_TASKS) { n++; break; }   /* (counted to say so, never stored) */
            *tasks = xrealloc(*tasks, sizeof **tasks * (size_t)(n + 1));
            const char *d = p; while (d < title && !isdigit((unsigned char)*d)) d++;
            num[n] = atoi(d);
            (*tasks)[n++] = (orch_task){ .title = xstrndup(title, tl) };
        } else if (n && plan_after(p, len, dep[n - 1], &ndep[n - 1], &none)) said[n - 1] = true;   /* (not part of the instructions) */
        else if (n && plan_field(p, len, "role", &v, &vl)) {
            free((*tasks)[n - 1].role);
            (*tasks)[n - 1].role = vl ? xstrndup(v, vl) : NULL;
        } else if (n && plan_field(p, len, "owns", &v, &vl)) plan_owns(&(*tasks)[n - 1], v, vl);
        else if (n && plan_field(p, len, "layer", &v, &vl)) {
            int lv = 0;
            for (size_t k = 0; k < vl && k < 4 && isdigit((unsigned char)v[k]); k++) lv = lv * 10 + (v[k] - '0');
            if (lv > 0) (*tasks)[n - 1].layer = lv;
        }
        else if (!n && plan_field(p, len, "contract", &v, &vl)) {
            in_contract = true;
            if (vl) { sb_append(&csb, v, vl); sb_putc(&csb, '\n'); }
        } else if (!n && in_contract) { sb_append(&csb, p, len); sb_putc(&csb, '\n'); }
        else if (n) { sb_append(&body, p, len); sb_putc(&body, '\n'); }
        p += len; if (*p == '\n') p++;
    }
    if (n > ORCH_MAX_TASKS) n = ORCH_MAX_TASKS;
    else if (n) { (*tasks)[n - 1].body = xstrdup(body.data ? body.data : ""); trim((*tasks)[n - 1].body); }
    sb_free(&body);
    if (contract && csb.len) {
        *contract = xstrdup(csb.data);
        trim(*contract);
        if (!(*contract)[0]) { free(*contract); *contract = NULL; }
    }
    sb_free(&csb);
    /* a header with nothing to it — no title and no instructions — is not a task */
    int keep = 0, at[ORCH_MAX_TASKS];   /* at[i]: where task i ended up, -1 = dropped */
    for (int i = 0; i < n; i++) {
        if (!(*tasks)[i].body) (*tasks)[i].body = xstrdup("");
        if (!(*tasks)[i].title[0] && !(*tasks)[i].body[0]) {
            free((*tasks)[i].title); free((*tasks)[i].body); free((*tasks)[i].role);
            for (int k = 0; k < (*tasks)[i].nowns; k++) free((*tasks)[i].owns[k]);
            free((*tasks)[i].owns);
            at[i] = -1; continue;
        }
        at[i] = keep; (*tasks)[keep++] = (*tasks)[i];
    }
    /* layers (/plan): a task with none is in the layer of the one before it, the first in layer 1; a layer never goes back */
    int lay = 1;
    for (int k = 0; k < keep; k++) { if ((*tasks)[k].layer > lay) lay = (*tasks)[k].layer; (*tasks)[k].layer = lay; }
    /* what each waits for: the tasks its AFTER line names — earlier ones only, so nothing can wait
     * in a circle — or, having none, the task before it: a plan that says nothing runs in order */
    for (int i = 0; i < n; i++) {
        if (at[i] < 0) continue;
        unsigned after = 0;
        if (!said[i]) { for (int j = i - 1; j >= 0; j--) if (at[j] >= 0) { after = 1u << at[j]; break; } }
        else for (int k = 0; k < ndep[i]; k++) for (int j = 0; j < i; j++) if (at[j] >= 0 && num[j] == dep[i][k]) after |= 1u << at[j];
        (*tasks)[at[i]].after = after;
    }
    if (!keep) { free(*tasks); *tasks = NULL; }
    return keep;
}

void orch_tasks_free(orch_task *t, int n) {
    for (int i = 0; i < n; i++) {
        free(t[i].title); free(t[i].body); free(t[i].role);
        for (int k = 0; k < t[i].nowns; k++) free(t[i].owns[k]);
        free(t[i].owns);
    }
    free(t);
}

/* ---------- prices ----------
 * The providers' list prices, USD per million input / output tokens, as their pricing pages had
 * them in October 2026. Prices move and models come and go: /usage says which price it used, and
 * /usage price sets another. The higher rates some charge for a very long prompt (xAI from 200k
 * tokens, OpenAI from 272k, Haiku 5.5 from 100k) are left out: the one hosted call corbienest
 * makes is a consultation, whose brief is capped far below that. */
static const struct { const char *name; double in, out, cache; } LIST_PRICES[] = {
    /* cache: what a cached input token costs, × the input price (the providers' cached-input column; the
     * newest Anthropic models read at 5% or 2.5%). A model with no cached price is 1: no discount assumed. */
    { "anthropic:claude-fable-5", 10, 50, 0.10 },     { "anthropic:claude-fable-5-1", 10, 50, 0.025 },
    { "anthropic:claude-mythos-5", 10, 50, 0.10 },    { "anthropic:claude-mythos-5-1", 10, 50, 0.025 },
    { "anthropic:claude-opus-5-5", 4, 20, 0.05 },     { "anthropic:claude-opus-5", 5, 25, 0.10 },
    { "anthropic:claude-opus-4", 5, 25, 0.10 },       { "anthropic:claude-opus-4-1", 15, 75, 0.10 },
    { "anthropic:claude-sonnet-5-5", 2, 10, 0.05 },   { "anthropic:claude-sonnet-5", 2, 10, 0.10 },
    { "anthropic:claude-sonnet-4", 3, 15, 0.10 },
    { "anthropic:claude-haiku-5", 0.10, 0.50, 0.10 }, { "anthropic:claude-haiku-4-5", 1, 5, 0.10 },
    { "openai:gpt-6-astra", 10, 50, 0.10 },   { "openai:gpt-6.1-sol", 2, 10, 0.05 },   { "openai:gpt-6-sol", 2, 10, 0.10 },   { "openai:gpt-6-luna", 0.10, 0.50, 0.10 },
    { "openai:gpt-5.6-sol", 4, 20, 0.10 },    { "openai:gpt-5.6-terra", 2, 12, 0.10 }, { "openai:gpt-5.6-luna", 0.20, 1.20, 0.10 },
    { "openai:gpt-5.5", 5, 30, 0.10 },        { "openai:gpt-5.5-pro", 30, 180, 1 },
    { "openai:gpt-5.4", 2.50, 15, 0.10 },     { "openai:gpt-5.4-mini", 0.75, 4.50, 0.10 }, { "openai:gpt-5.4-nano", 0.20, 1.25, 0.10 }, { "openai:gpt-5.4-pro", 30, 180, 1 },
    { "openai:gpt-5.2", 1.75, 14, 0.10 },     { "openai:gpt-5.2-pro", 21, 168, 1 },  { "openai:gpt-5.1", 1.25, 10, 0.10 },
    { "openai:gpt-5", 1.25, 10, 0.10 },       { "openai:gpt-5-mini", 0.25, 2, 0.10 },   { "openai:gpt-5-nano", 0.05, 0.40, 0.10 }, { "openai:gpt-5-pro", 15, 120, 1 },
    { "openai:gpt-4.1", 2, 8, 0.25 },         { "openai:gpt-4.1-mini", 0.40, 1.60, 0.25 }, { "openai:gpt-4.1-nano", 0.10, 0.40, 0.25 },
    { "openai:gpt-4o", 2.50, 10, 0.50 },      { "openai:gpt-4o-mini", 0.15, 0.60, 0.50 },
    { "openai:o3", 2, 8, 0.25 },              { "openai:o3-pro", 20, 80, 1 },        { "openai:o3-mini", 1.10, 4.40, 0.50 },   { "openai:o4-mini", 1.10, 4.40, 0.25 },
    { "xai:grok-4.7", 2, 6, 0.25 },           { "xai:grok-4.6", 2, 6, 0.25 },           { "xai:grok-4.5", 2, 6, 0.15 },
    { "xai:grok-4.3", 1.25, 2.50, 0.16 },     { "xai:grok-4.20", 1.25, 2.50, 0.16 },    { "xai:grok-4.20-multi-agent", 1.25, 2.50, 0.16 }, { "xai:grok-build-0.1", 1, 2, 0.20 },
};

/* a hosted model under its provider's own name ("claude:x" is "anthropic:x"); any other as it is. malloc'd */
static char *price_key(const char *model) {
    const char *m; const provider_def *p = provider_find(model, &m);
    if (!p) return xstrdup(model);
    sbuf b; sb_init(&b); sb_printf(&b, "%s:%s", p->name, m);
    return sb_detach(&b);
}

/* the built-in price for a key ("provider:model"): the longest name it is, or a dated snapshot of, -1 if none.
 * "gpt-5" says nothing about "gpt-5.9" or "gpt-5-mini" */
static int list_match(const char *key) {
    int best = -1; size_t bl = 0;
    for (size_t i = 0; i < sizeof LIST_PRICES / sizeof *LIST_PRICES; i++) {
        size_t l = strlen(LIST_PRICES[i].name);
        if (l <= bl || strncmp(key, LIST_PRICES[i].name, l)) continue;
        if (key[l] && !(key[l] == '-' && isdigit((unsigned char)key[l + 1]))) continue;
        best = (int)i; bl = l;
    }
    return best;
}

int model_price(const char *model, double *in, double *out) {
    if (!model || !*model) return 0;
    char *key = price_key(model);
    int found = 0;
    for (int i = 0; !found && i < g_cfg.n_prices; i++)
        if (!strcmp(g_cfg.prices[i].model, key)) { *in = g_cfg.prices[i].in; *out = g_cfg.prices[i].out; found = 2; }
    if (!found) { int m = list_match(key); if (m >= 0) { *in = LIST_PRICES[m].in; *out = LIST_PRICES[m].out; found = 1; } }
    free(key);
    return found;
}

/* Prompt caching, as Anthropic bills it (its prompt-caching and models pages): a read costs a tenth of
 * the base input price — 2.5% on Fable 5.1 and Mythos 5.1, 5% on Opus 5.5 and Sonnet 5.5 — and a write to
 * the 5-minute cache 1.25 times it. OpenAI and xAI bill cached input at the rate in LIST_PRICES (their
 * usage reports it as a part of the prompt; they bill no separate write). */
static bool anthropic_model(const char *model, const char **name) {
    const provider_def *p = model ? provider_find(model, name) : NULL;
    return p && p->style == PROVIDER_MESSAGES;
}
double model_cache_write_ratio(const char *model) { return anthropic_model(model, NULL) ? 1.25 : 1; }
/* a cached input token costs this × the input price: the list's figure, else Anthropic's tenth and 1 for the rest */
double model_cache_read_ratio(const char *model) {
    if (!model || !*model) return 1;
    char *key = price_key(model);
    int m = list_match(key);
    double r = m >= 0 ? LIST_PRICES[m].cache : anthropic_model(model, NULL) ? 0.10 : 1;
    free(key);
    return r;
}
/* what a model's tokens cost in USD: the uncached input at the input price, the cache reads and writes
 * at their ratios, the output at the output price. known = model_price()'s answer (0 = no price: cost 0). */
double usage_cost_usd(const char *model, long in, long out, long cache_read, long cache_write, int *known) {
    double pi = 0, po = 0;
    *known = model_price(model, &pi, &po);
    if (!*known) return 0;
    long plain = in - cache_read - cache_write; if (plain < 0) plain = 0;
    return ((double)plain * pi + (double)cache_read * pi * model_cache_read_ratio(model)
            + (double)cache_write * pi * model_cache_write_ratio(model) + (double)out * po) / 1e6;
}

void price_set(const char *model, double in, double out) {
    if (!model || !*model || strchr(model, '\n')) return;
    char *key = price_key(model);
    int i = 0;
    while (i < g_cfg.n_prices && strcmp(g_cfg.prices[i].model, key)) i++;
    if (in < 0) {   /* forget it */
        if (i < g_cfg.n_prices) { free(g_cfg.prices[i].model); g_cfg.prices[i] = g_cfg.prices[--g_cfg.n_prices]; }
        free(key);
        return;
    }
    if (i == g_cfg.n_prices) {
        g_cfg.prices = xrealloc(g_cfg.prices, sizeof *g_cfg.prices * (size_t)(g_cfg.n_prices + 1));
        g_cfg.prices[g_cfg.n_prices++] = (price_entry){ key, in, out };
    } else { g_cfg.prices[i].in = in; g_cfg.prices[i].out = out; free(key); }
}

/* base64, RFC 4648 with '=' padding; the input may hold any bytes, NULs included */
char *base64_encode(const char *in, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc(((n + 2) / 3) * 4 + 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63]; out[o++] = T[(v >> 6) & 63]; out[o++] = T[v & 63];
    }
    if (n - i == 1) {
        unsigned v = (unsigned char)in[i] << 16;
        out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63]; out[o++] = '='; out[o++] = '=';
    } else if (n - i == 2) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out[o++] = T[v >> 18]; out[o++] = T[(v >> 12) & 63]; out[o++] = T[(v >> 6) & 63]; out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

/* ---------- gzip request bodies ----------
 * A body is gzipped only where the server has been seen to take it (the per-provider setting, probed
 * by /gzip probe); a refusal falls back to the plain body (provider.c). The compression itself needs
 * zlib, built in with WITH_ZLIB; without it gzip_available() is false and nothing is compressed. */
#ifdef WITH_ZLIB
bool gzip_available(void) { return true; }

char *gzip_body(const char *in, size_t n, size_t *out_n) {
    if (n > UINT_MAX / 2) return NULL;
    z_stream z; memset(&z, 0, sizeof z);
    /* windowBits 15 + 16: the gzip wrapper (RFC 1952), not zlib's own, which is what Content-Encoding: gzip means */
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t cap = deflateBound(&z, (uLong)n) + 64;
    unsigned char *out = malloc(cap);
    if (!out) { deflateEnd(&z); return NULL; }
    z.next_in = (Bytef *)in; z.avail_in = (uInt)n;
    z.next_out = out; z.avail_out = (uInt)cap;
    int rc = deflate(&z, Z_FINISH);
    size_t produced = cap - z.avail_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END) { free(out); return NULL; }
    *out_n = produced;
    return (char *)out;
}
#else
bool gzip_available(void) { return false; }
char *gzip_body(const char *in, size_t n, size_t *out_n) { (void)in; (void)n; (void)out_n; return NULL; }
#endif

/* A server that cannot read a gzipped body says so with 415, or with a 400 whose message names the
 * encoding or the JSON it could not read. Any other 400 (a bad parameter, a long context) is not
 * one: it would be answered the same way plain. */
bool gzip_refused(int status, const char *answer) {
    if (status == 415) return true;
    if (status != 400 || !answer) return false;
    static const char *WORDS[] = { "encod", "gzip", "decompress", "valid json", "not json", "parse", NULL };
    for (int i = 0; WORDS[i]; i++) if (strcasestr(answer, WORDS[i])) return true;
    return false;
}

int gzip_probe_verdict(int plain_status, int gz_status, const char *gz_answer) {
    if (plain_status < 200 || plain_status > 299) return -1;   /* without a working plain call there is nothing to compare with */
    if (gz_status >= 200 && gz_status <= 299) return 1;
    if (gzip_refused(gz_status, gz_answer)) return 0;
    return -1;
}

static gzip_entry *gzip_find(const char *name) {
    for (int i = 0; i < g_cfg.n_gzips; i++) if (!strcasecmp(g_cfg.gzips[i].name, name)) return &g_cfg.gzips[i];
    return NULL;
}

bool gzip_get(const char *name, bool *on) {
    gzip_entry *e = gzip_find(name);
    if (!e) return false;
    *on = e->on;
    return true;
}

void gzip_set(const char *name, bool on) {
    if (!name || !*name) return;
    gzip_entry *e = gzip_find(name);
    if (e) { e->on = on; return; }
    g_cfg.gzips = xrealloc(g_cfg.gzips, sizeof *g_cfg.gzips * (size_t)(g_cfg.n_gzips + 1));
    g_cfg.gzips[g_cfg.n_gzips++] = (gzip_entry){ xstrdup(name), on };
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

/* The host of a URL, or of a host setting as the user writes one — scheme, user@ and port all
 * optional ("gpu-box:11434", "http://[::1]:8080/v1") — lowercased, without the port. */
bool url_hostname(const char *url, char *out, size_t n) {
    if (!url || !n) return false;
    out[0] = 0;
    const char *p = strstr(url, "://"); p = p ? p + 3 : url;
    while (*p == ' ') p++;
    size_t len = strcspn(p, "/?#");
    for (size_t i = len; i > 0; i--) if (p[i - 1] == '@') { p += i; len -= i; break; }
    size_t h = p[0] == '[' ? (memchr(p, ']', len) ? (size_t)((const char *)memchr(p, ']', len) - p) + 1 : len) : strcspn(p, ":/?#");
    if (h > len) h = len;
    if (h >= n) h = n - 1;
    for (size_t i = 0; i < h; i++) out[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] - 'A' + 'a') : p[i];
    out[h] = 0;
    return out[0] != 0;
}

/* This machine: localhost, an IPv4 loopback address (a literal one — "127.evil.example" is a
 * name like any other), ::1, 0.0.0.0, or no host at all (":11434"). */
bool url_is_local(const char *url) {
    char h[256];
    if (!url_hostname(url, h, sizeof h)) return true;
    if (!strcmp(h, "localhost") || !strcmp(h, "[::1]") || !strcmp(h, "::1") || !strcmp(h, "0.0.0.0")) return true;
    return !strncmp(h, "127.", 4) && strspn(h, "0123456789.") == strlen(h);
}

/* Not encrypted on the way: anything but https — a host without a scheme is plain http */
bool url_cleartext(const char *url) {
    while (url && *url == ' ') url++;
    return !url || strncasecmp(url, "https://", 8) != 0;
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

/* ---------- what a request body must satisfy ----------
 * The server parses the body as strict JSON: one byte of invalid UTF-8 (a cut that split a
 * character, a binary file in a command's output) makes the whole request refused. cJSON does not
 * check, so the text is made valid where it enters the conversation. */
static size_t utf8_seq_len(const unsigned char *p, size_t left) {
    unsigned char c = p[0];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 && c >= 0xC2 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E && c <= 0xF4 ? 4 : 0;
    if (!n || n > left) return 0;
    if (n == 1) return 1;
    unsigned lo = 0x80, hi = 0xBF;   /* the second byte has its own bounds for E0, ED, F0, F4 */
    if (p[1] < lo || p[1] > hi) return 0;
    if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; else if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F;
    if (p[1] < lo || p[1] > hi) return 0;
    for (size_t i = 2; i < n; i++) if (p[i] < 0x80 || p[i] > 0xBF) return 0;
    return n;
}

/* true when buf ends inside an escape sequence or a UTF-8 character: what follows it is then not
 * the rest of that sequence, so nothing may be written between the two (the stdout hook splices
 * cursor moves between chunks, and a chunk can end anywhere in a large write). */
bool tail_incomplete(const char *buf, size_t n) {
    size_t from = n > 64 ? n - 64 : 0;
    const unsigned char *b = (const unsigned char *)buf;
    /* an escape sequence that starts in the tail and has not ended */
    size_t e = n;
    for (size_t k = n; k > from; k--) if (b[k - 1] == 0x1b) { e = k - 1; break; }
    if (e < n) {
        if (e + 1 == n) return true;                              /* a lone ESC */
        unsigned char c = b[e + 1];
        if (c == '[') {                                           /* CSI: parameters, then a final byte 0x40-0x7e */
            size_t j = e + 2;
            while (j < n && b[j] >= 0x20 && b[j] <= 0x3f) j++;
            if (j == n) return true;
        } else if (c == ']') {                                    /* OSC: ends at BEL or ESC \ */
            size_t j = e + 2; bool ended = false;
            for (; j < n; j++) {
                if (b[j] == 0x07) { ended = true; break; }
                if (b[j] == 0x1b && j + 1 < n && b[j + 1] == '\\') { ended = true; break; }
            }
            if (!ended) return true;
        } else if (c >= 0x20 && c <= 0x2f) {                     /* an intermediate byte: one final byte follows */
            if (e + 2 >= n) return true;
        }
    }
    /* a UTF-8 character whose continuation bytes have not all come */
    for (size_t k = n, seen = 0; k > from && seen < 4; k--, seen++) {
        unsigned char c = b[k - 1];
        if ((c & 0xC0) == 0x80) continue;                         /* a continuation byte: look further back */
        size_t want = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 1;
        return want > 1 && n - (k - 1) < want;
    }
    return false;
}

bool utf8_ok(const char *s, size_t len) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < len;) {
        size_t n = utf8_seq_len(p + i, len - i);
        if (!n) return false;
        i += n;
    }
    return true;
}

char *utf8_fix(const char *s) {
    size_t len = strlen(s);
    if (utf8_ok(s, len)) return xstrdup(s);
    sbuf b; sb_init(&b);
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < len;) {
        size_t n = utf8_seq_len(p + i, len - i);
        if (n) { sb_append(&b, s + i, n); i += n; }
        else { sb_puts(&b, "\xEF\xBF\xBD"); i++; }   /* U+FFFD for each bad byte */
    }
    return sb_detach(&b);
}

size_t utf8_cut(const char *s, size_t len, size_t cap) {
    if (cap >= len) return len;
    size_t n = cap;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   /* s[n] starts a character: the prefix ends on a boundary */
    return n;
}

/* ---------- the running-tasks strip: one row, and how many rows fit ---------- */

static int sr_cols(const char *s) {
    int w = 0;
    if (!s) return 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) w++;
    return w;
}
/* at most `cols` columns of s. A cut ends with …, which takes one of those columns. */
static void sr_put(sbuf *b, const char *s, int cols, bool ellip) {
    if (cols <= 0 || !s || !*s) return;
    if (sr_cols(s) <= cols) { sb_puts(b, s); return; }
    int keep = ellip && cols >= 1 ? cols - 1 : cols;
    int got = 0;
    for (const char *p = s; *p && got < keep; ) {
        const char *nx = p + 1;
        while (((unsigned char)*nx & 0xC0) == 0x80) nx++;
        sb_append(b, p, (size_t)(nx - p));
        got++;
        p = nx;
    }
    if (ellip && cols >= 1) sb_puts(b, "…");
}
static void sr_pad(sbuf *b, int have, int want) {
    for (int i = have; i < want; i++) sb_putc(b, ' ');
}
static void sr_finish(char *out, size_t n, sbuf *b, int cols) {
    const char *s = b && b->data ? b->data : "";
    sbuf c; sb_init(&c);
    sr_put(&c, s, cols, true);
    size_t len = c.len;
    if (len > n - 1) {
        len = n - 1;
        while (len > 0 && c.data && ((unsigned char)c.data[len] & 0xC0) == 0x80) len--;
    }
    if (c.data && len) memcpy(out, c.data, len);
    out[len] = 0;
    sb_free(&c);
    sb_free(b);
}

void strip_row(char *out, size_t n, int cols, int kind, const char *name, int name_w,
               const char *mid, int done, int total, const char *right) {
    if (out && n) out[0] = 0;
    if (!out || n < 2) return;
    if (cols < 1) cols = 1;
    if (kind == STRIP_ROW_ALL) {
        sbuf b; sb_init(&b);
        sb_puts(&b, "  ");
        sb_puts(&b, name && *name ? name : "show all");
        sr_finish(out, n, &b, cols);
        return;
    }
    if (done < 0) done = 0;
    if (total < 0) total = 0;
    if (done > total) done = total;
    bool prog = total > 0;
    const char *rt = right && *right ? right : NULL;
    const char *md = mid && *mid ? mid : NULL;
    bool trail = prog || md || rt;
    const char *prefix = kind == STRIP_ROW_MAIN ? "● " : "  ◯ ";
    if (!name) name = "";
    if (name_w < 1) name_w = sr_cols(name);

    sbuf b; sb_init(&b);
    sb_puts(&b, prefix);
    if (!trail) {
        int room = cols - sr_cols(prefix);
        sr_put(&b, name, room > 0 ? room : 0, true);
        sr_finish(out, n, &b, cols);
        return;
    }
    int nw = sr_cols(name), shown = nw > name_w ? name_w : nw;
    sr_put(&b, name, name_w, true);
    sr_pad(&b, shown, name_w);
    sb_puts(&b, "  ");
    int left = sr_cols(b.data);

    if (prog) {
        char frac[32], tail[160];
        snprintf(frac, sizeof frac, "%d/%d", done, total);
        if (rt) snprintf(tail, sizeof tail, "%s · %s", frac, rt);
        else snprintf(tail, sizeof tail, "%s", frac);
        int tw = sr_cols(tail), avail = cols - left;
        if (avail < 1) { sr_finish(out, n, &b, cols); return; }
        int room = avail - 2 - tw;   /* two spaces between the bar and the fraction */
        if (tw > avail || room < 4) sr_put(&b, tail, avail, true);
        else {
            int barw = room < 20 ? room : 20;
            int fill = done <= 0 ? 0 : (done >= total ? barw : (done * barw) / total);
            if (done > 0 && done < total && fill == 0) fill = 1;
            if (done >= total) fill = barw;
            for (int i = 0; i < barw; i++) sb_puts(&b, i < fill ? "▰" : "▱");
            sb_puts(&b, "  ");
            sb_puts(&b, tail);
        }
        sr_finish(out, n, &b, cols);
        return;
    }

    /* activity: the text on the left, the time and tokens at the right edge */
    int rw = rt ? sr_cols(rt) : 0, gap = rt ? 2 : 0, mid_w = cols - left - gap - rw;
    if (rt && mid_w < 0) {
        if (rw < cols - left) { sr_pad(&b, left, cols - rw); sb_puts(&b, rt); }
        else sr_put(&b, md ? md : "", cols - left, true);
        sr_finish(out, n, &b, cols);
        return;
    }
    sr_put(&b, md ? md : "", mid_w > 0 ? mid_w : 0, true);
    if (rt) { sr_pad(&b, sr_cols(b.data), cols - rw); sb_puts(&b, rt); }
    sr_finish(out, n, &b, cols);
}

void strip_layout(int room, int total, int *rows, int *shown, bool *with_main, bool *more) {
    int r = 0, s = 0; bool m = false, o = false;
    if (room < 0) room = 0;
    if (total < 0) total = 0;
    if (total > 0 && room > 0) {
        if (1 + total <= room) { m = true; s = total; r = 1 + total; }          /* ● main and every task */
        else if (total <= room) { s = total; r = total; }                        /* the tasks fill the room: no main row */
        else {
            o = true;                                                            /* the last row is "show all" */
            int body = room - 1;
            if (body >= 2) { m = true; s = body - 1; }                           /* ● main, then as many tasks as fit */
            else if (body == 1) s = 1;                                           /* one task, then "show all" */
            if (s > total) s = total;
            r = (m ? 1 : 0) + s + 1;
        }
    }
    if (rows) *rows = r;
    if (shown) *shown = s;
    if (with_main) *with_main = m;
    if (more) *more = o;
}

/* The first line of a shell command, as a fold's head: the leading blank lines and the trailing
 * whitespace go, control characters become spaces, and the line is cut to `width` columns (a
 * character is a column; the cut ends with "…"). Returns how many lines follow the first one,
 * not counting blank ones at the end: 0 means the command is one line and needs no fold. */
int cmd_head(const char *cmd, int width, char *out, size_t n) {
    size_t len = strlen(cmd), s = 0, e = len;
    while (s < len && isspace((unsigned char)cmd[s])) s++;
    while (e > s && isspace((unsigned char)cmd[e - 1])) e--;
    int more = 0;
    for (size_t i = s; i < e; i++) if (cmd[i] == '\n') more++;
    size_t fl = strcspn(cmd + s, "\n");
    if (s + fl > e) fl = e - s;
    while (fl > 0 && isspace((unsigned char)cmd[s + fl - 1])) fl--;
    int columns = 0;
    for (size_t i = 0; i < fl; i++) if (((unsigned char)cmd[s + i] & 0xC0) != 0x80) columns++;
    bool cut = columns > width;
    int limit = cut ? width - 1 : width; if (limit < 0) limit = 0;
    size_t o = 0; int cols = 0;
    if (n == 0) return more;
    for (size_t i = 0; i < fl; i++) {
        unsigned char c = (unsigned char)cmd[s + i];
        bool lead = (c & 0xC0) != 0x80;
        if (lead && cols == limit) break;
        if (lead) cols++;
        if (c < 32 || c == 127) c = ' ';
        if (o + 1 >= n) break;
        out[o++] = (char)c;
    }
    if (cut && o + 3 < n) { memcpy(out + o, "\xe2\x80\xa6", 3); o += 3; }   /* … */
    out[o] = 0;
    return more;
}

/* A tool result for a call the conversation has no answer to (see messages_repair). */
static cJSON *orphan_result(const char *name, cJSON *id) {
    cJSON *tm = cJSON_CreateObject();
    cJSON_AddStringToObject(tm, "role", "tool");
    cJSON_AddStringToObject(tm, "tool_name", name);
    if (cJSON_IsString(id) && id->valuestring[0]) cJSON_AddStringToObject(tm, "tool_call_id", id->valuestring);
    cJSON_AddStringToObject(tm, "content",
        "not run: the request stopped before this call ran (its tool round hit the limit, or was interrupted). "
        "The user's next message follows; call the tool again if it is still needed.");
    return tm;
}

static const char *msg_role(cJSON *m) {
    cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
    return cJSON_IsString(r) ? r->valuestring : "";
}

static bool thinking_block(cJSON *b) {
    cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type");
    return cJSON_IsString(ty) && (!strcmp(ty->valuestring, "thinking") || !strcmp(ty->valuestring, "redacted_thinking"));
}

int messages_drop_thinking(cJSON *msgs, int from) {
    if (!cJSON_IsArray(msgs) || from < 0) return 0;
    int dropped = 0, n = cJSON_GetArraySize(msgs);
    for (int i = from; i < n; i++) {
        cJSON *m = cJSON_GetArrayItem(msgs, i);
        cJSON *pc = cJSON_GetObjectItemCaseSensitive(m, "provider_content");
        if (!cJSON_IsArray(pc)) continue;
        for (int k = cJSON_GetArraySize(pc) - 1; k >= 0; k--) {
            if (!thinking_block(cJSON_GetArrayItem(pc, k))) continue;
            cJSON_DeleteItemFromArray(pc, k);
            dropped++;
        }
        if (cJSON_GetArraySize(pc) == 0) cJSON_DeleteItemFromObject(m, "provider_content");
        cJSON_DeleteItemFromObject(m, "think_bind");   /* the signature it recorded is gone with the blocks */
    }
    return dropped;
}

static bool message_has_thinking(cJSON *m) {
    cJSON *pc = cJSON_GetObjectItemCaseSensitive(m, "provider_content"), *b;
    cJSON_ArrayForEach(b, pc) if (thinking_block(b)) return true;
    return false;
}
bool messages_has_thinking(cJSON *msgs, int from) {
    if (!cJSON_IsArray(msgs) || from < 0) return false;
    int n = cJSON_GetArraySize(msgs);
    for (int i = from; i < n; i++) if (message_has_thinking(cJSON_GetArrayItem(msgs, i))) return true;
    return false;
}

/* FNV-1a of one message, the fields the next call actually replays. think_bind itself is not part of it:
 * recording the prefix must not change the prefix. */
static uint64_t think_fnv(uint64_t h, const char *s) {
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
    return h;
}
static const char *msg_field(cJSON *m, const char *k) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(m, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static uint64_t think_hash_message(uint64_t h, cJSON *m) {
    h = think_fnv(h, msg_field(m, "role"));
    h = think_fnv(h, "\n");
    h = think_fnv(h, msg_field(m, "content"));
    h = think_fnv(h, "\n");
    h = think_fnv(h, msg_field(m, "tool_name"));
    h = think_fnv(h, "\n");
    h = think_fnv(h, msg_field(m, "tool_call_id"));
    h = think_fnv(h, "\n");
    cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
    char *t = tc ? cJSON_PrintUnformatted(tc) : NULL;
    h = think_fnv(h, t ? t : "");
    free(t);
    h = think_fnv(h, "\n");
    cJSON *pc = cJSON_GetObjectItemCaseSensitive(m, "provider_content");
    char *p = pc ? cJSON_PrintUnformatted(pc) : NULL;
    h = think_fnv(h, p ? p : "");
    free(p);
    return think_fnv(h, "\n");
}
static uint64_t think_prefix_hash(cJSON *msgs, int upto) {
    uint64_t h = 14695981039346656037ULL;
    int n = cJSON_IsArray(msgs) ? cJSON_GetArraySize(msgs) : 0;
    if (upto > n) upto = n;
    for (int i = 0; i < upto; i++) h = think_hash_message(h, cJSON_GetArrayItem(msgs, i));
    return h;
}
static void think_bind_hex(uint64_t h, char *buf, size_t n) {
    snprintf(buf, n, "%016llx", (unsigned long long)h);
}
void messages_note_thinking(cJSON *msgs, int index) {
    if (!cJSON_IsArray(msgs) || index < 0 || index >= cJSON_GetArraySize(msgs)) return;
    cJSON *m = cJSON_GetArrayItem(msgs, index);
    if (!message_has_thinking(m)) return;
    char buf[32];
    think_bind_hex(think_prefix_hash(msgs, index), buf, sizeof buf);
    cJSON *old = cJSON_GetObjectItemCaseSensitive(m, "think_bind");
    if (cJSON_IsString(old)) cJSON_SetValuestring(old, buf);
    else cJSON_AddStringToObject(m, "think_bind", buf);
}
int messages_thinking_cut(cJSON *msgs) {
    if (!messages_has_thinking(msgs, 0)) return -1;
    uint64_t h = 14695981039346656037ULL;
    int n = cJSON_GetArraySize(msgs);
    for (int i = 0; i < n; i++) {
        cJSON *m = cJSON_GetArrayItem(msgs, i);
        cJSON *b = cJSON_GetObjectItemCaseSensitive(m, "think_bind");
        if (message_has_thinking(m) && cJSON_IsString(b) && b->valuestring[0]) {
            char buf[32];
            think_bind_hex(h, buf, sizeof buf);
            if (strcmp(buf, b->valuestring)) return i;   /* this block and every later one: a gap is refused too */
        }
        h = think_hash_message(h, m);
    }
    return -1;
}

int messages_repair(cJSON *msgs) {
    int fixed = 0;
    for (int i = 0; i < cJSON_GetArraySize(msgs); i++) {
        cJSON *m = cJSON_GetArrayItem(msgs, i);
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(c) && !utf8_ok(c->valuestring, strlen(c->valuestring))) {
            char *clean = utf8_fix(c->valuestring);
            cJSON_SetValuestring(c, clean); free(clean); fixed++;
        }
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        if (strcmp(msg_role(m), "assistant") || !cJSON_IsArray(tc)) continue;
        /* the run of tool results that follows this reply: each call needs one of them */
        int end = i + 1;
        while (end < cJSON_GetArraySize(msgs) && !strcmp(msg_role(cJSON_GetArrayItem(msgs, end)), "tool")) end++;
        cJSON *call;
        cJSON_ArrayForEach(call, tc) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            if (!cJSON_IsString(id) || !id->valuestring[0]) continue;   /* no id, nothing to match it by */
            bool answered = false;
            for (int k = i + 1; k < end && !answered; k++) {
                cJSON *rid = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(msgs, k), "tool_call_id");
                answered = cJSON_IsString(rid) && !strcmp(rid->valuestring, id->valuestring);
            }
            if (answered) continue;
            cJSON_InsertItemInArray(msgs, end, orphan_result(cJSON_IsString(nm) ? nm->valuestring : "?", id));
            end++; fixed++;
        }
        i = end - 1;
    }
    return fixed;
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

/* ---------- the advisor: how much it is leaned on ----------
 * normal is what the advisor always was: the agent asks when it judges the work hard. A small
 * model is a poor judge of that — it is sure of the wrong approach, and calls a task done that is
 * not — so strong has corbienest ask as well, before a request that changed a lot ends, and max
 * also before the request's first change is made. The advice gets longer and more concrete the
 * higher it goes: the reader is a model that follows steps better than it follows advice. */
const advisor_guidance_def ADVISOR_GUIDANCE[GUIDANCE_COUNT] = {
    { "light",  1, 250, 32 * 1024, false, false, false, false, "only when the agent is stuck · 1 consultation per request · short answers" },
    { "normal", 3, 400, 48 * 1024, false, false, false, true,  "when the work is hard · 3 per request" },
    { "strong", 6, 700, 96 * 1024, true,  false, false, true,  "early and often · 6 per request · concrete steps · and reviews a request that changed a lot (40 lines or 3 files) before it ends" },
    { "max",   10, 900, 96 * 1024, true,  true,  true,  true,  "as strong, 10 per request · reviews any change before the request ends · and checks the first change of a request before it is made" },
};

/* Changed lines of a unified diff: the lines that add or remove text, not the ---/+++ headers. */
int diff_changed_lines(const char *d) {
    int n = 0;
    for (const char *p = d ? d : ""; *p; ) {
        if ((*p == '+' && strncmp(p, "+++", 3)) || (*p == '-' && strncmp(p, "---", 3))) n++;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

/* What counts as a tool call that failed, for the advisor stepping in: a result that is an error,
 * or a shell command whose last line says it did not exit with 0. A consultation that could not
 * be had is not one — asking the advisor about the advisor helps nobody. */
bool tool_result_failed(const char *name, const char *result, bool error) {
    if (!name || !strcmp(name, "advisor")) return false;
    if (error) return true;
    if (strcmp(name, "bash") || !result) return false;
    const char *p = NULL;
    for (const char *q = result; (q = strstr(q, "exit code: ")); q += 11) p = q;   /* the last one: the command's output may say it too */
    return p && atoi(p + 11) != 0;
}
const advisor_guidance_def *advisor_guidance(void) {
    int g = g_cfg.advisor_guidance;
    return &ADVISOR_GUIDANCE[g >= 0 && g < GUIDANCE_COUNT ? g : GUIDANCE_NORMAL];
}
int advisor_guidance_parse(const char *s) {
    for (int i = 0; s && i < GUIDANCE_COUNT; i++) if (!strcasecmp(s, ADVISOR_GUIDANCE[i].name)) return i;
    return -1;
}

/* "LGTM", "LGTM.", "LGTM — the change is correct and complete": nothing to act on. "LGTM, but
 * the test for X is missing" is advice all the same, and goes to the agent. Taking an approval
 * for advice costs a round of the main model, which on a small machine is minutes; taking advice
 * for an approval loses what the advisor found — so a short sentence after it is still an
 * approval, one that objects to anything is not. */
bool advisor_approves(const char *advice) {
    while (advice && (*advice == ' ' || *advice == '\n' || *advice == '*')) advice++;
    if (!advice || strncasecmp(advice, ADVISOR_REVIEW_OK, strlen(ADVISOR_REVIEW_OK))) return false;
    /* words that object, or make it conditional — not ones an approval uses as readily
     * ("the fix is right", "the tests still pass", "nothing to add") */
    static const char *OBJECTIONS[] = { "but", "however", "except", "though", "although", "once", "unless", "until", "missing", "should",
                                        "must", "need", "needs", "wrong", "not", "bug", "fails", "failing", "instead", NULL };
    int words = 0;
    for (const char *c = advice + strlen(ADVISOR_REVIEW_OK); *c; ) {
        while (*c && !isalnum((unsigned char)*c)) c++;
        const char *w = c;
        while (*c && (isalnum((unsigned char)*c) || *c == '\'')) c++;
        if (c == w) break;
        if (++words > 15) return false;
        for (int i = 0; OBJECTIONS[i]; i++)
            if ((size_t)(c - w) == strlen(OBJECTIONS[i]) && !strncasecmp(w, OBJECTIONS[i], (size_t)(c - w))) return false;
    }
    return true;
}

/* ---------- the advisor: where it runs, and what it is shown ----------
 * Three placements, and they want opposite things. The main model as its own advisor must be
 * called exactly as the main model is: another num_ctx makes the server reload it, and
 * keep_alive 0 would unload it — either way the conversation's cache is gone. A model that is
 * not on this machine (one of Ollama's cloud models, or a hosted API) has its own window and no
 * memory of ours to give back, so neither key means anything. Another local model gets a window
 * of its own (a 70B model pays several times the memory per token of context that a small one
 * does) and keep_alive 0, so that its memory is free again when the main model wants it back. */
void advisor_plan_for(const char *advisor, int trained_ctx, advisor_plan *p) {
    memset(p, 0, sizeof *p);
    p->same = model_same(advisor, g_cfg.model);
    p->api = !p->same && provider_find(advisor, NULL) != NULL;
    p->cloud = !p->same && (p->api || model_is_cloud(advisor));
    if (p->same) { p->window = g_cfg.num_ctx > 0 ? g_cfg.num_ctx : ADVISOR_CTX_UNSET; }
    else if (p->api) { p->window = trained_ctx > 0 ? trained_ctx : ADVISOR_CTX_API; p->send_ctx = -1; p->keep_alive = ""; }
    else if (p->cloud) {
        p->window = trained_ctx > 0 && trained_ctx < ADVISOR_CTX_CLOUD ? trained_ctx : ADVISOR_CTX_CLOUD;
        p->send_ctx = -1; p->keep_alive = "";
    } else {
        int n = g_cfg.advisor_ctx > 0 ? g_cfg.advisor_ctx : g_cfg.num_ctx > ADVISOR_CTX_AUTO ? ADVISOR_CTX_AUTO : g_cfg.num_ctx;
        if (n > 0 && trained_ctx > 0 && n > trained_ctx) n = trained_ctx;
        p->window = n > 0 ? n : ADVISOR_CTX_UNSET;   /* left to the server: plan for the little it gives */
        p->send_ctx = n; p->keep_alive = "0";
    }
    int cap = p->api ? ADVISOR_REPLY_API : ADVISOR_REPLY_MAX;
    p->reply = p->window / 3 < cap ? p->window / 3 : cap;
    /* bytes for the whole prompt at a careful 3 bytes a token (it is another tokenizer than the
     * one the context estimate is calibrated on), and no more than the guidance's brief_max
     * however large the window: what the advisor has to read first is the wait */
    int tokens = p->window - p->reply - 256;
    size_t room = tokens > 0 ? (size_t)tokens * 3 : 0, most = advisor_guidance()->brief_max;
    if (room < ADVISOR_BRIEF_MIN) room = ADVISOR_BRIEF_MIN;   /* a window too small to plan for (-c 512): a floor, and the server cuts what it must */
    p->budget = room < most ? room : most;
}

/* The one user message of an advisor call. What matters most stands last — the question, and
 * who the reader is: a server that has to cut an over-long prompt keeps the end of it. */
char *advisor_brief(cJSON *msgs, int keep, size_t budget, const char *env, bool plan_mode, const char *rules, const char *question, int words) {
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
    sb_printf(&b, "\nAnswer the agent now — verdict first, then the steps, under about %d words. "
                  "You are the advisor, not the agent: do not continue the conversation above and do not call tools.\n", words > 0 ? words : 400);
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

/* ---------- syntax highlighting for code in a fenced block ----------
 * Basic and by the fence's name: keywords, strings, comments and numbers, for the usual languages. The
 * escapes are the only thing added — the text, and so its columns, is what it was. `base` is the colour the
 * code is printed in: each token ends by going back to it. A language not known is left as it is. */
typedef enum { HL_NONE, HL_C, HL_PY, HL_SH, HL_JSON } hl_lang;
static hl_lang hl_language(const char *name) {
    static const struct { const char *n; hl_lang l; } NAMES[] = {
        { "c", HL_C }, { "h", HL_C }, { "cpp", HL_C }, { "c++", HL_C }, { "cc", HL_C }, { "hpp", HL_C }, { "java", HL_C },
        { "js", HL_C }, { "javascript", HL_C }, { "ts", HL_C }, { "typescript", HL_C }, { "go", HL_C }, { "rust", HL_C },
        { "rs", HL_C }, { "cs", HL_C }, { "csharp", HL_C }, { "swift", HL_C }, { "kotlin", HL_C }, { "php", HL_C }, { "scala", HL_C },
        { "py", HL_PY }, { "python", HL_PY }, { "python3", HL_PY },
        { "sh", HL_SH }, { "bash", HL_SH }, { "shell", HL_SH }, { "zsh", HL_SH }, { "console", HL_SH },
        { "json", HL_JSON }, { "jsonc", HL_JSON },
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
        if (!strcasecmp(name, NAMES[i].n)) return NAMES[i].l;
    return HL_NONE;
}
static bool hl_keyword(hl_lang l, const char *w, size_t n) {
    static const char *const C_KW[] = { "auto", "break", "case", "catch", "char", "class", "const", "continue", "default", "do", "double",
        "else", "enum", "extern", "false", "final", "finally", "float", "for", "func", "fn", "function", "go", "if", "impl", "import",
        "in", "int", "interface", "let", "long", "match", "mut", "new", "nil", "null", "package", "private", "protected", "public",
        "return", "short", "signed", "sizeof", "static", "struct", "super", "switch", "this", "throw", "true", "try", "type", "typedef",
        "union", "unsigned", "use", "var", "void", "volatile", "while", "const", "export", "from", "async", "await", "yield", "self",
        "bool", "string", "undefined", "pub", "mod", "where", "trait", "defer", "select", "chan", "range", "readonly", NULL };
    static const char *const PY_KW[] = { "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif",
        "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None", "nonlocal", "not",
        "or", "pass", "raise", "return", "True", "try", "while", "with", "yield", "self", NULL };
    static const char *const SH_KW[] = { "case", "do", "done", "elif", "else", "esac", "export", "fi", "for", "function", "if", "in",
        "local", "return", "select", "then", "until", "while", "readonly", "unset", "declare", NULL };
    static const char *const JSON_KW[] = { "true", "false", "null", NULL };
    const char *const *list = l == HL_C ? C_KW : l == HL_PY ? PY_KW : l == HL_SH ? SH_KW : JSON_KW;
    for (int i = 0; list[i]; i++) if (strlen(list[i]) == n && !strncmp(list[i], w, n)) return true;
    return false;
}

void highlight_line(const char *lang, const char *line, sbuf *out, const char *base) {
    hl_lang l = hl_language(lang ? lang : "");
    if (l == HL_NONE || !line) { sb_puts(out, line ? line : ""); return; }
    bool hash = l == HL_PY || l == HL_SH;          /* comments: '#' … the end of the line */
    bool slash = l == HL_C;                        /* comments: '//' to the end, and a block comment within the line */
    const char *p = line;
    while (*p) {
        if ((hash && *p == '#') || (slash && p[0] == '/' && p[1] == '/')) {
            sb_puts(out, C_GRAY); sb_puts(out, p); sb_puts(out, C_RESET); sb_puts(out, base); return;
        }
        if (slash && p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/"); size_t n = e ? (size_t)(e + 2 - p) : strlen(p);
            sb_puts(out, C_GRAY); sb_append(out, p, n); sb_puts(out, C_RESET); sb_puts(out, base);
            p += n; continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p; const char *s = p++;
            while (*p && *p != q) { if (*p == '\\' && p[1]) p++; p++; }
            if (*p) p++;
            sb_puts(out, C_GREEN); sb_append(out, s, (size_t)(p - s)); sb_puts(out, C_RESET); sb_puts(out, base);
            continue;
        }
        if (isdigit((unsigned char)*p) && !(p > line && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '.' || *p == '_') p++;
            sb_puts(out, C_YELLOW); sb_append(out, s, (size_t)(p - s)); sb_puts(out, C_RESET); sb_puts(out, base);
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            if (hl_keyword(l, s, (size_t)(p - s))) { sb_puts(out, C_MAGENTA); sb_append(out, s, (size_t)(p - s)); sb_puts(out, C_RESET); sb_puts(out, base); }
            else sb_append(out, s, (size_t)(p - s));
            continue;
        }
        sb_putc(out, *p++);
    }
}

/* ---------- the git branch of a directory (the status bar's location line) ----------
 * The .git of dir or of the nearest directory above it (a file there, as a linked worktree has, points to
 * its git directory). Writes the branch name, or the first seven digits of the commit a detached HEAD is on,
 * into out. 1 when dir is in a repository, else 0. Reads only: a few small files, so it can run at every redraw. */
int git_branch(const char *dir, char *out, size_t n) {
    char cur[4096], gitp[4200], head[4300];
    if (!dir || !*dir || strlen(dir) >= sizeof cur) return 0;
    snprintf(cur, sizeof cur, "%s", dir);
    for (;;) {
        snprintf(gitp, sizeof gitp, "%s/.git", cur);
        /* opened once and asked through the descriptor (a stat, then an open of the name, could see another file) */
        int fd = open(gitp, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); snprintf(head, sizeof head, "%s/HEAD", gitp); }
            else {   /* "gitdir: PATH": a linked worktree */
                FILE *f = fdopen(fd, "r"); char line[1024] = "";
                if (!f) { close(fd); return 0; }
                if (!fgets(line, sizeof line, f)) line[0] = 0;
                fclose(f);
                size_t l = strlen(line); while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
                if (strncmp(line, "gitdir: ", 8)) return 0;
                if (line[8] == '/') snprintf(head, sizeof head, "%s/HEAD", line + 8);
                else snprintf(head, sizeof head, "%s/%s/HEAD", cur, line + 8);
            }
            break;
        }
        if (!strcmp(cur, "/")) return 0;   /* the root: no repository */
        char *slash = strrchr(cur, '/');
        if (slash == cur) strcpy(cur, "/"); else *slash = 0;
    }
    FILE *f = fopen(head, "r");
    if (!f) return 0;
    char line[512] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    size_t l = strlen(line); while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
    if (!strncmp(line, "ref: refs/heads/", 16)) snprintf(out, n, "%s", line + 16);
    else if (strlen(line) >= 7) snprintf(out, n, "%.7s", line);   /* detached: the commit */
    else snprintf(out, n, "%s", "HEAD");
    return 1;
}

/* The text goes on the end of a sub-agent's log, as plain text: escape sequences and control
 * bytes other than tab and newline are dropped (a fold body is written into the model, where a
 * CR or an ESC would move the cursor). Past `cap` bytes the front goes, cut after a line break,
 * with a note that lines were dropped. */
void agent_log_add(sbuf *log, const char *text, size_t cap) {
    static const char NOTE[] = "… earlier lines dropped\n";
    size_t nl = strlen(NOTE);
    sbuf plain; sb_init(&plain);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == 27) {   /* ESC [ params final, or ESC x */
            if (!p[1]) break;
            if (p[1] == '[') { p += 2; while (*p && (*p < 0x40 || *p > 0x7e)) p++; if (!*p) break; }
            else p++;
            continue;
        }
        if (*p == '\r' || *p == 0x7f || (*p < 0x20 && *p != '\n' && *p != '\t')) continue;
        sb_append(&plain, (const char *)p, 1);
    }
    sb_append(log, plain.data ? plain.data : "", plain.len);
    sb_free(&plain);
    if (log->len <= cap) return;
    size_t keep = cap > nl ? cap - nl : 0;   /* what stays, the note included, is at most cap */
    size_t cut = log->len - keep;            /* at least one byte goes */
    while (cut < log->len && log->data[cut - 1] != '\n') cut++;
    memmove(log->data, log->data + cut, log->len - cut);
    log->len -= cut;
    sbuf b; sb_init(&b);
    sb_append(&b, NOTE, nl); sb_append(&b, log->data, log->len);
    sb_free(log); *log = b;
}

/* "[Pasted text #K +N lines]" at s[i..]: its K and N and the byte after it, or false */
static bool paste_token_at(const char *s, size_t len, size_t i, int *k, int *nl, size_t *end) {
    static const char HEAD[] = "[Pasted text #";
    size_t h = sizeof HEAD - 1;
    if (i + h > len || memcmp(s + i, HEAD, h)) return false;
    size_t p = i + h; long kv = 0, nv = 0;
    if (p >= len || s[p] < '0' || s[p] > '9') return false;
    while (p < len && s[p] >= '0' && s[p] <= '9' && kv < 100000) kv = kv * 10 + (s[p++] - '0');
    if (kv < 1 || p + 2 > len || memcmp(s + p, " +", 2)) return false;
    p += 2;
    if (p >= len || s[p] < '0' || s[p] > '9') return false;
    while (p < len && s[p] >= '0' && s[p] <= '9' && nv < 100000) nv = nv * 10 + (s[p++] - '0');
    if (nv < 1 || p + 7 > len || memcmp(s + p, " lines]", 7)) return false;
    *k = (int)kv; *nl = (int)nv; *end = p + 7;
    return true;
}
size_t paste_tokens(const char *s, size_t len, size_t (*sp)[2], int *ks, int *ns, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < len && n < max; i++) {
        int k, nl; size_t end;
        if (s[i] == '[' && paste_token_at(s, len, i, &k, &nl, &end)) {
            if (sp) { sp[n][0] = i; sp[n][1] = end; }
            if (ks) ks[n] = k;
            if (ns) ns[n] = nl;
            n++; i = end - 1;
        }
    }
    return n;
}
bool paste_is_block(const char *t, size_t n, int *lines) {
    int nl = 0;
    for (size_t i = 0; i < n; i++) if (t[i] == '\n') nl++;
    if (n && t[n - 1] != '\n') nl++;
    if (lines) *lines = nl;
    return nl > PASTE_LINES || n > PASTE_BYTES;
}

char *paste_clean(const char *s, size_t n, size_t *out_len) {
    sbuf q; sb_init(&q);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') { if (i + 1 < n && s[i + 1] == '\n') continue; c = '\n'; }
        if ((unsigned char)c < 32 && c != '\n' && c != '\t') continue;
        if (c == 127) continue;
        sb_putc(&q, c);
    }
    if (utf8_ok(q.data ? q.data : "", q.len)) {
        if (out_len) *out_len = q.len;
        return sb_detach(&q);
    }
    /* a byte that is not UTF-8 becomes U+FFFD here, as the request path would make it (messages_repair) */
    char *fixed = utf8_fix(q.data ? q.data : "");
    sb_free(&q);
    if (out_len) *out_len = strlen(fixed);
    return fixed;
}

char *paste_expand(const char *s, size_t len, const char *const *texts, const int *lines, int n) {
    sbuf o; sb_init(&o);
    for (size_t i = 0; i < len; i++) {
        int k, nl; size_t end;
        if (s[i] == '[' && paste_token_at(s, len, i, &k, &nl, &end) && k <= n && nl == lines[k - 1]) {
            sb_puts(&o, texts[k - 1]); i = end - 1;
        } else sb_putc(&o, s[i]);
    }
    return sb_detach(&o);
}

/* ---------- rate limits of a response (anthropic-ratelimit-* headers) ---------- */
/* One header line as libcurl hands it over ("name: value", CRLF at the end); only the
 * requests and token headers of the limits count, the tokens-* aggregate is left out. */
void ratelimit_header(ratelimit_info *rl, const char *line, size_t n) {
    static const char P[] = "anthropic-ratelimit-";
    size_t pl = sizeof P - 1;
    if (n < pl || strncasecmp(line, P, pl)) return;
    const char *colon = memchr(line, ':', n);
    if (!colon) return;
    size_t nl = (size_t)(colon - line);
    char name[64], val[64];
    if (nl >= sizeof name) return;
    memcpy(name, line, nl); name[nl] = 0;
    for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
    const char *v = colon + 1; const char *end = line + n;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t')) end--;
    if ((size_t)(end - v) >= sizeof val) return;
    memcpy(val, v, (size_t)(end - v)); val[end - v] = 0;
    const char *what = name + pl;
    long long *slot = NULL;
    if (!strncmp(what, "requests-", 9)) {
        if (!strcmp(what + 9, "limit")) slot = &rl->req_lim; else if (!strcmp(what + 9, "remaining")) slot = &rl->req_rem;
    } else if (!strncmp(what, "input-tokens-", 13)) {
        if (!strcmp(what + 13, "limit")) slot = &rl->in_lim; else if (!strcmp(what + 13, "remaining")) slot = &rl->in_rem;
        else if (!strcmp(what + 13, "reset")) { snprintf(rl->reset, sizeof rl->reset, "%.39s", val); rl->have = true; return; }
    } else if (!strncmp(what, "output-tokens-", 14)) {
        if (!strcmp(what + 14, "limit")) slot = &rl->out_lim; else if (!strcmp(what + 14, "remaining")) slot = &rl->out_rem;
    }
    if (!slot) return;
    char *e; long long x = strtoll(val, &e, 10);
    if (e == val || *e) return;
    *slot = x; rl->have = true;
}

/* The last rate limits seen per model this session: /usage shows them under each hosted model. */
#define RATELIMIT_MAX 16
static struct { char model[128]; ratelimit_info rl; } g_ratelimits[RATELIMIT_MAX];
static int g_n_ratelimits = 0;
void ratelimit_set(const char *model, const ratelimit_info *rl) {
    if (!model || !rl || !rl->have) return;
    for (int i = 0; i < g_n_ratelimits; i++)
        if (!strcmp(g_ratelimits[i].model, model)) { g_ratelimits[i].rl = *rl; return; }
    if (g_n_ratelimits == RATELIMIT_MAX) return;
    snprintf(g_ratelimits[g_n_ratelimits].model, sizeof g_ratelimits[0].model, "%s", model);
    g_ratelimits[g_n_ratelimits++].rl = *rl;
}
bool ratelimit_get(const char *model, ratelimit_info *out) {
    for (int i = 0; model && i < g_n_ratelimits; i++)
        if (!strcmp(g_ratelimits[i].model, model)) { *out = g_ratelimits[i].rl; return true; }
    return false;
}

/* ---------- /goal: the pure half (see common.h) ---------- */
static const char *goal_skip_space(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

bool goal_parse(const char *arg, goal_spec *g, char *err, size_t errn) {
    memset(g, 0, sizeof *g);
    g->rounds = 3; g->cands = ORCH_WORKERS_DEFAULT;
    const char *p = goal_skip_space(arg);
    if (!*p) { snprintf(err, errn, "no metric command"); return false; }
    sbuf m; sb_init(&m);
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        const char *e = strchr(p, q);
        if (!e) { sb_free(&m); snprintf(err, errn, "the metric command has no closing %c", q); return false; }
        sb_append(&m, p, (size_t)(e - p)); p = e + 1;
    } else {
        const char *e = p; while (*e && *e != ' ' && *e != '\t') e++;
        sb_append(&m, p, (size_t)(e - p)); p = e;
    }
    if (!m.len) { sb_free(&m); snprintf(err, errn, "no metric command"); return false; }
    g->metric = sb_detach(&m);
    for (;;) {   /* options, in any order, before the task */
        p = goal_skip_space(p);
        const char *e = p; while (*e && *e != ' ' && *e != '\t') e++;
        size_t n = (size_t)(e - p);
        if (n == 3 && !strncmp(p, "min", 3)) { g->lower = true; p = e; continue; }
        if (n == 3 && !strncmp(p, "max", 3)) { g->lower = false; p = e; continue; }
        if (n == 2 && !strncmp(p, "-m", 2)) {
            const char *v = goal_skip_space(e), *ve = v;
            while (*ve && *ve != ' ' && *ve != '\t') ve++;
            if (ve == v) { snprintf(err, errn, "-m needs a list of models, a,b,c"); goal_spec_free(g); return false; }
            int cnt = 1; for (const char *c = v; c < ve; c++) if (*c == ',') cnt++;
            if (cnt > ORCH_WORKERS_MAX) { snprintf(err, errn, "-m: at most %d models", ORCH_WORKERS_MAX); goal_spec_free(g); return false; }
            char **list = xmalloc(sizeof *list * (size_t)cnt); int k = 0;
            for (const char *c = v; c <= ve; ) {
                const char *comma = c; while (comma < ve && *comma != ',') comma++;
                if (comma == c) { for (int j = 0; j < k; j++) free(list[j]); free(list); snprintf(err, errn, "-m: an empty model name in the list"); goal_spec_free(g); return false; }
                list[k] = xmalloc((size_t)(comma - c) + 1); memcpy(list[k], c, (size_t)(comma - c)); list[k][comma - c] = 0; k++;
                c = comma + 1;
                if (comma == ve) break;
            }
            for (int j = 0; j < g->nmodels; j++) free(g->models[j]);
            free(g->models);
            g->models = list; g->nmodels = k;
            p = ve; continue;
        }
        if (n == 2 && (!strncmp(p, "-n", 2) || !strncmp(p, "-k", 2))) {
            bool rounds = p[1] == 'n';
            const char *v = goal_skip_space(e);
            char *end; long k = strtol(v, &end, 10);
            if (end == v || (*end && *end != ' ' && *end != '\t')) { snprintf(err, errn, "%.2s needs a number", rounds ? "-n" : "-k"); goal_spec_free(g); return false; }
            if (rounds && (k < 1 || k > GOAL_ROUNDS_MAX)) { snprintf(err, errn, "-n: 1 to %d rounds", GOAL_ROUNDS_MAX); goal_spec_free(g); return false; }
            if (!rounds && (k < 2 || k > ORCH_WORKERS_MAX)) { snprintf(err, errn, "-k: 2 to %d candidates (one candidate has nothing to fight)", ORCH_WORKERS_MAX); goal_spec_free(g); return false; }
            if (rounds) g->rounds = (int)k; else g->cands = (int)k;
            p = end; continue;
        }
        break;
    }
    p = goal_skip_space(p);
    while (*p == '\n' || *p == '\r') p++;
    size_t tl = strlen(p);
    while (tl && (p[tl - 1] == ' ' || p[tl - 1] == '\t' || p[tl - 1] == '\n' || p[tl - 1] == '\r')) tl--;
    if (!tl) { snprintf(err, errn, "no goal: say what to reach after the options"); goal_spec_free(g); return false; }
    g->task = xmalloc(tl + 1); memcpy(g->task, p, tl); g->task[tl] = 0;
    return true;
}

void goal_spec_free(goal_spec *g) {
    free(g->metric); free(g->task);
    for (int i = 0; i < g->nmodels; i++) free(g->models[i]);
    free(g->models);
    memset(g, 0, sizeof *g);
}

bool goal_number(const char *out, double *v) {
    if (!out) return false;
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    size_t start = n;
    while (start > 0 && out[start - 1] != '\n') start--;   /* the last line */
    while (start < n && (out[start] == ' ' || out[start] == '\t')) start++;
    if (start >= n) return false;
    char line[64];
    if (n - start >= sizeof line) return false;
    memcpy(line, out + start, n - start); line[n - start] = 0;
    char *end; double d = strtod(line, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (*end || end == line || d != d || d - d != 0) return false;   /* nothing after the number; not nan, not inf */
    *v = d;
    return true;
}

bool goal_better(double v, double best, bool lower) { return lower ? v < best : v > best; }

int goal_verdict(const char *report, char *why, size_t n) {
    int found = GOAL_NONE;
    if (why && n) why[0] = 0;
    if (!report) return GOAL_NONE;
    for (const char *line = report; *line; ) {
        const char *e = strchr(line, '\n'); size_t len = e ? (size_t)(e - line) : strlen(line);
        const char *p = line;
        while (p < line + len && (*p == ' ' || *p == '\t' || *p == '*' || *p == '`' || *p == '#')) p++;
        if (line + len - p >= 5 && !strncasecmp(p, "GOAL:", 5)) {
            p = goal_skip_space(p + 5);
            if (line + len - p >= 7 && !strncasecmp(p, "REACHED", 7)) { found = GOAL_REACHED; if (why && n) why[0] = 0; }
            else if (line + len - p >= 10 && !strncasecmp(p, "IMPOSSIBLE", 10)) {
                found = GOAL_IMPOSSIBLE;
                if (why && n) {
                    const char *r = goal_skip_space(p + 10);
                    while (r < line + len && (*r == ':' || *r == '-' || *r == '*' || *r == ' ')) r++;
                    const char *re = line + len;
                    while (re > r && (re[-1] == '*' || re[-1] == '`' || re[-1] == ' ' || re[-1] == '\r')) re--;
                    size_t k = (size_t)(re - r); if (k >= n) k = n - 1;
                    memcpy(why, r, k); why[k] = 0;
                }
            }
        }
        if (!e) break;
        line = e + 1;
    }
    return found;
}

/* A suggested next message, as the prompt shows it (see term_hint_set): the model's reply to the
 * suggestion call, cleaned. The first line only, without the quotes or the "User:" label a model
 * adds, no control byte (it is drawn raw into the field), at most SUGGEST_MAX bytes. NULL when there
 * is nothing to offer: an empty reply, NO_SUGGESTION, or a reply cut off by its token cap (cut).
 * Malloc'd. */
char *suggest_clean(const char *raw, bool cut) {
    if (!raw || cut) return NULL;
    sbuf b; sb_init(&b);
    for (const char *p = raw; *p && *p != '\n' && *p != '\r'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) continue;
        sb_putc(&b, (char)c);
    }
    const char *s = b.data; size_t n = b.len;
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= 5 && !strncasecmp(s, "user:", 5)) { s += 5; n -= 5; while (n && isspace((unsigned char)*s)) { s++; n--; } }
    while (n >= 2 && strchr("\"'`", s[0]) && s[n - 1] == s[0]) { s++; n -= 2; }
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    char *out = NULL;
    if (n) {
        char *line = xstrndup(s, n);
        if (!strstr(line, "NO_SUGGESTION")) out = xstrndup(line, utf8_cut(line, n, SUGGEST_MAX));
        free(line);
    }
    sb_free(&b);
    return out;
}
