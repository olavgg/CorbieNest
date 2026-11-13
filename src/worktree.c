/* Git worktrees as scratch copies of the project: what /goal's candidates and the sub-agents that write
 * (task with isolation=worktree) work in.
 *
 * The work is snapshotted first: the working tree as it is, uncommitted and untracked files included, in a
 * commit made through a temporary index, so neither the index nor HEAD of the user's repository is touched
 * (wt_snapshot). A copy is a detached worktree of that commit (wt_add), removed again with wt_remove. What a
 * copy did is committed in it (wt_commit) and brought back as a patch against the snapshot (wt_changed,
 * wt_apply_check, wt_apply): all or nothing, so a patch that does not fit leaves the tree as it was, and the
 * caller keeps the commit as a branch (wt_branch).
 *
 * Every git command runs through tools_shell_in (the timeout, the busy label), from the repository's top: `git -C
 * TOP`, so a project that lives in a subfolder is no different. They are held (tools_shell_hold): a step is not cut
 * off by Esc, which would leave a half-applied patch or a lost copy; the key stays for the caller to see. The user's
 * git configuration is kept out where it would change what is done: the patch is plumbing (diff-tree, a/ b/
 * prefixes), and the hooks of the repository do not run for a copy (core.hooksPath=/dev/null). Nothing here prints. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* a string as one shell word */
static void shq(sbuf *b, const char *s) {
    sb_putc(b, '\'');
    for (; *s; s++) { if (*s == '\'') sb_puts(b, "'\\''"); else sb_putc(b, *s); }
    sb_putc(b, '\'');
}

/* a copy of s without its trailing newlines and blanks, malloc'd */
static char *trim_end(const char *s) {
    size_t n = s ? strlen(s) : 0;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    char *t = xmalloc(n + 1); memcpy(t, s ? s : "", n); t[n] = 0;
    return t;
}

/* runs cmd (not in a directory of its own); the exit code, the output in out */
static int run(sbuf *cmd, int timeout_s, sbuf *out, const char *busy, const char *verb) {
    bool intr = false;
    tools_shell_hold(true);
    int rc = tools_shell_in(NULL, cmd->data, timeout_s, out, &intr, busy, verb);
    tools_shell_hold(false);
    return intr ? 130 : rc;
}
/* a private folder (0700) for temporary files: the index copy, a list of names; false if it cannot be made */
static bool tmp_dir(char d[1024]) {
    const char *td = getenv("TMPDIR");
    snprintf(d, 1024, "%s/corbie-wt-XXXXXX", td && *td ? td : "/tmp");
    return mkdtemp(d) != NULL;
}
static void tmp_dir_rm(const char *d) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "rm -rf "); shq(&c, d);
    run(&c, 60, &o, "cleaning up", "worktree");
    sb_free(&c); sb_free(&o);
}
/* cmd's standard output, whole (a command's output in a buffer is capped; a list of names must not be): malloc'd, its
 * length in *len; NULL when the command failed */
static char *run_whole(sbuf *cmd, int timeout_s, const char *busy, const char *verb, size_t *len) {
    char d[1024];
    *len = 0;
    if (!tmp_dir(d)) return NULL;
    char f[1100]; snprintf(f, sizeof f, "%s/out", d);
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_printf(&c, "{ %s ; } > ", cmd->data); shq(&c, f);
    int rc = run(&c, timeout_s, &o, busy, verb);
    char *data = rc == 0 ? read_whole_file(f, len, 0) : NULL;
    if (rc == 0 && !data) { data = xstrdup(""); *len = 0; }
    unlink(f); rmdir(d);
    sb_free(&c); sb_free(&o);
    return data;
}
/* a NUL-separated list of names, sorted, so a name is looked up by bisection */
typedef struct { const char **v; int n; } namelist;
static int name_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void names_index(const char *buf, size_t len, namelist *nl) {
    nl->n = 0; nl->v = NULL;
    int cap = 0;
    for (size_t at = 0; buf && at < len; ) {
        const char *p = buf + at; size_t l = strnlen(p, len - at);
        if (l) { if (nl->n == cap) { cap = cap ? cap * 2 : 64; nl->v = xrealloc(nl->v, sizeof *nl->v * (size_t)cap); } nl->v[nl->n++] = p; }
        at += l + 1;
    }
    if (nl->n > 1) qsort(nl->v, (size_t)nl->n, sizeof *nl->v, name_cmp);
}
static bool names_has(const namelist *nl, const char *name) {
    return nl->n && bsearch(&name, nl->v, (size_t)nl->n, sizeof *nl->v, name_cmp) != NULL;
}
/* an object id: 40 (SHA-1) or 64 (SHA-256) hex digits */
static bool is_id(const char *s) {
    size_t n = 0;
    for (; s[n]; n++) if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return false;
    return n == 40 || n == 64;
}
/* the ids on the last line of a command's output (what git said on stderr comes before it): want of them, each checked */
static bool last_ids(const char *out, int want, char *a, char *b) {
    char *t = trim_end(out ? out : "");
    char *line = strrchr(t, '\n'); line = line ? line + 1 : t;
    char x[128] = "", y[128] = "", z[8] = "";
    int k = sscanf(line, "%127s %127s %7s", x, y, z);
    bool ok = k == want && is_id(x) && (want == 1 || is_id(y));
    if (ok) { snprintf(a, WT_ID, "%s", x); if (want == 2) snprintf(b, WT_ID, "%s", y); }
    free(t);
    return ok;
}
/* Fills d/index as the user's index brought up to the working tree, with no flag of theirs in the way: a tracked file
 * that matches .gitignore stays tracked (it is in the index already), and an entry marked assume-unchanged, or
 * skip-worktree while its file is there, is read from the disk like any other. The command is appended to c, after a
 * cd to top; false when the flags cannot be read. */
