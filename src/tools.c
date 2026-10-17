/* Agent tools: read_file, write_file, edit_file, list_dir, grep, bash, web_search, web_fetch, task (sub-agent).
 * Mutating / shell / network tools ask the user for confirmation depending on the permission mode. */
#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>

#define CONF_EDIT_K 0
#define CONF_BASH_K 1
#define CONF_FETCH_K 2
#define CONF_MCP_K 3
/* Everything a tool returns lands in the model's context and is re-sent on every following
 * round, so the caps are deliberately modest; the model is told to page with offset/limit. */
#define READ_FILE_MAX (4 * 1024 * 1024)  /* bytes of a file we look at (offset/limit page inside this) */
#define READ_LINES 2000                  /* default limit for read_file */
#define READ_CAP   (64 * 1024)           /* max bytes one read_file call returns */
#define OUT_CAP    (32 * 1024)           /* max bytes of bash/grep output fed back */
#define WEB_CAP    (24 * 1024)           /* max bytes one web_fetch call returns (it pages with offset) */
#define WEB_MAX    (2u * 1024 * 1024)    /* refuse to download more of a page than this */
#define MAX_ENTRIES 400

static bool g_always_write = false, g_always_edit = false, g_always_bash = false, g_always_fetch = false, g_always_mcp = false;
bool tools_no_confirm = false;   /* set by the caller for user-typed "!cmd": no prompt, no plan-mode veto */
void tools_reset_permissions(void) { g_always_write = g_always_edit = g_always_bash = g_always_fetch = g_always_mcp = false; }
const char *tools_summary_line(void) {
    static char b[160];
    snprintf(b, sizeof b, "read_file, write_file, edit_file, list_dir, grep, bash, bash_job, todo_write, %stask%s", g_cfg.web ? "web_search, web_fetch, " : "", g_cfg.advisor ? ", advisor" : "");
    return b;
}

/* ---------- persistent project permissions ----------
 * The config directory's permissions/ file for this project (perm_path()) holds one rule per line. Allow rules:
 * "edit" (file writes/edits are fine in this project), "bash <words>" (shell commands whose leading words match, e.g.
 * "bash git status", "bash make") or "fetch <host>" (web_fetch may read pages from that host, e.g.
 * "fetch www.postgresql.org"). Commands with shell metacharacters (; | & $ ` > < newline) never match an allow
 * rule — a chained command is not "just git status". Guard rules: "deny <tool> <pattern>" and "ask <tool> <pattern>",
 * tool bash, edit or fetch, the pattern a glob (* and ** match any text, ? one character) matched against the command
 * (or any of its parts), the path, or the host (or the URL, when the pattern has a /). deny stops the tool in every
 * mode; ask prompts in every mode. deny beats ask beats allow. They are guardrails for the model, not a sandbox. */
#define PERM_MAX  200
static char *g_perm[PERM_MAX]; static int g_perm_n = 0;

/* Where this project's rules are kept: in the user's config directory, under a name made from the
 * project's path. A file in the project itself cannot grant anything — a cloned repository could
 * carry one that says "edit" or "bash git" — so the rules are never read from the working tree. */
static const char *perm_path(void) {
    static char path[1200];
    char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    unsigned long long h = 1469598103934665603ULL;   /* FNV-1a, 64 bit */
    for (const char *p = cwd; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    snprintf(path, sizeof path, "%s/permissions/%016llx.rules", config_dir(), h);
    return path;
}

void tools_permissions_load(void) {
    for (int i = 0; i < g_perm_n; i++) free(g_perm[i]);
    g_perm_n = 0;
    if (access(".corbienest/permissions", F_OK) == 0)
        printf(C_DIM "  ⚠ .corbienest/permissions in this project is not read: a project cannot grant permissions (a cloned one could). Add its rules again with /permissions." C_RESET "\n");
    size_t n; char *d = read_whole_file(perm_path(), &n, 64 * 1024);
    if (!d) return;
    char *save = NULL;
    for (char *l = strtok_r(d, "\n", &save); l && g_perm_n < PERM_MAX; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ' || *l == '\t') l++;
        size_t k = strlen(l); while (k && (l[k-1] == ' ' || l[k-1] == '\r' || l[k-1] == '\t')) l[--k] = 0;
        if (!*l || *l == '#') continue;
        g_perm[g_perm_n++] = xstrdup(l);
    }
    free(d);
}
static void perm_save(void) {
    if (!g_perm_n) { unlink(perm_path()); return; }
    sbuf b; sb_init(&b);
    char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    sb_printf(&b, "# corbienest project permissions for %s — one rule per line: \"edit\", \"bash <leading words>\", \"fetch <host>\", or \"deny|ask bash|edit|fetch <pattern>\" (see /permissions)\n", cwd);
    for (int i = 0; i < g_perm_n; i++) sb_printf(&b, "%s\n", g_perm[i]);
    char dir[1100]; snprintf(dir, sizeof dir, "%s/permissions", config_dir());
    mkdir_p(dir);
    write_whole_file_atomic(perm_path(), b.data, b.len);
    sb_free(&b);
}
int tools_permissions_count(void) { return g_perm_n; }
const char *tools_permissions_get(int i) { return i >= 0 && i < g_perm_n ? g_perm[i] : NULL; }
bool tools_permissions_add(const char *rule) {
    for (int i = 0; i < g_perm_n; i++) if (!strcmp(g_perm[i], rule)) return false;
    if (g_perm_n >= PERM_MAX) { free(g_perm[0]); memmove(g_perm, g_perm + 1, sizeof(char*) * (PERM_MAX - 1)); g_perm_n--; }
    g_perm[g_perm_n++] = xstrdup(rule);
    perm_save();
    return true;
}
bool tools_permissions_remove(int i) {
    if (i < 0 || i >= g_perm_n) return false;
    free(g_perm[i]); memmove(g_perm + i, g_perm + i + 1, sizeof(char*) * (size_t)(g_perm_n - i - 1)); g_perm_n--;
    perm_save();
    return true;
}
void tools_permissions_clear(void) { for (int i = 0; i < g_perm_n; i++) free(g_perm[i]); g_perm_n = 0; unlink(perm_path()); }

/* Rule text a command would be remembered under: its first word, plus the subcommand for
 * tools that have them (git, npm, cargo, docker …). Empty if the command is not "simple". */
static void bash_rule_for(const char *cmd, char *out, size_t n) {
    out[0] = 0;
    if (strpbrk(cmd, ";|&$`<>\n")) return;
    static const char *sub[] = { "git", "npm", "npx", "pnpm", "yarn", "cargo", "go", "docker", "kubectl", "gh", "pip", "pip3", "python", "python3", "uv", "poetry", NULL };
    const char *p = cmd; while (*p == ' ') p++;
    const char *e = p; while (*e && *e != ' ') e++;
    if (e == p) return;
    size_t l = (size_t)(e - p);
    snprintf(out, n, "bash %.*s", (int)l, p);
    for (int i = 0; sub[i]; i++) if (strlen(sub[i]) == l && !strncmp(sub[i], p, l)) {
        const char *q = e; while (*q == ' ') q++;
        const char *qe = q; while (*qe && *qe != ' ') qe++;
        if (qe > q && *q != '-') { size_t used = strlen(out); snprintf(out + used, n - used, " %.*s", (int)(qe - q), q); }
        break;
    }
}
/* does a saved rule cover this command? (rule words are a prefix of the command's words) */
static bool bash_rule_matches(const char *rule, const char *cmd) {
    if (strncmp(rule, "bash ", 5)) return false;
    if (strpbrk(cmd, ";|&$`<>\n")) return false;
    const char *r = rule + 5, *c = cmd;
    while (*c == ' ') c++;
    for (;;) {
        while (*r == ' ') r++;
        if (!*r) return true;
        const char *re = r; while (*re && *re != ' ') re++;
        while (*c == ' ') c++;
        const char *ce = c; while (*ce && *ce != ' ') ce++;
        if (ce - c != re - r || strncmp(r, c, (size_t)(re - r))) return false;
        r = re; c = ce;
    }
}
/* Rule text a URL is remembered under: its host, so approving one page of a manual approves
 * the manual. Empty if the URL has no host we can name. */
static void fetch_rule_for(const char *url, char *out, size_t n) {
    char host[512];
    out[0] = 0;
    if (!url_host(url, host, sizeof host)) return;
    if (strlen(host) + 7 > n) return;   /* a truncated host would match the wrong thing: offer no rule */
    snprintf(out, n, "fetch %s", host);
}
/* Exact host match — no wildcards: "always allow docs.example.org" should not quietly mean
 * every other name under example.org. */
static bool fetch_rule_matches(const char *rule, const char *url) {
    if (strncmp(rule, "fetch ", 6)) return false;
    char host[512];
    if (!url_host(url, host, sizeof host)) return false;
    return !strcmp(rule + 6, host);
}

/* glob: * and ** match any text (a / too), ? one character; the whole of s has to match. */
bool glob_match(const char *p, const char *s) {
    const char *star = NULL, *mark = NULL;   /* the last * in p, and the point in s it has taken up to */
    while (*s) {
        if (*p == '*') { while (*p == '*') p++; star = p; mark = s; continue; }
        if (*p && (*p == '?' || *p == *s)) { p++; s++; continue; }
        if (!star) return false;
        p = star; s = ++mark;   /* the last * takes one character more */
    }
    while (*p == '*') p++;
    return !*p;
}

/* A bash deny/ask pattern against a command: the whole command, or any one of its parts. The parts are split at ; | &
 * and newlines (so "cd x && rm y" has an "rm y" in it), a leading NAME=value is dropped, and a path in front of the
 * program is cut to its name too (/bin/rm is rm). */
static bool bash_match(const char *pat, const char *cmd) {
    if (glob_match(pat, cmd)) return true;
    const char *p = cmd;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        const char *e = p; while (*e && !strchr(";|&\n", *e)) e++;
        const char *w = p;
        for (;;) {   /* leading NAME=value words */
            const char *we = w; while (we < e && *we != ' ' && *we != '\t') we++;
            const char *eq = memchr(w, '=', (size_t)(we - w));
            if (!eq || eq == w || !(isalpha((unsigned char)*w) || *w == '_') || memchr(w, '/', (size_t)(eq - w))) break;
            w = we; while (w < e && (*w == ' ' || *w == '\t')) w++;
        }
        if (w < e) {
            const char *fe = w; while (fe < e && *fe != ' ' && *fe != '\t') fe++;
            const char *base = fe; while (base > w && base[-1] != '/') base--;
            char *raw = xstrndup(w, (size_t)(e - w)), *named = xstrndup(base, (size_t)(e - base));
            for (char *t = raw + strlen(raw); t > raw && (t[-1] == ' ' || t[-1] == '\t'); ) *--t = 0;
            for (char *t = named + strlen(named); t > named && (t[-1] == ' ' || t[-1] == '\t'); ) *--t = 0;
            bool hit = glob_match(pat, raw) || glob_match(pat, named);
            free(raw); free(named);
            if (hit) return true;
        }
        if (!*e) return false;
        p = e + 1;
    }
}

/* What one saved rule says about a target of `tool` ("bash", "edit" or "fetch"); the target is the command, the path
 * (as perm_path_norm() gives it) or the URL. */
int perm_rule_verdict(const char *rule, const char *tool, const char *target) {
    if (!rule || !tool || !target) return VERDICT_NONE;
    bool deny = !strncmp(rule, "deny ", 5), ask = !deny && !strncmp(rule, "ask ", 4);
    if (deny || ask) {
        const char *r = rule + (deny ? 5 : 4), *sp = strchr(r, ' ');
        if (!sp || (size_t)(sp - r) != strlen(tool) || strncmp(r, tool, (size_t)(sp - r))) return VERDICT_NONE;
        const char *pat = sp; while (*pat == ' ') pat++;
        if (!*pat) return VERDICT_NONE;
        bool hit;
        if (!strcmp(tool, "bash")) {
            hit = bash_match(pat, target);
            if (!hit && (strchr(target, '`') || strstr(target, "$("))) return VERDICT_ASK;   /* a subshell the rule cannot see into */
        } else if (!strcmp(tool, "fetch")) {
            if (strchr(pat, '/')) hit = glob_match(pat, target);
            else { char host[512]; hit = url_host(target, host, sizeof host) && glob_match(pat, host); }
        } else hit = glob_match(pat, target);
        return hit ? (deny ? VERDICT_DENY : VERDICT_ASK) : VERDICT_NONE;
    }
    if (!strcmp(tool, "edit")) return !strcmp(rule, "edit") ? VERDICT_ALLOW : VERDICT_NONE;
    if (!strcmp(tool, "mcp")) return !strncmp(rule, "mcp ", 4) && !strcmp(rule + 4, target) ? VERDICT_ALLOW : VERDICT_NONE;
    if (!strcmp(tool, "bash")) return bash_rule_matches(rule, target) ? VERDICT_ALLOW : VERDICT_NONE;
    if (!strcmp(tool, "fetch")) return fetch_rule_matches(rule, target) ? VERDICT_ALLOW : VERDICT_NONE;
    return VERDICT_NONE;
}

