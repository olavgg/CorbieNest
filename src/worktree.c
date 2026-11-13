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
 * Every git command runs through tools_shell_in (the interrupt, the timeout, the busy label), from the
 * repository's top: `git -C TOP`, so a project that lives in a subfolder is no different. Nothing here prints. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int rc = tools_shell_in(NULL, cmd->data, timeout_s, out, &intr, busy, verb);
    return intr ? 130 : rc;
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

bool wt_snapshot(const char *top, char tree[WT_ID], char snap[WT_ID]) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    tree[0] = snap[0] = 0;
    sb_puts(&c, "cd "); shq(&c, top);
    sb_puts(&c, " && T=$(mktemp -u) && GIT_INDEX_FILE=$T git add -A && TREE=$(GIT_INDEX_FILE=$T git write-tree) && rm -f $T && P= && if git rev-parse -q --verify HEAD >/dev/null; then P='-p HEAD'; fi"
                " && S=$(" WT_GIT "git -c commit.gpgsign=false commit-tree $TREE $P -m corbie-snapshot) && echo \"$TREE $S\"");
    int rc = run(&c, 300, &o, "snapshotting the working tree", "snapshot");
    bool ok = rc == 0 && sscanf(o.data ? o.data : "", "%63s %63s", tree, snap) == 2;
    if (!ok) tree[0] = snap[0] = 0;
    sb_free(&c); sb_free(&o);
    return ok;
}

bool wt_tree_of_working(const char *top, char tree[WT_ID]) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    tree[0] = 0;
    sb_puts(&c, "cd "); shq(&c, top);
    sb_puts(&c, " && T=$(mktemp -u) && GIT_INDEX_FILE=$T git add -A && GIT_INDEX_FILE=$T git write-tree; rm -f $T");
    int rc = run(&c, 300, &o, "checking the working tree", "checking");
    if (rc == 0 && o.data) sscanf(o.data, "%63s", tree);
    sb_free(&c); sb_free(&o);
    return tree[0] != 0;
}

bool wt_add(const char *top, const char *dir, const char *commit) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    char *parent = xstrdup(dir); char *sl = strrchr(parent, '/');
    if (sl && sl != parent) { *sl = 0; mkdir_p(parent); }
    free(parent);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " worktree add --detach "); shq(&c, dir); sb_putc(&c, ' '); shq(&c, commit); sb_puts(&c, " >/dev/null 2>&1");
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

bool wt_commit(const char *dir, char commit[WT_ID]) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    commit[0] = 0;
    sb_puts(&c, "git -C "); shq(&c, dir);
    sb_puts(&c, " add -A >/dev/null 2>&1 && " WT_GIT "git -C "); shq(&c, dir);
    sb_puts(&c, " -c commit.gpgsign=false commit -q --no-verify --allow-empty -m corbie-copy >/dev/null 2>&1; git -C "); shq(&c, dir);
    sb_puts(&c, " rev-parse --verify -q HEAD 2>/dev/null");
    int rc = run(&c, 300, &o, "collecting what the copy changed", "commit");
    if (rc == 0 && o.data) sscanf(o.data, "%63s", commit);
    sb_free(&c); sb_free(&o);
    return commit[0] != 0;
}

int wt_changed(const char *top, const char *snap, const char *commit, sbuf *names) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "git -C "); shq(&c, top); sb_puts(&c, " -c core.quotepath=off diff --name-only --no-renames "); shq(&c, snap); sb_putc(&c, ' '); shq(&c, commit);
    int rc = run(&c, 120, &o, "reading what the copy changed", "result");
    int n = 0;
    if (rc == 0) for (char *p = o.data; p && *p; ) {
        char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len) { if (names) { sb_append(names, p, len); sb_putc(names, '\n'); } n++; }
        if (!e) break;
        p = e + 1;
    }
    sb_free(&c); sb_free(&o);
    return rc == 0 ? n : -1;
}

/* the patch from snap to commit, piped into git apply (with --check, or for real), from the repository's top */
static bool apply_patch(const char *top, const char *snap, const char *commit, bool check, sbuf *why) {
    sbuf c, o; sb_init(&c); sb_init(&o);
    sb_puts(&c, "cd "); shq(&c, top);
    sb_puts(&c, " && git diff --binary --full-index --no-renames "); shq(&c, snap); sb_putc(&c, ' '); shq(&c, commit);
    sb_puts(&c, check ? " | git apply --check 2>&1" : " | git apply 2>&1");
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