static bool temp_index(sbuf *c, const char *top, const char *d) {
    sbuf l; sb_init(&l);
    sb_puts(&l, "git -C "); shq(&l, top); sb_puts(&l, " ls-files -z -v");
    size_t len = 0;
    char *v = run_whole(&l, 120, "reading the index", "snapshot", &len);
    sb_free(&l);
    if (!v) return false;
    sbuf au, sw; sb_init(&au); sb_init(&sw);
    for (size_t at = 0; at < len; ) {
        const char *p = v + at; size_t n = strnlen(p, len - at);
        if (n > 2 && p[1] == ' ') {
            const char *name = p + 2;
            if (p[0] >= 'a' && p[0] <= 'z') { sb_puts(&au, name); sb_putc(&au, 0); }
            else if (p[0] == 'S') {
                struct stat st; sbuf f; sb_init(&f); sb_printf(&f, "%s/%s", top, name);
                if (lstat(f.data, &st) == 0) { sb_puts(&sw, name); sb_putc(&sw, 0); }
                sb_free(&f);
            }
        }
        at += n + 1;
    }
    free(v);
    char fa[1100], fs[1100]; snprintf(fa, sizeof fa, "%s/au", d); snprintf(fs, sizeof fs, "%s/sw", d);
    bool ok = (!au.len || write_whole_file(fa, au.data, au.len) == 0) && (!sw.len || write_whole_file(fs, sw.data, sw.len) == 0);
    sbuf ix; sb_init(&ix); sb_printf(&ix, "%s/index", d);
    sb_puts(c, "cd "); shq(c, top);
    sb_puts(c, " && I=$(git rev-parse --git-path index) && { if [ -f \"$I\" ]; then cp \"$I\" "); shq(c, ix.data); sb_puts(c, "; fi; }");
    if (au.len) { sb_puts(c, " && GIT_INDEX_FILE="); shq(c, ix.data); sb_puts(c, " git update-index -z --no-assume-unchanged --stdin < "); shq(c, fa); }
    if (sw.len) { sb_puts(c, " && GIT_INDEX_FILE="); shq(c, ix.data); sb_puts(c, " git update-index -z --no-skip-worktree --stdin < "); shq(c, fs); }
    sb_puts(c, " && GIT_INDEX_FILE="); shq(c, ix.data); sb_puts(c, " git add -A >/dev/null 2>&1 && export GIT_INDEX_FILE="); shq(c, ix.data);
    sb_free(&ix); sb_free(&au); sb_free(&sw);
    return ok;
}

char *wt_toplevel(void) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git rev-parse --show-toplevel 2>/dev/null");
    int rc = run(&c, 30, &o, "reading the repository", "repository");
    char *top = trim_end(rc == 0 ? o.data : "");
    sb_free(&c); sb_free(&o);
    if (!top[0]) { free(top); return NULL; }
    return top;
}

char *wt_prefix(void) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git rev-parse --show-prefix 2>/dev/null");
    int rc = run(&c, 30, &o, "reading the repository", "repository");
    char *p = trim_end(rc == 0 ? o.data : "");
    sb_free(&c); sb_free(&o);
    return p;   /* "" at the top, "sub/dir/" below it */
}

bool wt_ignored(const char *top, const char *prefix) {
    if (!prefix || !*prefix) return false;
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " check-ignore -q -- "); shq(&c, prefix);
    bool ign = run(&c, 30, &o, "reading the repository", "repository") == 0;
    sb_free(&c); sb_free(&o);
    return ign;
}