/* The verdict of all the project's rules on a target: deny beats ask beats allow. *rule is the rule that decided it. */
static int perm_verdict(int kind, const char *target, const char **rule) {
    const char *tool = kind == CONF_EDIT_K ? "edit" : kind == CONF_BASH_K ? "bash" : kind == CONF_MCP_K ? "mcp" : "fetch";
    int best = VERDICT_NONE;
    *rule = NULL;
    for (int i = 0; i < g_perm_n; i++) {
        int v = perm_rule_verdict(g_perm[i], tool, target ? target : "");
        if (v == VERDICT_DENY) { *rule = g_perm[i]; return v; }
        if (v == VERDICT_ASK && best != VERDICT_ASK) { best = v; *rule = g_perm[i]; }
        else if (v == VERDICT_ALLOW && best == VERDICT_NONE) { best = v; *rule = g_perm[i]; }
    }
    return best;
}

/* The path a rule is matched against: ~ expanded, a .. resolved in the directory part (the file may not exist yet), the
 * working directory and any ./ taken off, so "src/../.env" is ".env" and "/home/x/proj/a" is "a". Malloc'd. */
static char *perm_path_norm(const char *path) {
    char *p = expand_home(path);
    if (strstr(p, "..")) {
        char *slash = strrchr(p, '/');
        const char *base = slash ? slash + 1 : p;
        char dirp[PATH_MAX], rp[PATH_MAX];
        if (!slash) snprintf(dirp, sizeof dirp, ".");
        else if (slash == p) snprintf(dirp, sizeof dirp, "/");
        else snprintf(dirp, sizeof dirp, "%.*s", (int)(slash - p), p);
        if (realpath(dirp, rp)) {
            char *n = xmalloc(strlen(rp) + strlen(base) + 2);
            sprintf(n, "%s/%s", !strcmp(rp, "/") ? "" : rp, base);
            free(p); p = n;
        }
    }
    char cwd[PATH_MAX];
    if (p[0] == '/' && getcwd(cwd, sizeof cwd) && !strncmp(p, cwd, strlen(cwd)) && p[strlen(cwd)] == '/') {
        char *n = xstrdup(p + strlen(cwd) + 1); free(p); p = n;
    }
    while (p[0] == '.' && p[1] == '/') { char *n = xstrdup(p + 2); free(p); p = n; }
    return p;
}

/* ---------- checkpoints (for /rewind) ----------
 * Before write_file/edit_file touch a file, its previous content (or "did not exist") is
 * kept together with the request ("turn") it happened in. tools_checkpoint_restore(turn)
 * puts every file changed in that request or later back the way it was — newest change
 * first, so a file edited several times ends up at its oldest saved state. In memory
 * only (per process), capped at CKPT_MAX_BYTES. */
#define CKPT_MAX_BYTES (64u * 1024 * 1024)
typedef struct { int turn; char *path; char *content; size_t len; bool existed; int step; } ckpt_t;
static ckpt_t *g_ckpt; static int g_ckpt_n, g_ckpt_cap; static size_t g_ckpt_bytes; static int g_ckpt_turn = -1;
static int g_ckpt_step = 0;   /* a part of the request that is looked at by itself (a task of /orchestrate); 0 = none */

void tools_checkpoint_turn(int turn) { g_ckpt_turn = turn; g_ckpt_step = 0; }
/* /goal: while a candidate's tool call runs, the working directory is its worktree and writes must stay in it */
static const char *g_tool_root = NULL;
static int g_tool_saved = -1;
bool tools_enter(const char *root) {
    if (g_tool_saved >= 0 || !root) return false;
    int fd = open(".", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    if (chdir(root) != 0) { close(fd); return false; }
    g_tool_saved = fd; g_tool_root = root;
    return true;
}
void tools_leave(void) {
    if (g_tool_saved < 0) return;
    if (fchdir(g_tool_saved) != 0) fprintf(stderr, "corbienest: cannot return to the working directory: %s\n", strerror(errno));
    close(g_tool_saved); g_tool_saved = -1; g_tool_root = NULL;
}
/* p is as a tool was given it, ~ expanded, and is taken from the current directory: inside the root? A path that does
 * not exist yet is checked by its folder. Symlinks are resolved, so one out of the worktree does not pass. */
static bool path_in_root(const char *p) {
    if (!g_tool_root) return true;
    char root[PATH_MAX], full[PATH_MAX];
    if (!realpath(g_tool_root, root)) return false;
    if (!realpath(p, full)) {
        const char *slash = strrchr(p, '/'), *name = slash ? slash + 1 : p;
        if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
        char dir[PATH_MAX];
        if (!slash) snprintf(dir, sizeof dir, ".");
        else if (slash == p) snprintf(dir, sizeof dir, "/");
        else if ((size_t)(slash - p) < sizeof dir) snprintf(dir, sizeof dir, "%.*s", (int)(slash - p), p);
        else return false;
        char real[PATH_MAX];
        if (!realpath(dir, real)) return false;
        size_t rl = strlen(real), nl = strlen(name);
        if (rl + nl + 2 > sizeof full) return false;
        memcpy(full, real, rl); full[rl] = '/'; memcpy(full + rl + 1, name, nl + 1);
    }
    size_t n = strlen(root);
    return !strncmp(full, root, n) && (full[n] == '/' || full[n] == 0);
}
void tools_checkpoint_step(int step) { g_ckpt_step = step; }
static void ckpt_free(ckpt_t *c) { free(c->path); free(c->content); g_ckpt_bytes -= c->len; }
void tools_checkpoint_clear(void) { for (int i = 0; i < g_ckpt_n; i++) ckpt_free(&g_ckpt[i]); g_ckpt_n = 0; }
static void ckpt_save(const char *path) {
    if (g_ckpt_turn < 0 || g_tool_root) return;   /* a candidate's files are not the user's (see tools_enter) */
    for (int i = 0; i < g_ckpt_n; i++) if (g_ckpt[i].turn == g_ckpt_turn && g_ckpt[i].step == g_ckpt_step && !strcmp(g_ckpt[i].path, path)) return;   /* first state in this turn (and step) wins */
    ckpt_t c = { g_ckpt_turn, xstrdup(path), NULL, 0, false, g_ckpt_step };
    if (is_file(path)) { c.content = read_whole_file(path, &c.len, CKPT_MAX_BYTES); if (!c.content) { free(c.path); return; } c.existed = true; }
    while (g_ckpt_n && g_ckpt_bytes + c.len > CKPT_MAX_BYTES) { ckpt_free(&g_ckpt[0]); memmove(g_ckpt, g_ckpt + 1, sizeof *g_ckpt * (size_t)(g_ckpt_n - 1)); g_ckpt_n--; }
    if (g_ckpt_n == g_ckpt_cap) { g_ckpt_cap = g_ckpt_cap ? g_ckpt_cap * 2 : 32; g_ckpt = xrealloc(g_ckpt, sizeof *g_ckpt * (size_t)g_ckpt_cap); }
    g_ckpt[g_ckpt_n++] = c; g_ckpt_bytes += c.len;
}
void tools_checkpoint_path(const char *path) { ckpt_save(path); }
/* files changed in `turn` or later: distinct paths, appended comma-separated to `names` (may be NULL); returns the count */
int tools_checkpoint_files(int turn, sbuf *names) {
    int n = 0;
    for (int i = 0; i < g_ckpt_n; i++) {
        if (g_ckpt[i].turn < turn) continue;
        bool seen = false; for (int j = 0; j < i; j++) if (g_ckpt[j].turn >= turn && !strcmp(g_ckpt[j].path, g_ckpt[i].path)) { seen = true; break; }
        if (seen) continue;
        if (names) sb_printf(names, "%s%s", n ? ", " : "", g_ckpt[i].path);
        n++;
    }
    return n;
}
/* What step `step` of request `turn` did to the files it wrote: a unified diff of each, from
 * the state saved before its first write to what is on disk now, appended to `out`. Returns the
 * number of files that differ. diff(1) does the comparing — it is on every POSIX system, and
 * writing the saved state to a temporary file for it is all it takes. */
/* One checkpoint against the file as it is now: the unified diff appended to `out`; 1 if the file differs. */
static int ckpt_diff1(const ckpt_t *c, sbuf *out) {
    const char *tmpdir = getenv("TMPDIR");
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s/corbienest_was_XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    int fd = mkstemp(tmp);
    if (fd < 0) return 0;
    size_t off = 0;
    while (c->existed && off < c->len) { ssize_t w = write(fd, c->content + off, c->len - off); if (w <= 0) break; off += (size_t)w; }
    close(fd);
    char *qold = shell_escape(tmp), *qnew = shell_escape(is_file(c->path) ? c->path : "/dev/null"), *qname = shell_escape(c->path);
    sbuf cmd; sb_init(&cmd);
    sb_printf(&cmd, "diff -u -L a/%s -L b/%s %s %s 2>&1", qname, qname, qold, qnew);
    FILE *p = popen(cmd.data, "r");
    size_t before = out->len;
    if (p) { char buf[4096]; size_t k; while ((k = fread(buf, 1, sizeof buf, p)) > 0) sb_append(out, buf, k); pclose(p); }
    int differ = 0;
    if (out->len > before) { differ = 1; if (out->data[out->len - 1] != '\n') sb_putc(out, '\n'); }
    sb_free(&cmd); free(qold); free(qnew); free(qname);
    unlink(tmp);
    return differ;
}

int tools_checkpoint_diff(int turn, int step, sbuf *out) {
    int n = 0;
    for (int i = 0; i < g_ckpt_n; i++) {
        ckpt_t *c = &g_ckpt[i];
        if (c->turn != turn || c->step != step) continue;
        n += ckpt_diff1(c, out);
    }
    return n;
}

/* What the request that began at `turn` changed on disk: for each file it wrote, the diff from its state before the
 * request's first write to what is there now, so an edit that was undone counts as nothing. Returns the files that
 * differ; *lines is the number of changed lines over all of them. */
int tools_request_changes(int turn, int *lines) {
    int n = 0; *lines = 0;
    for (int i = 0; i < g_ckpt_n; i++) {
        if (g_ckpt[i].turn < turn) continue;
        bool seen = false; for (int j = 0; j < i; j++) if (g_ckpt[j].turn >= turn && !strcmp(g_ckpt[j].path, g_ckpt[i].path)) { seen = true; break; }
        if (seen) continue;
        sbuf d; sb_init(&d);
        n += ckpt_diff1(&g_ckpt[i], &d);
        *lines += diff_changed_lines(d.data);
        sb_free(&d);
    }
    return n;
}

int tools_checkpoint_restore(int turn) {
    int n = 0;
    for (int i = g_ckpt_n - 1; i >= 0; i--) {
        ckpt_t *c = &g_ckpt[i];
        if (c->turn < turn) continue;
        if (c->existed) write_whole_file(c->path, c->content, c->len); else unlink(c->path);
        n++;
    }
    /* drop the used records (they are ≥ turn; all such sit at the tail because turns only grow) */
    int keep = 0; for (int i = 0; i < g_ckpt_n; i++) { if (g_ckpt[i].turn >= turn) ckpt_free(&g_ckpt[i]); else g_ckpt[keep++] = g_ckpt[i]; }
    g_ckpt_n = keep;
    return n;
}

/* ---------- tool definitions ---------- */
static cJSON *mk_tool(const char *name, const char *desc, cJSON *props, const char *required[]) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "function");
    cJSON *f = cJSON_AddObjectToObject(t, "function");
    cJSON_AddStringToObject(f, "name", name);
    cJSON_AddStringToObject(f, "description", desc);
    cJSON *p = cJSON_AddObjectToObject(f, "parameters");
    cJSON_AddStringToObject(p, "type", "object");
    cJSON_AddItemToObject(p, "properties", props);
    cJSON *req = cJSON_AddArrayToObject(p, "required");
    for (int i = 0; required[i]; i++) cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
    return t;
}
static cJSON *prop(cJSON *props, const char *name, const char *type, const char *desc) {
    cJSON *o = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "description", desc);
    return o;
}

cJSON *tools_definitions(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *p;

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Path to the file (relative to the working directory or absolute).");
    prop(p, "offset", "integer", "Optional 1-based line number to start reading from.");
    prop(p, "limit", "integer", "Optional maximum number of lines to return.");
    cJSON_AddItemToArray(arr, mk_tool("read_file",
        "Read a text file. Returns the content with line numbers; by default the first 2000 lines (at most 64 KB). Use offset/limit to page through larger files, and read only what you need.",
        p, (const char*[]){"path", NULL}));

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Path of the file to create or overwrite.");
    prop(p, "content", "string", "The full new content of the file.");
    cJSON_AddItemToArray(arr, mk_tool("write_file",
        "Create a new file or completely overwrite an existing file with the given content. Parent directories are created automatically. Prefer edit_file for small changes to existing files.",
        p, (const char*[]){"path", "content", NULL}));

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Path of the file to edit.");
    prop(p, "old_string", "string", "Exact text to find (must match exactly, including whitespace/indentation, and be unique in the file unless replace_all is true).");
    prop(p, "new_string", "string", "Replacement text.");
    prop(p, "replace_all", "boolean", "Replace every occurrence instead of requiring uniqueness. Default false.");
    cJSON_AddItemToArray(arr, mk_tool("edit_file",
        "Edit an existing file by replacing an exact string with a new string. Read the file first so old_string matches exactly.",
        p, (const char*[]){"path", "old_string", "new_string", NULL}));

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Directory to list. Defaults to the current working directory.");
    cJSON_AddItemToArray(arr, mk_tool("list_dir",
        "List files and subdirectories in a directory (non-recursive).",
        p, (const char*[]){NULL}));

    p = cJSON_CreateObject();
    prop(p, "pattern", "string", "Regular expression (grep -E syntax) to search for.");
    prop(p, "path", "string", "File or directory to search recursively. Defaults to the current working directory.");
    prop(p, "include", "string", "Optional glob to restrict files, e.g. \"*.c\" or \"*.py\".");
    cJSON_AddItemToArray(arr, mk_tool("grep",
        "Search file contents recursively with a regular expression. Returns matching lines as path:line:text (at most 300 lines / 32 KB, so narrow the pattern, path or include glob rather than grepping broadly).",
        p, (const char*[]){"pattern", NULL}));

    p = cJSON_CreateObject();
    prop(p, "command", "string", "The shell command to run (executed with /bin/sh -c). Use it for git, build tools, tests, package managers, find, etc.");
    prop(p, "timeout", "integer", "Optional timeout in seconds (default 120, max 600). Not used for a background command.");
    prop(p, "run_in_background", "boolean", "Start the command and return at once, for a long-running one (a dev server, a watch). Read it with bash_job.");
    cJSON_AddItemToArray(arr, mk_tool("bash",
        "Run a shell command in the working directory and return its combined stdout/stderr and exit code. Use for git operations, running builds/tests, installing packages, and anything the other tools do not cover. Avoid interactive commands. Output is capped at 32 KB (head and tail are kept), so filter or paginate noisy commands (| tail, | head, -q flags).",
        p, (const char*[]){"command", NULL}));

    if (g_cfg.web) {
        p = cJSON_CreateObject();
        prop(p, "url", "string", "Absolute http:// or https:// URL.");
        prop(p, "offset", "integer", "1-based line of the text to start at (for paging). Default 1.");
        prop(p, "timeout", "integer", "Seconds (default 30, max 120).");
        cJSON_AddItemToArray(arr, mk_tool("web_fetch",
            "Read a web page as text: markup, scripts and styling stripped, code samples kept. At most 24 KB per call — the page is paged, not truncated, so use offset for the rest.",
            p, (const char*[]){"url", NULL}));

        p = cJSON_CreateObject();
        prop(p, "query", "string", "A few precise words: the project, its version, and the exact symbol, option or error.");
        prop(p, "max_results", "integer", "Default 8, max 20.");
        cJSON_AddItemToArray(arr, mk_tool("web_search",
            "Find documentation when you do not know its URL. Returns title, URL and snippet per result; the snippets are not the docs — web_fetch the best one, preferring the project's own domain over blogs and Q&A sites.",
            p, (const char*[]){"query", NULL}));
    }

    p = cJSON_CreateObject();
    prop(p, "job", "string", "The job's id, as bash printed it (e.g. bg1).");
    cJSON *ap = prop(p, "action", "string", "output: what it printed since the last read, and whether it runs; kill: stop it.");
    cJSON *ae = cJSON_AddArrayToObject(ap, "enum");
    cJSON_AddItemToArray(ae, cJSON_CreateString("output")); cJSON_AddItemToArray(ae, cJSON_CreateString("kill"));
    cJSON_AddItemToArray(arr, mk_tool("bash_job",
        "Read the output of a background command that bash started (action output), or stop it (action kill). Each read gives what was printed since the last one.",
        p, (const char*[]){"job", "action", NULL}));

    p = cJSON_CreateObject();
    cJSON *ts = cJSON_AddObjectToObject(p, "todos");
    cJSON_AddStringToObject(ts, "type", "array");
    cJSON_AddStringToObject(ts, "description", "The whole task list, in order: every task, not only the changed ones. An empty array clears it.");
    cJSON *it = cJSON_AddObjectToObject(ts, "items");
    cJSON_AddStringToObject(it, "type", "object");
    cJSON *ip = cJSON_AddObjectToObject(it, "properties");
    prop(ip, "content", "string", "What the task is, in a few words.");
    cJSON *sp = prop(ip, "status", "string", "pending, in_progress (at most one) or completed.");
    cJSON *en = cJSON_AddArrayToObject(sp, "enum");
    cJSON_AddItemToArray(en, cJSON_CreateString("pending")); cJSON_AddItemToArray(en, cJSON_CreateString("in_progress")); cJSON_AddItemToArray(en, cJSON_CreateString("completed"));
    cJSON *rq = cJSON_AddArrayToObject(it, "required");
    cJSON_AddItemToArray(rq, cJSON_CreateString("content")); cJSON_AddItemToArray(rq, cJSON_CreateString("status"));
    cJSON_AddItemToArray(arr, mk_tool("todo_write",
        "Keep a visible task list for the work of this request. Send the whole list each time, with each task's status updated. Use it for work of three or more steps; skip it for a single change or a question.",
        p, (const char*[]){"todos", NULL}));

    p = cJSON_CreateObject();
    cJSON_AddItemToArray(arr, mk_tool("exit",
        "End the program once you have replied. Call it only when the user has said they are finished and want to leave (\"exit\", \"bye\", \"that's all\"): reply with a short goodbye first, in the same turn. Never call it to end a task.",
        p, (const char*[]){NULL}));

    p = cJSON_CreateObject();
    prop(p, "description", "string", "A short (3-6 word) label for what the sub-agent does, shown to the user.");
    prop(p, "agent", "string", "The name of a sub-agent defined for this project (listed in the system prompt): it runs with its own role and tools. Omit for a general sub-agent.");
    prop(p, "prompt", "string", "The complete task for the sub-agent. It starts with a fresh context and sees nothing of this conversation, so include every relevant detail: what to look for, where, and exactly what to report back.");
    cJSON_AddItemToArray(arr, mk_tool("task",
        "Delegate a self-contained research or exploration task to a sub-agent with its own fresh context. It can read files, list directories, grep and run read-only shell commands, but cannot modify files, and returns a text report. "
        "Use it for broad searches or investigations whose intermediate output would clutter your context (\"find every place X is handled and summarise\", \"figure out how the build works\"), not for simple one-file lookups.",
        p, (const char*[]){"description", "prompt", NULL}));

    if (g_cfg.advisor) {   /* only with /advisor MODEL: a tool that always fails is worse than none */
        p = cJSON_CreateObject();
        prop(p, "question", "string", "Optional. What you most want to know, in a sentence or two: your plan, the decision you face, or the error you cannot explain — and what you already tried. The advisor reads the whole conversation by itself, so do not repeat it.");
        cJSON_AddItemToArray(arr, mk_tool("advisor",
            "Consult the advisor: a stronger but much slower model that is shown this whole conversation — the request, every tool call you made and every result — and tells you how to proceed. It has no tools and cannot read files, so read the relevant code before you ask. "
            "Use it for the hard parts: before you commit to an approach for a non-trivial change (explore first, then ask, then edit), when an error has survived two attempts to fix it or a result makes no sense, and before you call a difficult task done. "
            "Do not use it for what a tool call can tell you.",   /* how often it may is in the system prompt: the tool list stays the same whatever the guidance */
            p, (const char*[]){NULL}));
    }
    return arr;
}

/* ---------- helpers ---------- */
static const char *jstr(cJSON *o, const char *k, const char *dflt) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    if (cJSON_IsNumber(v)) { static char nb[64]; snprintf(nb, sizeof nb, "%g", v->valuedouble); return nb; }
    return dflt;
}
static int jint(cJSON *o, const char *k, int dflt) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (int)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && isdigit((unsigned char)v->valuestring[0])) return atoi(v->valuestring);
    return dflt;
}
static bool jbool(cJSON *o, const char *k, bool dflt) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring) return !strcasecmp(v->valuestring, "true");
    return dflt;
}

/* Truncate a big output keeping head and tail. */
static void cap_output(sbuf *b, size_t cap) {
    if (b->len <= cap) return;
    size_t head = utf8_cut(b->data, b->len, cap * 2 / 3), tail = cap - cap * 2 / 3;
    const char *t = b->data + b->len - tail;   /* the tail starts on a character too */
    while (tail && ((unsigned char)*t & 0xC0) == 0x80) { t++; tail--; }
    sbuf n; sb_init(&n);
    sb_append(&n, b->data, head);
    sb_printf(&n, "\n\n[... output truncated: %zu bytes omitted ...]\n\n", b->len - head - tail);
    sb_append(&n, t, tail);
    sb_free(b);
    *b = n;
}

/* Print a few lines of text with a prefix, coloured, for previews. `lang` (a file's extension, or NULL) colours
 * the code in it, as the replies' code blocks are; a line too long to fit is shown plain, as it always was. */
static void preview_lines(const char *text, int max_lines, const char *color, const char *lang) {
    int lines = 0, total = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') total++;
    if (*text && text[strlen(text)-1] != '\n') total++;
    const char *p = text;
    size_t w = (size_t)term_width(); w = w > 8 ? w - 4 : 4;   /* 2 indent + room for the ellipsis */
    while (*p && lines < max_lines) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (lang && n <= w) {
            sbuf h; sb_init(&h);
            highlight_line(lang, p, &h, color);   /* the line as it is, with the code's colours */
            printf("  %s%s" C_RESET "\n", color, h.data ? h.data : "");
            sb_free(&h);
        } else printf("  %s%.*s%s" C_RESET "\n", color, (int)(n > w ? w : n), p, n > w ? "…" : "");
        lines++;
        if (!e) break;
        p = e + 1;
    }
    if (total > lines) printf("  " C_DIM "… (%d more lines)" C_RESET "\n", total - lines);
}

enum { CONF_EDIT = CONF_EDIT_K, CONF_BASH = CONF_BASH_K, CONF_FETCH = CONF_FETCH_K, CONF_MCP = CONF_MCP_K };
/* Would confirm() ask? The same early returns, without the output: a command that will be asked
 * about is never folded away (the user approves what they can see). Keep in step with confirm_body(). */
static bool confirm_will_ask(int kind, bool *always_flag, const char *cmd) {
    if (tools_no_confirm) return false;
    const char *rule;
    int v = perm_verdict(kind, cmd, &rule);
    if (v == VERDICT_DENY) return false;
    if (v == VERDICT_ASK) return g_cfg.interactive;
    if (always_flag && *always_flag) return false;
    if (YOLO() || (kind == CONF_EDIT && g_cfg.mode == MODE_ACCEPT_EDITS)) return false;
    if (kind == CONF_EDIT && g_cfg.mode == MODE_PLAN) return false;
    if (v == VERDICT_ALLOW) return false;
    return g_cfg.interactive;
}
/* Confirmation prompt. Returns 1 allow, 0 deny. On deny, *reason may be set (malloc'd).
 * kind: CONF_EDIT for write/edit, CONF_BASH for shell commands, CONF_FETCH for web_fetch;
 * `cmd` is the shell command or the URL, or for an edit the path (matched against the project's rules). */