bool wt_snapshot(const char *top, char tree[WT_ID], char snap[WT_ID]) {
    tree[0] = snap[0] = 0;
    char d[1024];
    if (!tmp_dir(d)) return false;
    sbuf c, o; sb_init(&c); sb_init(&o);
    bool ok = temp_index(&c, top, d);
    sb_puts(&c, " && TREE=$(git write-tree) && P= && if git rev-parse -q --verify HEAD >/dev/null; then P='-p HEAD'; fi"
                " && S=$(" WT_GIT "git -c commit.gpgsign=false commit-tree $TREE $P -m corbie-snapshot) && echo \"$TREE $S\"");
    ok = ok && run(&c, 300, &o, "snapshotting the working tree", "snapshot") == 0 && last_ids(o.data, 2, tree, snap);
    if (!ok) tree[0] = snap[0] = 0;
    tmp_dir_rm(d);
    sb_free(&c); sb_free(&o);
    return ok;
}

bool wt_tree_of_working(const char *top, char tree[WT_ID]) {
    tree[0] = 0;
    char d[1024];
    if (!tmp_dir(d)) return false;
    sbuf c, o; sb_init(&c); sb_init(&o);
    bool ok = temp_index(&c, top, d);
    sb_puts(&c, " && git write-tree");
    ok = ok && run(&c, 300, &o, "checking the working tree", "checking") == 0 && last_ids(o.data, 1, tree, NULL);
    if (!ok) tree[0] = 0;
    tmp_dir_rm(d);
    sb_free(&c); sb_free(&o);
    return ok;
}

bool wt_add(const char *top, const char *dir, const char *commit) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    char *parent = xstrdup(dir); char *sl = strrchr(parent, '/');
    if (sl && sl != parent) { *sl = 0; mkdir_p(parent); }
    free(parent);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " -c core.hooksPath=/dev/null worktree add --detach "); shq(&c, dir); sb_putc(&c, ' '); shq(&c, commit); sb_puts(&c, " >/dev/null 2>&1");
    bool ok = run(&c, 120, &o, "making a copy of the project", "worktree") == 0;
    sb_free(&c); sb_free(&o);
    return ok;
}

void wt_remove(const char *top, const char *dir) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " worktree remove --force "); shq(&c, dir); sb_puts(&c, " >/dev/null 2>&1");
    run(&c, 120, &o, "removing a copy", "worktree");
    sb_free(&c); sb_free(&o);
}

void wt_prune(const char *top) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " worktree prune >/dev/null 2>&1");
    run(&c, 60, &o, "cleaning up", "worktree");
    sb_free(&c); sb_free(&o);
}

bool wt_gitdir(const char *dir, char *out, size_t n) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git -C "); shq(&c, dir); sb_puts(&c, " rev-parse --absolute-git-dir 2>/dev/null");
    int rc = run(&c, 30, &o, "reading the copy", "worktree");
    char *g = trim_end(rc == 0 ? o.data : "");
    bool ok = g[0] == '/' && strlen(g) < n;
    snprintf(out, n, "%s", ok ? g : "");
    free(g); sb_free(&c); sb_free(&o);
    return ok;
}