static int confirm_body(const char *what, int kind, bool *always_flag, const char *target, char **reason) {
    *reason = NULL;
    if (tools_no_confirm) return 1;   /* the user's own !cmd: the rules guard the model, not the user */
    const char *rule;
    int v = perm_verdict(kind, target, &rule);
    if (v == VERDICT_DENY) {   /* in every mode, and whatever was approved before */
        char msg[200]; snprintf(msg, sizeof msg, "%s", rule);
        printf("  " C_YELLOW "%s" C_RESET " " C_RED "denied (project rule: %s)" C_RESET "\n", what, msg);
        char r[480]; snprintf(r, sizeof r, "blocked by a project rule (%s): do not retry this action or a variant of it; do the job another way, or say what is needed.", msg);
        *reason = xstrdup(r);
        return 0;
    }
    if (v == VERDICT_ASK) {   /* asked in every mode, and never remembered: the rule would win over any saved allow */
        if (!g_cfg.interactive) {
            printf("  " C_YELLOW "%s" C_RESET " " C_RED "denied (project rule asks, and there is no one to ask)" C_RESET "\n", what);
            char r[480]; snprintf(r, sizeof r, "a project rule (%.200s) asks the user before this, and no one can be asked now. Do not do it; say what is needed.", rule);
            *reason = xstrdup(r);
            return 0;
        }
        term_confirm_quiet = kind == CONF_BASH;
        int r = term_confirm(what, "Yes, this once (a project rule asks each time)", NULL, reason);
        term_confirm_quiet = false;
        return r > 0;
    }
    if (always_flag && *always_flag) return 1;
    if (YOLO() || (kind == CONF_EDIT && g_cfg.mode == MODE_ACCEPT_EDITS)) {
        if (g_cfg.interactive && kind != CONF_BASH) printf("  " C_YELLOW "%s" C_RESET "  " C_DIM "auto-approved (%s mode)" C_RESET "\n", what, mode_name(g_cfg.mode));
        return 1;
    }
    if (kind == CONF_EDIT && g_cfg.mode == MODE_PLAN) {
        printf("  " C_YELLOW "%s" C_RESET " " C_RED "denied (plan mode is read-only)" C_RESET "\n", what);
        *reason = xstrdup("corbienest is in plan mode (read-only): do not modify files. Present your plan as text; the user will switch modes (shift+tab) when they want it implemented.");
        return 0;
    }
    if (v == VERDICT_ALLOW) {
        if (kind != CONF_BASH) printf("  " C_YELLOW "%s" C_RESET "  " C_DIM "auto-approved (project rule: %s)" C_RESET "\n", what, rule);
        return 1;
    }
    if (!g_cfg.interactive) {
        printf("  " C_YELLOW "%s" C_RESET " " C_RED "denied (non-interactive; use --yolo or --mode to allow)" C_RESET "\n", what);
        *reason = xstrdup("corbienest is running non-interactively and cannot ask for confirmation. Continue without this action.");
        return 0;
    }
    const char *always = kind == CONF_BASH  ? "Yes, and don't ask again for shell commands this session"
                       : kind == CONF_MCP ? "Yes, and don't ask again for this server's tools this session"
                       : kind == CONF_FETCH ? "Yes, and don't ask again for web fetches this session"
                                            : "Yes, and don't ask again for file edits this session";
    char newrule[256] = "", plabel[320] = "";
    if (kind == CONF_BASH) { bash_rule_for(target, newrule, sizeof newrule); if (newrule[0]) snprintf(plabel, sizeof plabel, "Yes, and always allow `%s …` in this project", newrule + 5); }
    else if (kind == CONF_MCP) { snprintf(newrule, sizeof newrule, "mcp %s", target); snprintf(plabel, sizeof plabel, "Yes, and always allow %s in this project", target); }
    else if (kind == CONF_FETCH) { fetch_rule_for(target, newrule, sizeof newrule); if (newrule[0]) snprintf(plabel, sizeof plabel, "Yes, and always allow fetching from %s in this project", newrule + 6); }
    else { snprintf(newrule, sizeof newrule, "edit"); snprintf(plabel, sizeof plabel, "Yes, and always allow file edits in this project"); }
    term_confirm_quiet = kind == CONF_BASH;
    int r = term_confirm(what, always, newrule[0] ? plabel : NULL, reason);
    term_confirm_quiet = false;
    if (r == 2 && always_flag) *always_flag = true;
    if (r == 3 && newrule[0]) { tools_permissions_add(newrule); printf("  " C_DIM "saved to %s: %s" C_RESET "\n", perm_path(), newrule); }
    return r > 0;
}
static int confirm(const char *what, int kind, bool *always_flag, const char *cmd, char **reason) {
    char *np = kind == CONF_EDIT && cmd ? perm_path_norm(cmd) : NULL;
    int r = confirm_body(what, kind, always_flag, np ? np : cmd, reason);
    free(np);
    return r;
}

/* ---------- read_file ---------- */
static tool_status t_read_file(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", NULL);
    if (!path) { sb_puts(out, "error: missing 'path'"); return TOOL_ERROR; }
    char *p = expand_home(path);
    if (is_dir(p)) { sb_printf(out, "error: %s is a directory (use list_dir)", path); free(p); return TOOL_ERROR; }
    size_t len = 0;
    char *d = read_whole_file(p, &len, READ_FILE_MAX);
    if (!d) { sb_printf(out, "error: cannot read %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    /* binary check */
    for (size_t i = 0; i < len && i < 4096; i++) if (d[i] == 0) { sb_printf(out, "error: %s appears to be a binary file", path); free(d); free(p); return TOOL_ERROR; }
    int offset = jint(args, "offset", 1), limit = jint(args, "limit", 0);
    if (offset < 1) offset = 1;
    bool explicit_limit = limit > 0;
    if (!explicit_limit) limit = READ_LINES;
    int lineno = 0, emitted = 0;
    bool capped = false;   /* stopped early: default line limit or byte cap reached with more of the file left */
    size_t start = out->len;
    const char *s = d, *end = d + len;
    while (s < end) {
        const char *e = memchr(s, '\n', (size_t)(end - s));
        size_t n = e ? (size_t)(e - s) : (size_t)(end - s);
        lineno++;
        if (lineno >= offset) {
            if (emitted >= limit) { capped = !explicit_limit; break; }   /* the model asked for exactly this many */
            if (emitted > 0 && out->len - start + n > READ_CAP) { capped = true; break; }
            sb_printf(out, "%6d| ", lineno);
            sb_append(out, s, n);
            sb_putc(out, '\n');
            emitted++;
        }
        if (!e) break;
        s = e + 1;
    }
    if (emitted == 0) sb_printf(out, "(no lines in range; file has %d lines)\n", lineno);
    else if (capped) sb_printf(out, "\n[showing lines %d-%d; more follows: call read_file again with offset=%d]\n", offset, lineno - 1, lineno);
    else if (len >= READ_FILE_MAX) sb_printf(out, "\n[file is larger than %d MB; only the beginning was read]\n", READ_FILE_MAX / (1024 * 1024));
    free(d); free(p);
    return TOOL_OK;
}

/* ---------- write_file ---------- */
static tool_status t_write_file(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", NULL);
    const char *content = jstr(args, "content", NULL);
    if (!path || !content) { sb_puts(out, "error: need 'path' and 'content'"); return TOOL_ERROR; }
    char *p = expand_home(path);
    if (!path_in_root(p)) { sb_printf(out, "error: %s is outside this candidate's worktree; write inside the working directory", path); free(p); return TOOL_ERROR; }
    bool exists = is_file(p);
    printf("  " C_DIM "%s %s (%zu bytes)" C_RESET "\n", exists ? "overwrite" : "create", path, strlen(content));
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    preview_lines(content, 12, C_GREEN, dot && (!slash || dot > slash) ? dot + 1 : NULL);
    char *reason = NULL;
    if (!confirm(exists ? "Overwrite this file?" : "Create this file?", CONF_EDIT, &g_always_write, path, &reason)) {
        sb_printf(out, "User denied writing %s.%s%s", path, reason ? " Reason: " : "", reason ? reason : "");
        free(reason); free(p); return TOOL_DENIED;
    }
    /* mkdir parents */
    char *dir = xstrdup(p); char *sl = strrchr(dir, '/');
    if (sl && sl != dir) { *sl = 0; mkdir_p(dir); }
    free(dir);
    ckpt_save(p);
    if (write_whole_file(p, content, strlen(content)) != 0) { sb_printf(out, "error: cannot write %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    int lines = 0; for (const char *c = content; *c; c++) if (*c == '\n') lines++;
    sb_printf(out, "Wrote %zu bytes (%d lines) to %s", strlen(content), lines, path);
    free(p);
    return TOOL_OK;
}

/* ---------- edit_file ---------- */
static int count_occ(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!nl) return 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += nl) c++;
    return c;
}
static tool_status t_edit_file(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", NULL);
    const char *olds = jstr(args, "old_string", NULL);
    const char *news = jstr(args, "new_string", NULL);
    bool all = jbool(args, "replace_all", false);
    if (!path || !olds || !news) { sb_puts(out, "error: need 'path', 'old_string' and 'new_string'"); return TOOL_ERROR; }
    if (!*olds) { sb_puts(out, "error: old_string must not be empty (use write_file to create files)"); return TOOL_ERROR; }
    char *p = expand_home(path);
    if (!path_in_root(p)) { sb_printf(out, "error: %s is outside this candidate's worktree; edit inside the working directory", path); free(p); return TOOL_ERROR; }
    size_t len; char *d = read_whole_file(p, &len, 0);
    if (!d) { sb_printf(out, "error: cannot read %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    int occ = count_occ(d, olds);
    if (occ == 0) { sb_printf(out, "error: old_string not found in %s. Re-read the file and match the text exactly (whitespace matters).", path); free(d); free(p); return TOOL_ERROR; }
    if (occ > 1 && !all) { sb_printf(out, "error: old_string occurs %d times in %s; include more surrounding context to make it unique, or set replace_all=true.", occ, path); free(d); free(p); return TOOL_ERROR; }
    /* build result */
    sbuf nb; sb_init(&nb);
    size_t ol = strlen(olds);
    const char *s = d;
    for (;;) {
        const char *m = strstr(s, olds);
        if (!m) { sb_puts(&nb, s); break; }
        sb_append(&nb, s, (size_t)(m - s));
        sb_puts(&nb, news);
        s = m + ol;
        if (!all) { sb_puts(&nb, s); break; }
    }
    printf("  " C_DIM "edit %s (%d replacement%s)" C_RESET "\n", path, occ, occ == 1 ? "" : "s");
    /* diff-ish preview */
    { sbuf t; sb_init(&t); const char *q = olds; while (*q) { const char *e = strchr(q, '\n'); size_t n = e ? (size_t)(e-q) : strlen(q); sb_printf(&t, "- %.*s\n", (int)n, q); if (!e) break; q = e + 1; } preview_lines(t.data, 10, C_RED, NULL); sb_free(&t); }
    { sbuf t; sb_init(&t); const char *q = news; if (!*q) sb_puts(&t, "+ (deleted)\n"); while (*q) { const char *e = strchr(q, '\n'); size_t n = e ? (size_t)(e-q) : strlen(q); sb_printf(&t, "+ %.*s\n", (int)n, q); if (!e) break; q = e + 1; } preview_lines(t.data, 10, C_GREEN, NULL); sb_free(&t); }
    char *reason = NULL;
    if (!confirm("Apply this edit?", CONF_EDIT, &g_always_edit, path, &reason)) {
        sb_printf(out, "User denied editing %s.%s%s", path, reason ? " Reason: " : "", reason ? reason : "");
        free(reason); sb_free(&nb); free(d); free(p); return TOOL_DENIED;
    }
    ckpt_save(p);
    if (write_whole_file(p, nb.data ? nb.data : "", nb.len) != 0) { sb_printf(out, "error: cannot write %s: %s", path, strerror(errno)); sb_free(&nb); free(d); free(p); return TOOL_ERROR; }
    sb_printf(out, "Edited %s: %d replacement%s made.", path, occ, occ == 1 ? "" : "s");
    sb_free(&nb); free(d); free(p);
    return TOOL_OK;
}

/* ---------- list_dir ---------- */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static tool_status t_list_dir(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", ".");
    if (!*path) path = ".";
    char *p = expand_home(path);
    DIR *d = opendir(p);
    if (!d) { sb_printf(out, "error: cannot open %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    char **names = NULL; int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = xrealloc(names, sizeof(char*) * (size_t)cap); }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char*), cmp_str);
    sb_printf(out, "%s/ (%d entries)\n", path, n);
    for (int i = 0; i < n && i < MAX_ENTRIES; i++) {
        char full[4096]; snprintf(full, sizeof full, "%s/%s", p, names[i]);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) sb_printf(out, "  %s/\n", names[i]);
            else if (S_ISLNK(st.st_mode)) sb_printf(out, "  %s@\n", names[i]);
            else sb_printf(out, "  %s  (%lld bytes)\n", names[i], (long long)st.st_size);
        } else sb_printf(out, "  %s\n", names[i]);
    }
    if (n > MAX_ENTRIES) sb_printf(out, "  ... %d more entries not shown\n", n - MAX_ENTRIES);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names); free(p);
    return TOOL_OK;
}

/* ---------- shell runner ---------- */
/* Runs cmd via /bin/sh -c, capturing stdout+stderr. Returns exit code (or -1 on failure,
 * 124 on timeout, 130 if interrupted by user). A message queued meanwhile does not stop it.
 * `cap` bounds what is kept while it streams (a runaway command cannot eat the heap);
 * `busy`/`verb` label the status bar and the inline spinner while it runs. */
static int run_shell(const char *cmd, int timeout_s, sbuf *out, bool *interrupted,
                     size_t cap, const char *busy, const char *verb) {
    *interrupted = false;
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        dup2(pfd[1], STDOUT_FILENO); dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]); close(pfd[1]);
        /* restore default signals */
        signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("PAGER", "cat", 1);
        setenv("GIT_PAGER", "cat", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    setpgid(pid, pid);
    struct pollfd fds[2] = { { pfd[0], POLLIN, 0 }, { g_cfg.interactive ? term_interrupt_fd() : -1, POLLIN, 0 } };
    long remaining_ms = (long)timeout_s * 1000;
    bool timed_out = false;
    term_raw(true);
    term_busy(busy);
    struct timeval t0; gettimeofday(&t0, NULL);
    int spin = 0; bool spin_shown = false;
    for (;;) {
        int r = poll(fds, 2, 100);
        if (r < 0) { if (errno == EINTR) continue; break; }
        term_busy_tick();
        if (r == 0) {
            remaining_ms -= 100; if (remaining_ms <= 0) { timed_out = true; break; }
            if (g_cfg.interactive) {   /* inline spinner: nothing else moves while the command runs */
                struct timeval t; gettimeofday(&t, NULL);
                double el = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_usec - t0.tv_usec) / 1e6;
                static const char *SP[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
                spin = (spin + 1) % 10;
                printf("\r\x1b[2K  " C_ORANGE "%s" C_RESET C_DIM " %s… (%.0fs)  " C_RESET "ctrl-c to interrupt", SP[spin], verb, el);
                fflush(stdout); spin_shown = true;
            }
            continue;
        }
        if (fds[1].revents & POLLIN) {
            if (term_poll_interrupt()) { *interrupted = true; break; }   /* (a queued message waits for the command to finish) */
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[8192]; ssize_t n = read(pfd[0], buf, sizeof buf);
            if (n > 0) { sb_append(out, buf, (size_t)n); if (out->len > cap * 2) cap_output(out, cap); }
            else break;   /* EOF */
        }
    }
    if (spin_shown) { fputs("\r\x1b[2K", stdout); fflush(stdout); }
    term_busy(NULL);
    if (timed_out || *interrupted) { kill(-pid, SIGTERM); usleep(200 * 1000); kill(-pid, SIGKILL); }
    /* drain */
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (timed_out) return 124;
    if (*interrupted) return 130;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* The command as the transcript shows it. A command of several lines is folded to its first line,
 * with a mark that opens the rest (term_fold_begin); not when the user is about to be asked about
 * it (they approve what they can see), and off full-screen mode it is printed whole, as before. */
/* A one-line command short enough to sit in the question: the approval shows it in full, so no "$" line. */
int tools_shell_in(const char *dir, const char *cmd, int timeout_s, sbuf *out, bool *interrupted, const char *busy, const char *verb) {
    bool entered = dir ? tools_enter(dir) : false;
    if (dir && !entered) { *interrupted = false; sb_printf(out, "cannot enter %s", dir); return -1; }
    int rc = run_shell(cmd, timeout_s, out, interrupted, OUT_CAP * 2, busy, verb);
    if (entered) tools_leave();
    return rc;
}

static bool cmd_in_question(const char *cmd) { return !strchr(cmd, '\n') && strlen(cmd) <= 160; }
static void print_command(const char *cmd, bool will_ask) {
    char first[320];
    int w = term_width() - 6 - 16; if (w < 12) w = 12;   /* "  ▸ $ " and "  (+NNNN lines)" */
    int more = cmd_head(cmd, w, first, sizeof first);
    if (will_ask && cmd_in_question(cmd)) return;
    if (!will_ask && more == 0) return;   /* not asked, one line: the result's line shows it */
    if (more > 0 && !will_ask) {
        char head[512];
        snprintf(head, sizeof head, "  " C_DIM TERM_FOLD_CLOSED " $ " C_RESET C_BOLD "%s" C_RESET C_DIM "  (+%d lines)" C_RESET, first, more);
        if (term_fold_begin(head)) {
            for (const char *p = cmd; *p; ) {
                size_t len = strcspn(p, "\n");
                fputs("     " C_DIM, stdout);
                for (size_t i = 0; i < len; i++) { unsigned char c = (unsigned char)p[i]; if (c == '\t') fputs("    ", stdout); else fputc(c < 32 || c == 127 ? ' ' : c, stdout); }   /* no carriage returns, no escapes: they would move the model's cursor (cmd_head maps the same) */
                fputs(C_RESET "\n", stdout);
                p += len; if (*p == '\n') p++;
            }
            term_fold_end();
            return;
        }
    }
    printf("  " C_DIM "$ " C_RESET C_BOLD "%s" C_RESET "\n", cmd);
}

/* ---------- background jobs ----------
 * bash with run_in_background starts its command and returns at once. Its output goes to a file in TMPDIR, not a pipe:
 * nothing has to read it while the model works, and the job never blocks on it. bash_job reads what it printed since
 * the last read, and stops it. Jobs belong to this process: /ui re-executes it and quitting stops them. The job's
 * group is killed when corbienest exits by any way it can be told to (atexit, SIGTERM, SIGHUP); a SIGKILL of
 * corbienest is covered on Linux by PR_SET_PDEATHSIG for the shell only. */
#define BG_MAX 8
#define BG_READ_MAX (32 * 1024)
typedef struct { char id[16]; pid_t pid; char *cmd, *file; off_t off; bool done, killed; int rc; } bg_job;
static bg_job g_bg[BG_MAX];
static volatile pid_t g_bg_pgid[BG_MAX];   /* the groups still running, for the signal handler (async-signal-safe reads) */
static int g_bg_next = 1;
static bool g_in_subagent = false;

static void bg_reap(bg_job *j) {
    if (!j->cmd || j->done || j->pid <= 0) return;
    int st = 0;
    pid_t r = waitpid(j->pid, &st, WNOHANG);
    if (r == 0) return;
    j->done = true;
    j->rc = r < 0 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);   /* -1: gone, the code unknown */
    for (int i = 0; i < BG_MAX; i++) if (&g_bg[i] == j) g_bg_pgid[i] = 0;
}
static void bg_sweep(void) { for (int i = 0; i < BG_MAX; i++) bg_reap(&g_bg[i]); }
static void bg_forget(bg_job *j) {
    if (j->file) unlink(j->file);
    free(j->cmd); free(j->file);
    memset(j, 0, sizeof *j);
}
static void bg_signal_all(int sig) {
    for (int i = 0; i < BG_MAX; i++) { pid_t g = g_bg_pgid[i]; if (g > 0) kill(-g, sig); }
}
/* Stops every job still running and removes the files: at exit, and before corbienest re-executes itself. */
void tools_bg_kill_all(void) {
    bg_signal_all(SIGTERM);
    for (int k = 0; k < 10; k++) {   /* up to half a second for them to go */
        bool alive = false;
        for (int i = 0; i < BG_MAX; i++) if (g_bg_pgid[i] > 0) { bg_reap(&g_bg[i]); if (g_bg_pgid[i] > 0) alive = true; }
        if (!alive) break;
        usleep(50000);
    }
    bg_signal_all(SIGKILL);
    for (int i = 0; i < BG_MAX; i++) if (g_bg[i].cmd) { bg_reap(&g_bg[i]); bg_forget(&g_bg[i]); }
}
static void bg_on_signal(int sig) {
    bg_signal_all(SIGKILL);
    mcp_kill_signal();
    signal(sig, SIG_DFL);
    raise(sig);
}
void tools_bg_init(void) {
    atexit(tools_bg_kill_all);
    signal(SIGTERM, bg_on_signal);
    signal(SIGHUP, bg_on_signal);
}

static bg_job *bg_find(const char *id) {
    for (int i = 0; i < BG_MAX; i++) if (g_bg[i].cmd && !strcmp(g_bg[i].id, id)) return &g_bg[i];
    return NULL;
}
/* A free slot; when there is none, the oldest finished job gives its place up (its output is gone with it). */
static bg_job *bg_slot(void) {
    for (int i = 0; i < BG_MAX; i++) if (!g_bg[i].cmd) return &g_bg[i];
    for (int i = 0; i < BG_MAX; i++) if (g_bg[i].done) { bg_forget(&g_bg[i]); return &g_bg[i]; }
    return NULL;
}
static tool_status bg_start(const char *cmd, sbuf *out) {
    bg_sweep();
    bg_job *j = bg_slot();
    if (!j) { sb_printf(out, "error: %d background jobs are running; stop one with bash_job (action kill) first", BG_MAX); return TOOL_ERROR; }
    const char *tmpdir = getenv("TMPDIR");
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s/corbienest_bg_XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    int fd = mkstemp(tmp);
    if (fd < 0) { sb_printf(out, "error: cannot make the job's output file: %s", strerror(errno)); return TOOL_ERROR; }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) { close(fd); unlink(tmp); sb_printf(out, "error: fork: %s", strerror(errno)); return TOOL_ERROR; }
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(1);   /* corbienest died before the job began */
#endif
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); if (dn != STDIN_FILENO) close(dn); }
        dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
        for (int f = 3; f < 1024; f++) close(f);   /* the job inherits none of the session's descriptors */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);   /* (the child does it too: whichever runs first) */
    close(fd);
    memset(j, 0, sizeof *j);
    snprintf(j->id, sizeof j->id, "bg%d", g_bg_next++);
    j->pid = pid; j->cmd = xstrdup(cmd); j->file = xstrdup(tmp);
    g_bg_pgid[j - g_bg] = pid;
    sb_printf(out, "started background job %s (pid %d): %s\nRead what it prints with bash_job (action output); it runs until it ends or you stop it with bash_job (action kill). A background job belongs to this session: /resume does not bring it back, and quitting or /ui stops it.", j->id, (int)pid, cmd);
    return TOOL_OK;
}
/* One read of a job's output: what it printed since the last read (at most BG_READ_MAX of it), then its state. */
static void bg_read(bg_job *j, sbuf *out) {
    int fd = open(j->file, O_RDONLY);
    if (fd < 0) { sb_printf(out, "(no output file: %s)\n", strerror(errno)); return; }
    struct stat st;
    off_t size = fstat(fd, &st) == 0 ? st.st_size : j->off;
    if (size - j->off > BG_READ_MAX) { sb_printf(out, "(%lld bytes skipped)\n", (long long)(size - j->off - BG_READ_MAX)); j->off = size - BG_READ_MAX; }
    size_t want = (size_t)(size - j->off);
    if (want) {
        char *buf = xmalloc(want + 1);
        ssize_t r = pread(fd, buf, want, j->off);
        if (r > 0) { sb_append(out, buf, (size_t)r); j->off += r; }
        free(buf);
    }
    close(fd);
    if (out->len && out->data[out->len - 1] != '\n') sb_putc(out, '\n');
    if (!out->len) sb_puts(out, "(no new output)\n");
}
static tool_status t_bash_job(cJSON *args, sbuf *out) {
    const char *id = jstr(args, "job", NULL), *action = jstr(args, "action", NULL);
    if (!id || !action) { sb_puts(out, "error: need 'job' (e.g. bg1) and 'action' (output or kill)"); return TOOL_ERROR; }
    bg_sweep();
    bg_job *j = bg_find(id);
    if (!j) { sb_printf(out, "error: no background job %s (the ones running now are listed as bgN in the bash results)", id); return TOOL_ERROR; }
    if (!strcmp(action, "output")) {
        bg_read(j, out);
        if (!j->done) sb_printf(out, "[job %s: running]", id);
        else if (j->killed) sb_printf(out, "[job %s: stopped]", id);
        else if (j->rc < 0) sb_printf(out, "[job %s: exited, code unknown]", id);
        else sb_printf(out, "[job %s: exit code %d]", id, j->rc);
        return TOOL_OK;
    }
    if (!strcmp(action, "kill")) {
        if (!j->done) {
            j->killed = true;
            kill(-j->pid, SIGTERM);
            for (int k = 0; k < 40 && !j->done; k++) { bg_reap(j); if (!j->done) usleep(50000); }
            if (!j->done) { kill(-j->pid, SIGKILL); for (int k = 0; k < 20 && !j->done; k++) { bg_reap(j); if (!j->done) usleep(10000); } }
        }
        bg_read(j, out);
        sb_printf(out, "[job %s: %s]", id, j->killed ? "stopped" : j->rc >= 0 ? "had already exited" : "had already exited (code unknown)");
        bg_forget(j);
        return TOOL_OK;
    }
    sb_printf(out, "error: action must be output or kill, not %s", action);
    return TOOL_ERROR;
}