bool wt_commit(const char *dir, const char *gitdir, const char *snap, bool (*keep)(const char *name, void *ctx), void *ctx,
               sbuf *left, char all[WT_ID], char commit[WT_ID]) {
    sbuf env, c, o; sb_init(&env); sb_init(&c); sb_init(&o);
    commit[0] = 0;
    if (all) all[0] = 0;
    /* every step must work, or there is no commit: a failed one must not read as "nothing changed". With the git
     * directory given, the copy's own .git file (which a command in it could have rewritten) is not trusted. */
    sb_puts(&env, "cd "); shq(&env, dir);
    if (gitdir && *gitdir) { sb_puts(&env, " && export GIT_DIR="); shq(&env, gitdir); sb_puts(&env, " GIT_WORK_TREE=."); }
    sb_puts(&env, " && ");
    const char *COMMIT = WT_GIT "git -c core.hooksPath=/dev/null -c commit.gpgsign=false commit -q --no-verify --allow-empty -m corbie-copy >/dev/null 2>&1 && git rev-parse --verify -q HEAD 2>/dev/null";
    sb_printf(&c, "%sgit add -A >/dev/null 2>&1", env.data);
    bool ok = run(&c, 300, &o, "collecting what the copy changed", "commit") == 0;
    char d[1024] = "";
    sbuf drop; sb_init(&drop); int nd = 0;
    if (ok && keep && snap) {
        /* A file that is new in the copy — the snapshot did not have it, nor did the user's HEAD — and that no tool call
         * wrote (a build's output, a test's cache) is left out of the commit, on disk only. A file the snapshot had is
         * never left out: taking it out would read as its deletion. */
        size_t nl = 0, hl = 0;
        sb_clear(&c); sb_printf(&c, "%sgit diff --cached --name-only -z --no-renames --diff-filter=A ", env.data); shq(&c, snap);
        char *news = run_whole(&c, 120, "collecting what the copy changed", "commit", &nl);
        sb_clear(&c); sb_printf(&c, "%sP=$(git rev-parse -q --verify ", env.data); shq(&c, snap);
        sb_puts(&c, "^ 2>/dev/null || git hash-object -t tree /dev/null) && git diff --cached --name-only -z --no-renames --diff-filter=A $P");
        char *head_news = run_whole(&c, 120, "collecting what the copy changed", "commit", &hl);
        if (!news || !head_news) ok = false;
        namelist hn; names_index(head_news, hl, &hn);
        for (size_t at = 0; ok && at < nl; ) {
            const char *p = news + at; size_t l = strnlen(p, nl - at);
            if (l && names_has(&hn, p) && !keep(p, ctx)) {
                sb_append(&drop, p, l); sb_putc(&drop, 0); nd++;
                if (left) { sb_append(left, p, l); sb_putc(left, '\n'); }
            }
            at += l + 1;
        }
        free(hn.v); free(news); free(head_news);
    }
    if (ok && nd) {
        /* the whole copy first (kept as a branch by the caller, so what was left out is not lost), then without them */
        char fl[1100] = "";
        ok = tmp_dir(d);
        if (ok) { snprintf(fl, sizeof fl, "%s/drop", d); ok = write_whole_file(fl, drop.data, drop.len) == 0; }
        sb_clear(&c); sb_clear(&o); sb_printf(&c, "%s%s", env.data, COMMIT);
        if (ok) ok = run(&c, 300, &o, "collecting what the copy changed", "commit") == 0 && last_ids(o.data, 1, all ? all : commit, NULL);
        if (!all) commit[0] = 0;
        sb_clear(&c); sb_clear(&o);
        sb_printf(&c, "%sgit --literal-pathspecs rm -q -r --cached --pathspec-file-nul --pathspec-from-file=", env.data); shq(&c, fl);
        if (ok) ok = run(&c, 300, &o, "collecting what the copy changed", "commit") == 0;
    }
    if (d[0]) tmp_dir_rm(d);
    sb_free(&drop);
    sb_clear(&c); sb_clear(&o);
    sb_printf(&c, "%s%s", env.data, COMMIT);
    sb_free(&env);
    if (ok) ok = run(&c, 300, &o, "collecting what the copy changed", "commit") == 0 && last_ids(o.data, 1, commit, NULL);
    if (!ok) { commit[0] = 0; if (all) all[0] = 0; }
    sb_free(&c); sb_free(&o);
    return ok;
}

int wt_changed(const char *top, const char *snap, const char *commit, sbuf *names) {
    /* NUL-separated, so nothing is quoted, and read whole */
    sbuf c; sb_init(&c);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " diff-tree -r -z --name-only --no-renames "); shq(&c, snap); sb_putc(&c, ' '); shq(&c, commit);
    size_t len = 0;
    char *d = run_whole(&c, 120, "reading what the copy changed", "result", &len);
    int n = d ? 0 : -1;
    for (size_t at = 0; d && n >= 0 && at < len; ) {
        const char *p = d + at; size_t l = strnlen(p, len - at);
        if (l && memchr(p, '\n', l)) n = -1;   /* a name with a newline in it cannot be listed one per line: not applied */
        else if (l) { if (names) { sb_append(names, p, l); sb_putc(names, '\n'); } n++; }
        at += l + 1;
    }
    free(d); sb_free(&c);
    return n;
}

/* the patch from snap to commit, piped into git apply (with --check, or for real), from the repository's top */
static bool apply_patch(const char *top, const char *snap, const char *commit, bool check, sbuf *why) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "cd "); shq(&c, top);
    sb_puts(&c, " && git diff-tree -p --binary --full-index --no-renames --src-prefix=a/ --dst-prefix=b/ "); shq(&c, snap); sb_putc(&c, ' '); shq(&c, commit);
    sb_puts(&c, check ? " | git -c apply.whitespace=nowarn apply -p1 --check 2>&1" : " | git -c apply.whitespace=nowarn apply -p1 2>&1");
    int rc = run(&c, 300, &o, check ? "checking that the changes fit" : "applying the changes", "applying");
    if (rc != 0 && why && o.data) sb_put_cut(why, o.data, 400);
    sb_free(&c); sb_free(&o);
    return rc == 0;
}
bool wt_apply_check(const char *top, const char *snap, const char *commit, sbuf *why) { return apply_patch(top, snap, commit, true, why); }
bool wt_apply(const char *top, const char *snap, const char *commit, sbuf *why) { return apply_patch(top, snap, commit, false, why); }

bool wt_branch(const char *top, const char *name, const char *commit) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " branch "); shq(&c, name); sb_putc(&c, ' '); shq(&c, commit); sb_puts(&c, " >/dev/null 2>&1");
    bool ok = run(&c, 60, &o, "keeping the result", "branch") == 0;
    sb_free(&c); sb_free(&o);
    return ok;
}