/* A call of a server's tool: the user approves it like a shell command (allow rule: "mcp NAME", the always option
 * saves it), and the project's deny and ask guards apply to the tool's name. */
static tool_status t_mcp(const char *name, cJSON *args, sbuf *out) {
    char *reason = NULL;
    if (!confirm("Call this MCP tool?", CONF_MCP, &g_always_mcp, name, &reason)) {
        if (reason) sb_printf(out, "%s was not run: %s", name, reason); else sb_printf(out, "User denied calling %s.", name);
        free(reason); return TOOL_DENIED;
    }
    return mcp_call(name, args, out);
}
/* ---------- sandbox (sandbox=bwrap) ----------
 * The model's shell commands run in bubblewrap when the config says so: the whole filesystem read-only, the project
 * directory writable, a private /tmp, and the network as it is. The user's own !lines, the hooks and the MCP servers
 * are not in it. A sandbox that cannot start here is said so, and the commands run without it. */
static void put_word(sbuf *b, const char *s) {   /* a string as one shell word, appended to b */
    sb_putc(b, '\'');
    for (; *s; s++) { if (*s == '\'') sb_puts(b, "'\\''"); else sb_putc(b, *s); }
    sb_putc(b, '\'');
}
char *sandbox_command(const char *cwd, const char *cmd) {
    sbuf b; sb_init(&b);
    sb_puts(&b, "bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --die-with-parent --new-session --bind ");
    put_word(&b, cwd); sb_putc(&b, ' '); put_word(&b, cwd);
    sb_puts(&b, " --chdir "); put_word(&b, cwd);
    sb_puts(&b, " -- /bin/sh -c "); put_word(&b, cmd);
    return sb_detach(&b);
}
static int g_sandbox_state = 0;      /* 0 not asked, 1 usable, -1 not usable */
static char g_sandbox_why[600];
bool sandbox_available(void) {
    if (g_sandbox_state) return g_sandbox_state > 0;
    char path[1200];
    bool found = false;
    const char *pe = getenv("PATH"); if (!pe) pe = "/usr/bin:/bin";
    for (const char *p = pe; *p && !found; ) {
        const char *e = strchr(p, ':'); size_t n = e ? (size_t)(e - p) : strlen(p);
        snprintf(path, sizeof path, "%.*s/bwrap", (int)n, *p ? p : ".");
        if (!access(path, X_OK)) found = true;
        p = e ? e + 1 : p + n;
    }
    if (!found) { snprintf(g_sandbox_why, sizeof g_sandbox_why, "bwrap is not installed (apt install bubblewrap)"); g_sandbox_state = -1; return false; }
    char trial[1400], err[1400] = "";
    char tmpl[] = "/tmp/corbienest_sbx_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    snprintf(trial, sizeof trial, "bwrap --ro-bind / / --dev /dev --proc /proc --die-with-parent --new-session true >%s 2>&1", fd >= 0 ? tmpl : "/dev/null");
    int rc = system(trial);
    if (fd >= 0) { FILE *f = fopen(tmpl, "r"); if (f) { size_t n = fread(err, 1, sizeof err - 1, f); err[n] = 0; fclose(f); } unlink(tmpl); }
    if (rc == 0) { snprintf(g_sandbox_why, sizeof g_sandbox_why, "bwrap: the project directory writable, the rest read-only, a private /tmp, network allowed"); g_sandbox_state = 1; return true; }
    char *nl = strchr(err, '\n'); if (nl) *nl = 0;
    snprintf(g_sandbox_why, sizeof g_sandbox_why, "bwrap cannot start here (%s; user namespaces may be disabled)", err[0] ? err : "exit status not 0");
    g_sandbox_state = -1;
    return false;
}
const char *sandbox_why(void) { return g_sandbox_why; }

static tool_status t_bash(cJSON *args, sbuf *out) {
    const char *cmd = jstr(args, "command", NULL);
    if (!cmd || !*cmd) { sb_puts(out, "error: missing 'command'"); return TOOL_ERROR; }
    int timeout = jint(args, "timeout", 120);
    if (timeout <= 0) timeout = 120;
    if (timeout > 600) timeout = 600;
    bool bg = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "run_in_background"));
    if (bg && g_in_subagent) { sb_puts(out, "error: a sub-agent cannot start background commands; run it in the foreground"); return TOOL_ERROR; }
    bool asked = confirm_will_ask(CONF_BASH, &g_always_bash, cmd);
    print_command(cmd, asked);
    char q[256];   /* the question carries a short command in full: the approval is of what is shown */
    if (asked && cmd_in_question(cmd)) snprintf(q, sizeof q, bg ? "Run this command in the background?  %s" : "Run this command?  %s", cmd);
    else snprintf(q, sizeof q, bg ? "Run this command in the background?" : "Run this command?");
    char *reason = NULL;
    if (!confirm(q, CONF_BASH, &g_always_bash, cmd, &reason)) {
        sb_printf(out, "User denied running the command.%s%s", reason ? " Reason: " : "", reason ? reason : "");
        free(reason); return TOOL_DENIED;
    }
    /* the model's commands go through bwrap when the config says so (not the user's own !lines) */
    char *sbx = NULL; const char *run = cmd;
    if (g_cfg.sandbox && !tools_no_confirm) {
        char cwd[PATH_MAX];
        if (!sandbox_available()) {   /* said once, then the commands run without it */
            static bool said = false;
            if (!said) { said = true; printf("  " C_YELLOW "sandbox off" C_RESET C_DIM " — %s" C_RESET "\n", g_sandbox_why); }
        } else if (getcwd(cwd, sizeof cwd)) { sbx = sandbox_command(cwd, cmd); run = sbx; }
    }
    if (bg) { tool_status st = bg_start(run, out); free(sbx); return st; }
    sbuf o; sb_init(&o);
    bool intr = false;
    int rc = run_shell(run, timeout, &o, &intr, OUT_CAP * 2, "running command", "running");
    free(sbx);
    cap_output(&o, OUT_CAP);
    if (rc == 124) sb_printf(out, "(command timed out after %ds and was killed)\n", timeout);
    if (intr) sb_puts(out, "(command interrupted by user with Ctrl-C)\n");
    if (o.len) { sb_append(out, o.data, o.len); if (o.data[o.len-1] != '\n') sb_putc(out, '\n'); }
    else sb_puts(out, "(no output)\n");
    sb_printf(out, "exit code: %d", rc);
    sb_free(&o);
    return TOOL_OK;
}

/* ---------- grep ---------- */
static void sh_quote(sbuf *b, const char *s) {
    sb_putc(b, '\'');
    for (; *s; s++) { if (*s == '\'') sb_puts(b, "'\\''"); else sb_putc(b, *s); }
    sb_putc(b, '\'');
}
static tool_status t_grep(cJSON *args, sbuf *out) {
    const char *pat = jstr(args, "pattern", NULL);
    const char *path = jstr(args, "path", ".");
    const char *inc = jstr(args, "include", NULL);
    if (!pat || !*pat) { sb_puts(out, "error: missing 'pattern'"); return TOOL_ERROR; }
    if (!*path) path = ".";
    sbuf cmd; sb_init(&cmd);
    sb_puts(&cmd, "grep -rnIE --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=.venv --exclude-dir=__pycache__ ");
    if (inc && *inc) { sb_puts(&cmd, "--include="); sh_quote(&cmd, inc); sb_putc(&cmd, ' '); }
    sb_puts(&cmd, "-e "); sh_quote(&cmd, pat); sb_puts(&cmd, " -- ");
    char *p = expand_home(path); sh_quote(&cmd, p); free(p);
    sb_puts(&cmd, " 2>/dev/null | head -n 300");
    sbuf o; sb_init(&o); bool intr;
    run_shell(cmd.data, 60, &o, &intr, OUT_CAP * 2, "running command", "running");
    cap_output(&o, OUT_CAP);
    if (!o.len) sb_printf(out, "No matches for /%s/ in %s", pat, path);
    else { sb_append(out, o.data, o.len); }
    sb_free(&o); sb_free(&cmd);
    return TOOL_OK;
}

/* ---------- web_fetch ----------
 * corbienest's own HTTP client is plain POSIX sockets and does not speak TLS (http.c), and a
 * TLS library would break the "C11 + cJSON, nothing else" promise — so the page comes in
 * through curl (or wget), which any machine that can build corbienest already has. What the
 * model gets back is readable text, not markup: a docs page is worth a tool round, its <div>
 * soup is not. */
#define WEB_MARK "<<<corbie-fetch:"

static bool have_bin(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin:/usr/local/bin";
    for (const char *p = path; *p; ) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) {
            char full[4096];
            snprintf(full, sizeof full, "%.*s/%s", (int)n, p, name);
            if (access(full, X_OK) == 0) return true;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* wget tells us nothing about the content type, so sniff the start of the body. */
static bool looks_like_html(const char *d, size_t len) {
    size_t n = len < 2048 ? len : 2048;
    for (size_t i = 0; i + 5 < n; i++)
        if (d[i] == '<' && (!strncasecmp(d + i, "<html", 5) || !strncasecmp(d + i, "<!doctype html", 14) ||
                            !strncasecmp(d + i, "<head", 5) || !strncasecmp(d + i, "<body", 5))) return true;
    return false;
}

/* Downloads `url`. On success `body` holds the page (owned by the caller), *status the HTTP
 * status (0 with wget, which does not report one) and `ctype` its content type. On failure
 * `err` gets a line the model can act on. */
static bool web_get(const char *url, int timeout, const char *busy, const char *verb,
                    sbuf *body, int *status, char *ctype, size_t ctype_n, char *loc, size_t loc_n, sbuf *err) {
    sb_init(body); *status = 0; if (ctype_n) ctype[0] = 0; if (loc_n) loc[0] = 0;
    bool curl = have_bin("curl");
    if (!curl && !have_bin("wget")) {
        sb_puts(err, "error: neither curl nor wget is installed, so corbienest cannot reach the network. Tell the user; do not guess what the page says.");
        return false;
    }
    sbuf cmd; sb_init(&cmd);
    if (curl) {
        /* no -L: a redirect is not followed here, the new address is reported and fetched as a call of its own */
        sb_printf(&cmd, "curl -sS --proto =http,https --max-time %d --max-filesize %u --compressed -A 'corbienest/%s' "
                        "-H 'Accept: text/html,text/plain,text/markdown,application/json;q=0.9,*/*;q=0.8' "
                        "-w '\n" WEB_MARK "%%{http_code} %%{content_type}\n%%{redirect_url}>>>' -- ", timeout, WEB_MAX, CORBIE_VERSION);
    } else {
        sb_printf(&cmd, "wget -q -O - --max-redirect=0 --timeout=%d --tries=2 -U 'corbienest/%s' -- ", timeout, CORBIE_VERSION);
    }
    sh_quote(&cmd, url);

    bool intr = false;
    int rc = run_shell(cmd.data, timeout + 5, body, &intr, WEB_MAX, busy, verb);
    sb_free(&cmd);

    /* curl's -w marker is appended after the body: status and content type, then off it goes */
    if (body->len) {
        char *mark = NULL;
        for (char *q = strstr(body->data, WEB_MARK); q; q = strstr(q + 1, WEB_MARK)) mark = q;
        if (mark) {
            /* "CODE CONTENT-TYPE\nREDIRECT>>>": the type and the address are each on their own line */
            const char *q = mark + strlen(WEB_MARK), *nl = strchr(q, '\n'), *end = strstr(q, ">>>");
            sscanf(q, "%d", status);
            const char *sp = strchr(q, ' ');
            if (ctype_n && sp && nl && sp < nl) snprintf(ctype, ctype_n, "%.*s", (int)(nl - sp - 1), sp + 1);
            if (loc_n && nl && end && nl < end) {
                bool clean = true;   /* a server's address goes to the model and the screen: printable, no spaces */
                for (const char *c = nl + 1; c < end; c++) if ((unsigned char)*c <= 0x20 || (unsigned char)*c >= 0x7f) clean = false;
                if (clean) snprintf(loc, loc_n, "%.*s", (int)(end - nl - 1), nl + 1);
            }
            size_t at = (size_t)(mark - body->data);
            if (at && body->data[at-1] == '\n') at--;
            body->data[at] = 0; body->len = at;
        }
    }

    bool ok = true;
    if (rc == 124) { sb_printf(err, "error: fetching %s timed out after %ds", url, timeout); ok = false; }
    else if (intr) { sb_puts(err, "(the fetch was interrupted by the user with Ctrl-C)"); ok = false; }
    else if (*status >= 300 && *status < 400) {
        sb_printf(err, "error: %s redirects to %s. Redirects are not followed: if that page is wanted, call web_fetch with the new address (it is checked and confirmed like any other).",
                  url, loc[0] ? loc : "another address");
        ok = false;
    }
    else if (*status && (*status < 200 || *status >= 300))
        { sb_printf(err, "error: %s returned HTTP %d.%s", url, *status,
                    *status == 404 ? " The page does not exist — check the URL or the documentation version." : ""); ok = false; }
    else if ((curl && !*status) || (!curl && rc != 0)) {
        /* no HTTP response at all: DNS, TLS, connection refused. What curl said is on stderr,
         * which run_shell captured with the body — hand it to the model, it explains itself. */
        char msg[320] = "";
        if (body->len) {
            size_t n = body->len > 220 ? 220 : body->len;
            snprintf(msg, sizeof msg, ": %.*s", (int)n, body->data);
            for (char *q = msg; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        }
        sb_printf(err, "error: could not fetch %s (%s exited %d)%s", url, curl ? "curl" : "wget", rc, msg);
        ok = false;
    }
    else if (!body->len) { sb_printf(err, "error: %s returned nothing", url); ok = false; }
    if (!ok) sb_free(body);
    return ok;
}

static tool_status t_web_fetch(cJSON *args, sbuf *out) {
    const char *url = jstr(args, "url", NULL);
    if (!url || !*url) { sb_puts(out, "error: missing 'url'"); return TOOL_ERROR; }
    if (!g_cfg.web) { sb_puts(out, "error: web access is off in this session (the user can turn it on with /web on). Say so instead of guessing what the page says."); return TOOL_ERROR; }
    if (!url_ok(url)) { sb_printf(out, "error: web_fetch will not open '%s' — http:// or https:// only, no whitespace or control characters, and cloud-metadata addresses are refused.", url); return TOOL_ERROR; }
    int timeout = jint(args, "timeout", 30);
    if (timeout <= 0) timeout = 30;
    if (timeout > 120) timeout = 120;
    int offset = jint(args, "offset", 1);
    if (offset < 1) offset = 1;

    printf("  " C_DIM "⇣ " C_RESET C_BOLD "%s" C_RESET "\n", url);
    char *reason = NULL;
    if (!confirm("Fetch this page?", CONF_FETCH, &g_always_fetch, url, &reason)) {
        sb_printf(out, "User denied fetching the page.%s%s", reason ? " Reason: " : "", reason ? reason : "");
        free(reason); return TOOL_DENIED;
    }

    sbuf o; int status; char ctype[128], loc[1024];
    if (!web_get(url, timeout, "fetching page", "fetching", &o, &status, ctype, sizeof ctype, loc, sizeof loc, out)) return TOOL_ERROR;

    bool html = ctype[0] ? (strcasestr(ctype, "html") != NULL || strcasestr(ctype, "xml") != NULL)
                         : looks_like_html(o.data, o.len);
    char base[600] = "";   /* scheme://host, so rooted links come back fetchable */
    { const char *sl = strchr(url + 8, '/'); snprintf(base, sizeof base, "%.*s", sl ? (int)(sl - url) : (int)strlen(url), url); }
    char *text = html ? html_to_text(o.data, o.len, base) : xstrndup(o.data, o.len);
    sb_free(&o);
    size_t tlen = strlen(text);

    char size[32];
    if (tlen < 2048) snprintf(size, sizeof size, "%zu bytes", tlen);
    else snprintf(size, sizeof size, "%zu KB", (tlen + 512) / 1024);
    sb_printf(out, "%s%s%s (%s of text)\n\n", url, ctype[0] ? " — " : "", ctype[0] ? ctype : "", size);
    int total = 0;
    for (const char *q = text; *q; q++) if (*q == '\n') total++;
    if (tlen && text[tlen-1] != '\n') total++;

    int lineno = 0, emitted = 0; bool capped = false;
    size_t start = out->len;
    for (const char *q = text; *q; ) {
        const char *e = strchr(q, '\n');
        size_t n = e ? (size_t)(e - q) : strlen(q);
        lineno++;
        if (lineno >= offset) {
            if (emitted > 0 && out->len - start + n > WEB_CAP) { capped = true; break; }
            sb_append(out, q, n);
            sb_putc(out, '\n');
            emitted++;
        }
        if (!e) break;
        q = e + 1;
    }
    if (!emitted) sb_printf(out, "(no text at line %d; the page has %d lines)\n", offset, total);
    else if (capped) sb_printf(out, "\n[showing lines %d-%d of %d; call web_fetch again with the same url and offset=%d for the rest]\n", offset, lineno - 1, total, lineno);
    free(text);
    return TOOL_OK;
}

/* ---------- web_search ----------
 * The engine is a URL template with %s where the query goes (config `search_url`, /web
 * engine), so anyone can point this at their own SearXNG instead of the default. Results
 * come back as title / URL / snippet — the model is expected to web_fetch the good one. */
const char *SEARCH_URL_DEFAULT = "https://html.duckduckgo.com/html/?q=%s";

static tool_status t_web_search(cJSON *args, sbuf *out) {
    const char *q = jstr(args, "query", NULL);
    if (!q || !*q) { sb_puts(out, "error: missing 'query'"); return TOOL_ERROR; }
    if (!g_cfg.web) { sb_puts(out, "error: web access is off in this session (the user can turn it on with /web on). Say so instead of guessing."); return TOOL_ERROR; }
    int max = jint(args, "max_results", 8);
    if (max < 1) max = 1;
    if (max > 20) max = 20;

    const char *tmpl = g_cfg.search_url && *g_cfg.search_url ? g_cfg.search_url : SEARCH_URL_DEFAULT;
    char *enc = url_encode(q);
    sbuf u; sb_init(&u);
    const char *pct = strstr(tmpl, "%s");
    if (pct) { sb_append(&u, tmpl, (size_t)(pct - tmpl)); sb_puts(&u, enc); sb_puts(&u, pct + 2); }
    else { sb_puts(&u, tmpl); sb_puts(&u, enc); }   /* a template without %s: the query goes on the end */
    free(enc);
    char *url = sb_detach(&u);
    if (!url_ok(url)) { sb_printf(out, "error: the search engine URL '%s' is not usable (see /web engine)", url); free(url); return TOOL_ERROR; }
    char ehost[512] = ""; url_host(url, ehost, sizeof ehost);

    printf("  " C_DIM "⌕ " C_RESET C_BOLD "%s" C_RESET C_DIM " · %s" C_RESET "\n", q, ehost);
    char *reason = NULL;
    if (!confirm("Search the web?", CONF_FETCH, &g_always_fetch, url, &reason)) {
        sb_printf(out, "User denied the search.%s%s", reason ? " Reason: " : "", reason ? reason : "");
        free(reason); free(url); return TOOL_DENIED;
    }

    sbuf o; int status; char ctype[128], loc[1024];
    if (!web_get(url, 30, "searching the web", "searching", &o, &status, ctype, sizeof ctype, loc, sizeof loc, out)) { free(url); return TOOL_ERROR; }

    int n = 0;
    char *list = search_results_text(o.data, o.len, url, max, &n);
    sb_free(&o);
    if (!n) {
        sb_printf(out, "error: %s returned no results that could be read (the engine may have changed its page or refused the request). "
                       "Try different words, or web_fetch a documentation URL directly.", ehost);
        free(list); free(url);
        return TOOL_ERROR;
    }
    sb_printf(out, "%d result%s for \"%s\" via %s — web_fetch the best one, the snippets are not the docs.\n\n",
              n, n == 1 ? "" : "s", q, ehost);
    sb_puts(out, list);
    if (out->len > WEB_CAP) cap_output(out, WEB_CAP);
    free(list); free(url);
    return TOOL_OK;
}

/* ---------- task (sub-agent) ---------- */
tools_subagent_fn tools_subagent = NULL;
static tool_status t_task(cJSON *args, sbuf *out) {
    const char *desc = jstr(args, "description", "sub-agent");
    const char *prompt = jstr(args, "prompt", NULL);
    if (!prompt || !*prompt) { sb_puts(out, "error: missing 'prompt'"); return TOOL_ERROR; }
    if (!tools_subagent) { sb_puts(out, "error: sub-agents are not available here"); return TOOL_ERROR; }
    if (g_in_subagent) { sb_puts(out, "error: a sub-agent cannot start another sub-agent; do the work yourself"); return TOOL_ERROR; }
    g_in_subagent = true;
    int rc = tools_subagent(desc, prompt, jstr(args, "agent", NULL), out);
    g_in_subagent = false;
    return rc == 0 ? TOOL_OK : TOOL_ERROR;
}

/* ---------- exit (the model sees the user is leaving) ----------
 * The call only sets a flag: main.c ends the program when the turn is over, so the reply that says
 * goodbye is still written and the session is saved as on /quit. */
static bool g_exit_asked = false;
bool tools_exit_asked(void) { return g_exit_asked; }
void tools_exit_clear(void) { g_exit_asked = false; }
/* The task list (todo_write): the model's own plan for this request, shown in the transcript and saved with the session.
 * Not a tool the sub-agents have (run_subagent offers its own list). */
static cJSON *g_todos;
cJSON *tools_todos_get(void) { return g_todos; }
void tools_todos_set(cJSON *list) { cJSON_Delete(g_todos); g_todos = list; }
void tools_todo_render(sbuf *out) {
    if (!g_todos || !cJSON_GetArraySize(g_todos)) return;
    int done = 0, n = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, g_todos) { n++; cJSON *s = cJSON_GetObjectItemCaseSensitive(t, "status"); if (cJSON_IsString(s) && !strcmp(s->valuestring, "completed")) done++; }
    sb_printf(out, "Tasks (%d of %d done)\n", done, n);
    cJSON_ArrayForEach(t, g_todos) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(t, "content"), *s = cJSON_GetObjectItemCaseSensitive(t, "status");
        const char *st = cJSON_IsString(s) ? s->valuestring : "pending";
        sb_printf(out, "  %s %s\n", !strcmp(st, "completed") ? "☑" : !strcmp(st, "in_progress") ? "◐" : "☐", cJSON_IsString(c) ? c->valuestring : "");
    }
}
static tool_status t_todo_write(cJSON *args, sbuf *out) {
    cJSON *todos = cJSON_GetObjectItemCaseSensitive(args, "todos");
    if (!cJSON_IsArray(todos)) { sb_puts(out, "error: todos must be an array of {content, status}"); return TOOL_ERROR; }
    cJSON *list = cJSON_CreateArray(), *t;
    int busy = 0, i = 0;
    cJSON_ArrayForEach(t, todos) {
        i++;
        cJSON *c = cJSON_GetObjectItemCaseSensitive(t, "content"), *s = cJSON_GetObjectItemCaseSensitive(t, "status");
        const char *st = cJSON_IsString(s) ? s->valuestring : "";
        if (!cJSON_IsString(c) || !c->valuestring[0]) { cJSON_Delete(list); sb_printf(out, "error: task %d has no content", i); return TOOL_ERROR; }
        if (strcmp(st, "pending") && strcmp(st, "in_progress") && strcmp(st, "completed")) {
            cJSON_Delete(list); sb_printf(out, "error: task %d: status must be pending, in_progress or completed", i); return TOOL_ERROR;
        }
        if (!strcmp(st, "in_progress")) busy++;
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "content", c->valuestring); cJSON_AddStringToObject(n, "status", st);
        cJSON_AddItemToArray(list, n);
    }
    if (busy > 1) { cJSON_Delete(list); sb_puts(out, "error: only one task may be in_progress at a time"); return TOOL_ERROR; }
    tools_todos_set(list);
    if (!cJSON_GetArraySize(list)) sb_puts(out, "Task list cleared.");
    else tools_todo_render(out);
    return TOOL_OK;
}

static tool_status t_exit(sbuf *out) {
    if (g_in_subagent) { sb_puts(out, "error: a sub-agent cannot end the program"); return TOOL_ERROR; }
    g_exit_asked = true;
    sb_puts(out, "ok: the program ends after this reply. Say goodbye in one short line.");
    return TOOL_OK;
}

/* ---------- advisor (a stronger model, consulted) ---------- */
tools_advisor_fn tools_advisor = NULL;
static tool_status t_advisor(cJSON *args, sbuf *out) {
    const char *q = jstr(args, "question", NULL);
    if (!q) q = jstr(args, "prompt", NULL);
    if (!q) q = jstr(args, "query", "");
    if (!tools_advisor || !g_cfg.advisor) { sb_puts(out, "error: no advisor is set in this session (the user can set one with /advisor MODEL). Carry on by yourself."); return TOOL_ERROR; }
    if (g_in_subagent) { sb_puts(out, "error: the advisor is not available to a sub-agent; report what you found and let the main agent ask"); return TOOL_ERROR; }
    return tools_advisor(q, out) == 0 ? TOOL_OK : TOOL_ERROR;
}

/* ---------- dispatch ---------- */
/* ---------- hooks ----------
 * A hook is a shell command the user keeps for this project, in the config directory's hooks/ file (kept like the
 * permission rules: never read from the project tree, where a cloned repository could put commands). One per line:
 *   pre TOOL COMMAND    before a tool call. TOOL is the tool's name or a glob (* for all). Exit 2 stops the call, and
 *                       what the hook printed is the reason the model is given; a timeout, an interrupt or a hook that
 *                       could not run stops it too. Any other exit lets it run.
 *   post TOOL COMMAND   after a tool call; what it prints is added to the result, as a note for the model.
 *   start COMMAND       when corbienest starts; stop COMMAND when a request ends. What they print is shown.
 * A hook reads the event as JSON on its standard input: {"event", "tool", "input", "result", "cwd"}. Hooks run for the
 * model's tool calls; the user's own !lines are not hooked. */
#define HOOK_MAX 64
#define HOOK_TIMEOUT 60
static char *g_hook[HOOK_MAX]; static int g_hook_n = 0;

static const char *hook_path(void) {
    static char path[1200];
    char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    unsigned long long h = 1469598103934665603ULL;   /* FNV-1a, the same name as the project's permission rules */
    for (const char *p = cwd; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    snprintf(path, sizeof path, "%s/hooks/%016llx.hooks", config_dir(), h);
    return path;
}
void tools_hooks_load(void) {
    for (int i = 0; i < g_hook_n; i++) free(g_hook[i]);
    g_hook_n = 0;
    size_t n; char *d = read_whole_file(hook_path(), &n, 64 * 1024);
    if (!d) return;
    char *save = NULL;
    for (char *l = strtok_r(d, "\n", &save); l && g_hook_n < HOOK_MAX; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ' || *l == '\t') l++;
        size_t k = strlen(l); while (k && (l[k-1] == ' ' || l[k-1] == '\r' || l[k-1] == '\t')) l[--k] = 0;
        if (!*l || *l == '#') continue;
        g_hook[g_hook_n++] = xstrdup(l);
    }
    free(d);
}
static void hooks_save(void) {
    if (!g_hook_n) { unlink(hook_path()); return; }
    sbuf b; sb_init(&b);
    sb_puts(&b, "# corbienest hooks for this project — one per line: pre TOOL COMMAND, post TOOL COMMAND, start COMMAND, stop COMMAND (see /hooks)\n");
    for (int i = 0; i < g_hook_n; i++) sb_printf(&b, "%s\n", g_hook[i]);
    char dir[1100]; snprintf(dir, sizeof dir, "%s/hooks", config_dir());
    mkdir_p(dir);
    write_whole_file_atomic(hook_path(), b.data, b.len);
    sb_free(&b);
}
int tools_hooks_count(void) { return g_hook_n; }
const char *tools_hooks_get(int i) { return i >= 0 && i < g_hook_n ? g_hook[i] : NULL; }
bool tools_hooks_add(const char *line) {
    for (int i = 0; i < g_hook_n; i++) if (!strcmp(g_hook[i], line)) return false;
    if (g_hook_n >= HOOK_MAX) return false;
    g_hook[g_hook_n++] = xstrdup(line);
    hooks_save();
    return true;
}
bool tools_hooks_remove(int i) {
    if (i < 0 || i >= g_hook_n) return false;
    free(g_hook[i]); memmove(g_hook + i, g_hook + i + 1, sizeof(char*) * (size_t)(g_hook_n - i - 1)); g_hook_n--;
    hooks_save();
    return true;
}
void tools_hooks_clear(void) { for (int i = 0; i < g_hook_n; i++) free(g_hook[i]); g_hook_n = 0; unlink(hook_path()); }

/* Is this hook line an `event` hook (for `tool`, when it is a pre or post hook)? *cmd is its command. */
bool hook_match(const char *line, const char *event, const char *tool, const char **cmd) {
    if (!line || !event) return false;
    size_t el = strlen(event);
    if (strncmp(line, event, el) || line[el] != ' ') return false;
    const char *r = line + el + 1;
    if (!strcmp(event, "pre") || !strcmp(event, "post")) {
        const char *sp = strchr(r, ' ');
        if (!sp) return false;
        if (tool) {
            char pat[128]; size_t pl = (size_t)(sp - r);
            if (pl >= sizeof pat) return false;
            memcpy(pat, r, pl); pat[pl] = 0;
            if (!glob_match(pat, tool)) return false;
        }
        r = sp + 1; while (*r == ' ') r++;
    }
    if (!*r) return false;
    if (cmd) *cmd = r;
    return true;
}

/* Runs a hook's command with the event on its standard input. Returns the exit code, or run_shell's (124 timed out,
 * 130 interrupted, -1 could not run); what it printed goes to out. */
static int hook_run(const char *cmd, cJSON *ev, sbuf *out, bool *interrupted) {
    char *txt = cJSON_PrintUnformatted(ev);
    const char *tmpdir = getenv("TMPDIR");
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s/corbienest_hook_XXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    int fd = mkstemp(tmp);
    if (fd < 0) { free(txt); *interrupted = false; return -1; }
    size_t off = 0, len = strlen(txt);
    while (off < len) { ssize_t w = write(fd, txt + off, len - off); if (w <= 0) break; off += (size_t)w; }
    close(fd); free(txt);
    char *q = shell_escape(tmp);
    sbuf full; sb_init(&full);
    sb_printf(&full, "( %s\n) < %s", cmd, q);   /* the newline keeps a comment at the end of cmd from eating the ) */
    int rc = run_shell(full.data, HOOK_TIMEOUT, out, interrupted, OUT_CAP * 2, "running a hook", "hook");
    sb_free(&full); free(q);
    unlink(tmp);
    return rc;
}
/* The event as the hook reads it. `result` is cut to a bounded size. */
static cJSON *hook_event(const char *event, const char *tool, cJSON *input, const char *result) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "event", event);
    if (tool) cJSON_AddStringToObject(e, "tool", tool);
    if (input) cJSON_AddItemToObject(e, "input", cJSON_Duplicate(input, 1));
    if (result) { char *r = xstrndup(result, 8192); cJSON_AddStringToObject(e, "result", r); free(r); }
    char cwd[1024]; if (getcwd(cwd, sizeof cwd)) cJSON_AddStringToObject(e, "cwd", cwd);
    return e;
}
static void hook_say(const char *text, const char *color) {
    if (!text || !*text) return;
    const char *nl = strchr(text, '\n');
    printf("  %s%.*s" C_RESET "\n", color, (int)(nl ? (size_t)(nl - text) : strlen(text)), text);
}
/* Before a tool call: true if a hook stops it, and why goes to `why`. */
static bool hook_pre(const char *tool, cJSON *input, sbuf *why) {
    for (int i = 0; i < g_hook_n; i++) {
        const char *cmd;
        if (!hook_match(g_hook[i], "pre", tool, &cmd)) continue;
        cJSON *e = hook_event("pre", tool, input, NULL);
        sbuf out; sb_init(&out); bool intr = false;
        int rc = hook_run(cmd, e, &out, &intr);
        cJSON_Delete(e);
        const char *said = out.data && *out.data ? out.data : "";
        if (rc == 0) { sb_free(&out); continue; }
        if (rc == 2 || rc == 124 || rc == 130 || rc == -1) {
            if (rc == 2) sb_printf(why, "blocked by a hook (%s): %s", cmd, *said ? said : "no reason given");
            else sb_printf(why, "blocked: the hook (%s) %s", cmd, rc == 124 ? "timed out" : rc == 130 ? "was interrupted" : "could not run");
            printf("  " C_YELLOW "hook" C_RESET " " C_RED "stopped %s" C_RESET "\n", tool);
            hook_say(why->data, C_DIM);
            sb_free(&out);
            return true;
        }
        printf("  " C_DIM "hook exited %d: %s" C_RESET "\n", rc, cmd);
        sb_free(&out);
    }
    return false;
}
/* After a tool call: what the hooks print is added to its result, for the model. */
static void hook_post(const char *tool, cJSON *input, sbuf *result) {
    for (int i = 0; i < g_hook_n; i++) {
        const char *cmd;
        if (!hook_match(g_hook[i], "post", tool, &cmd)) continue;
        cJSON *e = hook_event("post", tool, input, result->data ? result->data : "");
        sbuf out; sb_init(&out); bool intr = false;
        int rc = hook_run(cmd, e, &out, &intr);
        cJSON_Delete(e);
        if (out.len) sb_printf(result, "\n[hook %s: exit %d]\n%s", cmd, rc, out.data);
        else if (rc != 0) sb_printf(result, "\n[hook %s: exit %d]\n", cmd, rc);
        sb_free(&out);
    }
}
/* The start and stop events: the hooks run, and what they print is shown. */
void tools_hook_event(const char *event) {
    for (int i = 0; i < g_hook_n; i++) {
        const char *cmd;
        if (!hook_match(g_hook[i], event, NULL, &cmd)) continue;
        cJSON *e = hook_event(event, NULL, NULL, NULL);
        sbuf out; sb_init(&out); bool intr = false;
        int rc = hook_run(cmd, e, &out, &intr);
        cJSON_Delete(e);
        if (out.len) { sb_putc(&out, '\n'); for (char *l = out.data; *l; ) { char *nl = strchr(l, '\n'); if (!nl) break; *nl = 0; printf("  " C_DIM "%s" C_RESET "\n", l); l = nl + 1; } }
        if (rc != 0) printf("  " C_DIM "hook %s exited %d" C_RESET "\n", cmd, rc);
        sb_free(&out);
    }
}

tool_status tools_execute(const char *name, cJSON *args, sbuf *out) {
    cJSON *tmp = NULL;
    if (!args || !cJSON_IsObject(args)) { tmp = cJSON_CreateObject(); args = tmp; }
    bool hooked = !tools_no_confirm && g_hook_n > 0;   /* the model's calls; the user's own !lines are not hooked */
    if (hooked) {
        sbuf why; sb_init(&why);
        if (hook_pre(name, args, &why)) { sb_clear(out); sb_puts(out, why.data); sb_free(&why); if (tmp) cJSON_Delete(tmp); return TOOL_DENIED; }
        sb_free(&why);
    }
    tool_status st;
    if (!strcmp(name, "read_file")) st = t_read_file(args, out);
    else if (!strcmp(name, "write_file")) st = t_write_file(args, out);
    else if (!strcmp(name, "edit_file")) st = t_edit_file(args, out);
    else if (!strcmp(name, "list_dir")) st = t_list_dir(args, out);
    else if (!strcmp(name, "grep")) st = t_grep(args, out);
    else if (!strcmp(name, "bash") || !strcmp(name, "shell") || !strcmp(name, "run_command")) st = t_bash(args, out);
    else if (!strcmp(name, "web_fetch") || !strcmp(name, "fetch_url") || !strcmp(name, "fetch")) st = t_web_fetch(args, out);
    else if (!strcmp(name, "web_search") || !strcmp(name, "search_web") || !strcmp(name, "search")) st = t_web_search(args, out);
    else if (!strcmp(name, "task")) st = t_task(args, out);
    else if (!strcmp(name, "advisor")) st = t_advisor(args, out);
    else if (!strcmp(name, "todo_write")) st = t_todo_write(args, out);
    else if (!strcmp(name, "bash_job")) st = t_bash_job(args, out);
    else if (mcp_is_tool(name)) st = t_mcp(name, args, out);
    else if (!strcmp(name, "exit")) st = t_exit(out);
    else { sb_printf(out, "error: unknown tool '%s'. Available tools: %s", name, tools_summary_line()); st = TOOL_ERROR; }
    if (hooked) hook_post(name, args, out);
    if (tmp) cJSON_Delete(tmp);
    /* what a tool returned goes to the terminal and into the request: a binary file's bytes are not UTF-8 */
    if (out->data && !utf8_ok(out->data, out->len)) { char *fx = utf8_fix(out->data); sb_clear(out); sb_puts(out, fx); free(fx); }
    return st;
}
