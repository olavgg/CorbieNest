/* Unit tests for corbienest internals. Build & run with `make test`. */
#define _GNU_SOURCE
#include "../src/common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); if (_a && _b && !strcmp(_a, _b)) g_pass++; else { g_fail++; fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)

/* capture stdout into a buffer while fn runs */
static char *capture(void (*fn)(void *), void *ud) {
    fflush(stdout);
    char path[] = "/tmp/crowtest_XXXXXX";
    int fd = mkstemp(path);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    fn(ud);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO); close(saved);
    close(fd);
    size_t n; char *d = read_whole_file(path, &n, 0);
    unlink(path);
    return d;
}

/* ---------- sbuf ---------- */
static void test_sbuf(void) {
    sbuf b; sb_init(&b);
    sb_puts(&b, "hello"); sb_putc(&b, ' '); sb_printf(&b, "%d-%s", 42, "x");
    CHECK_STR(b.data, "hello 42-x");
    CHECK(b.len == 10);
    for (int i = 0; i < 1000; i++) sb_puts(&b, "0123456789");
    CHECK(b.len == 10010);
    char *d = sb_detach(&b);
    CHECK(strlen(d) == 10010); CHECK(b.len == 0 && b.data == NULL);
    free(d);
    sb_free(&b);
}

/* ---------- markdown printer ---------- */
static void md_run(void *ud) {
    md_state m; md_init(&m); const char *s = ud;
    for (size_t i = 0; s[i]; i++) md_feed(&m, s + i, 1);   /* tiny chunks exercise pending state */
    md_finish(&m);
}
static void test_md(void) {
    char *o = capture(md_run, "a **b** c `d` e\n```\nx*y*\n```\n");
    CHECK(strstr(o, "a " C_BOLD "b" C_RESET " c " C_CYAN "`d`" C_RESET " e") != NULL);   /* bold + inline code */
    CHECK(strstr(o, "x*y*") != NULL);                                                     /* no bold inside fence */
    free(o);
    o = capture(md_run, "2 * 3 * 4");   /* lone stars survive */
    CHECK(strstr(o, "2 * 3 * 4") != NULL);
    free(o);
}

/* the text with its ANSI escapes taken out: what the reader sees, column for column */
static char *plain_of(const char *s) {
    sbuf b; sb_init(&b);
    for (const char *p = s; *p; p++) {
        if (*p == '\x1b' && p[1] == '[') { p += 2; while (*p && !((*p >= '@' && *p <= '~'))) p++; continue; }
        sb_putc(&b, *p);
    }
    return sb_detach(&b);
}
static void test_highlight(void) {
    /* the printer: a fence's language colours its keywords, strings, comments and numbers */
    char *o = capture(md_run, "```python\ndef f(x):  # note\n    return 'a' + 10\n```\n");
    CHECK(strstr(o, C_MAGENTA "def" C_RESET) != NULL && strstr(o, C_MAGENTA "return" C_RESET) != NULL);
    CHECK(strstr(o, C_GRAY "# note" C_RESET) != NULL && strstr(o, C_GREEN "'a'" C_RESET) != NULL && strstr(o, C_YELLOW "10" C_RESET) != NULL);
    char *pl = plain_of(o);
    CHECK(strstr(pl, "def f(x):  # note") != NULL && strstr(pl, "    return 'a' + 10") != NULL);   /* the text is what it was */
    free(pl); free(o);
    o = capture(md_run, "```c\nint x");   /* a last line that never ends is still printed, highlighted */
    CHECK(strstr(o, C_MAGENTA "int" C_RESET) != NULL && strstr(o, "x") != NULL);
    free(o);
    o = capture(md_run, "```\nx = 1  # plain\n```\n");   /* no language: no colour inside */
    CHECK(strstr(o, C_MAGENTA) == NULL && strstr(o, C_GRAY "# plain") == NULL && strstr(o, "x = 1  # plain") != NULL);
    free(o);
    /* the function itself: each language's comments and strings, a string's escaped quote, an identifier in a word */
    const char *cases[][3] = {
        { "c", "int x = 10; // hi \"q\"", "int" },
        { "js", "const s = \"a\\\"b\"; /* c */ let y = 2", "const" },
        { "sh", "if [ -f x ]; then echo '#not' # yes", "if" },
        { "json", "{\"k\": true, \"n\": null}", "true" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        sbuf h; sb_init(&h);
        highlight_line(cases[i][0], cases[i][1], &h, C_CYAN);
        CHECK(strstr(h.data, C_CYAN) != NULL && strstr(h.data, cases[i][2]) != NULL);
        char *pl2 = plain_of(h.data);
        CHECK(strcmp(pl2, cases[i][1]) == 0);
        free(pl2); sb_free(&h);
    }
    sbuf h; sb_init(&h);
    highlight_line("python", "value = 'it''s'  # end", &h, C_CYAN);
    CHECK(strstr(h.data, C_GRAY "# end" C_RESET) != NULL && strstr(h.data, "'it'") != NULL);
    sb_free(&h);
    sbuf k; sb_init(&k);
    highlight_line("c", "int classy = 2; defer_x = iffy", &k, C_CYAN);   /* keywords are whole words only */
    CHECK(strstr(k.data, C_MAGENTA "int" C_RESET) != NULL && strstr(k.data, C_MAGENTA "classy") == NULL && strstr(k.data, C_MAGENTA "iffy") == NULL);
    sb_free(&k);
}

/* ---------- the git branch of a directory (the status bar's location line) ---------- */
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (f) { fputs(text, f); fclose(f); }
}
static void test_gitbranch(void) {
    char root[] = "/tmp/crowtest_git_XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char p[600], b[256];
    /* a repository on a branch with a slash in its name, and a directory below it */
    snprintf(p, sizeof p, "%s/repo", root); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/repo/.git", root); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/repo/.git/HEAD", root); write_text(p, "ref: refs/heads/feature/status\n");
    snprintf(p, sizeof p, "%s/repo/src", root); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/repo/src/deep", root); mkdir(p, 0700);
    b[0] = 0;
    CHECK(git_branch(p, b, sizeof b) == 1 && !strcmp(b, "feature/status"));   /* found above the directory itself */
    snprintf(p, sizeof p, "%s/repo", root);
    CHECK(git_branch(p, b, sizeof b) == 1 && !strcmp(b, "feature/status"));
    /* a detached HEAD: the commit, seven digits */
    snprintf(p, sizeof p, "%s/repo/.git/HEAD", root); write_text(p, "0123456789abcdef0123456789abcdef01234567\n");
    snprintf(p, sizeof p, "%s/repo", root);
    CHECK(git_branch(p, b, sizeof b) == 1 && !strcmp(b, "0123456"));
    /* a linked worktree: .git is a file that names the git directory */
    snprintf(p, sizeof p, "%s/wt", root); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/wt/.git", root); write_text(p, "gitdir: /tmp/nonexistent-but-named\n");
    CHECK(git_branch(p, b, sizeof b) == 0);   /* the directory named is not there: no branch, not a crash */
    /* a directory outside any repository */
    snprintf(p, sizeof p, "%s", root);
    CHECK(git_branch(p, b, sizeof b) == 0);
    CHECK(git_branch(NULL, b, sizeof b) == 0);
}

/* ---------- text tool call recovery ---------- */
static void test_text_tool_calls(void) {
    cJSON *a = parse_text_tool_calls("blah\n<tool_call>\n<function=grep>\n<parameter=pattern>\nrange\n</parameter>\n<parameter=path>\n.\n</parameter>\n</function>\n</tool_call>");
    CHECK(a && cJSON_GetArraySize(a) == 1);
    if (a) {
        cJSON *fn = cJSON_GetObjectItem(cJSON_GetArrayItem(a, 0), "function");
        CHECK_STR(cJSON_GetObjectItem(fn, "name")->valuestring, "grep");
        cJSON *args = cJSON_GetObjectItem(fn, "arguments");
        CHECK_STR(cJSON_GetObjectItem(args, "pattern")->valuestring, "range");
        CHECK_STR(cJSON_GetObjectItem(args, "path")->valuestring, ".");
        cJSON_Delete(a);
    }
    a = parse_text_tool_calls("<tool_call>{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}</tool_call>");
    CHECK(a && cJSON_GetArraySize(a) == 1);
    if (a) { CHECK_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(a,0),"function"),"arguments"),"command")->valuestring, "ls"); cJSON_Delete(a); }
    /* multi-line parameter value keeps inner newlines */
    a = parse_text_tool_calls("<function=write_file><parameter=path>x.txt</parameter><parameter=content>\nline1\nline2\n</parameter></function>");
    CHECK(a != NULL);
    if (a) { CHECK_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(a,0),"function"),"arguments"),"content")->valuestring, "line1\nline2"); cJSON_Delete(a); }
    CHECK(parse_text_tool_calls("plain text with <b>html</b>") == NULL);
    CHECK(parse_text_tool_calls("") == NULL);
}

/* ---------- tools ---------- */
static void test_tools_body(void *ud);
static void test_tools(void) { char *o = capture(test_tools_body, NULL); CHECK(strstr(o, "Run this command?") != NULL); free(o); }

/* write_file's preview: a write that runs unasked is folded (its head says how many lines, the body is a fold),
 * and a write the user is asked about shows its first lines as before, unfolded */
static void write_preview_body(void *ud) {
    const char *which = ud;
    char dir[] = "/tmp/crowtest_wp_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char old[4096]; CHECK(getcwd(old, sizeof old) != NULL);
    CHECK(chdir(dir) == 0);
    sbuf body; sb_init(&body);
    for (int i = 0; i < 30; i++) sb_printf(&body, "line %d of the file\n", i);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "path", "long.txt");
    cJSON_AddStringToObject(a, "content", body.data);
    sbuf out; sb_init(&out);
    if (!strcmp(which, "unasked")) { g_cfg.mode = MODE_AUTO; g_cfg.interactive = false; }
    else { g_cfg.mode = MODE_MANUAL; g_cfg.interactive = true; }
    tools_execute("write_file", a, &out);
    cJSON_Delete(a); sb_free(&out); sb_free(&body);
    CHECK(chdir(old) == 0);
    g_cfg.mode = MODE_AUTO; g_cfg.interactive = false;
}
static void test_write_preview(void) {
    /* no full screen here (the unit build has none), so term_fold_begin is false and the preview keeps its lines: a
     * fold is a screen feature and is checked in the pty. What the unit build can prove is the decision: an asked
     * write never folds, and an unasked one shows the same first line either way. */
    char *o = capture(write_preview_body, (void *)"unasked");
    CHECK(strstr(o, "line 0 of the file") != NULL);
    CHECK(strstr(o, TERM_FOLD_CLOSED) == NULL);                 /* no screen: no fold, as term_fold_begin says */
    free(o);
    char *q = capture(write_preview_body, (void *)"asked");
    CHECK(strstr(q, TERM_FOLD_CLOSED) == NULL);                 /* asked: never folded */
    CHECK(strstr(q, "line 0 of the file") != NULL);
    free(q);
}
static void test_tools_body(void *ud) {
    (void)ud;
    char dir[] = "/tmp/crowtest_dir_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char old[4096]; CHECK(getcwd(old, sizeof old) != NULL);
    CHECK(chdir(dir) == 0);
    g_cfg.mode = MODE_AUTO;   /* no confirmations */
    sbuf out; sb_init(&out);
    cJSON *a;

    /* write_file creates parent dirs */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt"); cJSON_AddStringToObject(a, "content", "one\ntwo\nthree\n");
    CHECK(tools_execute("write_file", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(is_file("sub/dir/f.txt"));
    sb_clear(&out);

    /* read_file with numbers, offset/limit */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt");
    CHECK(tools_execute("read_file", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(strstr(out.data, "     1| one\n     2| two\n     3| three\n") != NULL);
    sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt"); cJSON_AddNumberToObject(a, "offset", 2); cJSON_AddNumberToObject(a, "limit", 1);
    CHECK(tools_execute("read_file", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK_STR(out.data, "     2| two\n");
    sb_clear(&out);

    /* edit_file: unique replace, not-found, ambiguous, replace_all */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt"); cJSON_AddStringToObject(a, "old_string", "two"); cJSON_AddStringToObject(a, "new_string", "2");
    CHECK(tools_execute("edit_file", a, &out) == TOOL_OK); cJSON_Delete(a); sb_clear(&out);
    size_t n; char *d = read_whole_file("sub/dir/f.txt", &n, 0); CHECK_STR(d, "one\n2\nthree\n"); free(d);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt"); cJSON_AddStringToObject(a, "old_string", "nope"); cJSON_AddStringToObject(a, "new_string", "x");
    CHECK(tools_execute("edit_file", a, &out) == TOOL_ERROR); cJSON_Delete(a); CHECK(strstr(out.data, "not found") != NULL); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt"); cJSON_AddStringToObject(a, "old_string", "e"); cJSON_AddStringToObject(a, "new_string", "E");
    CHECK(tools_execute("edit_file", a, &out) == TOOL_ERROR); cJSON_Delete(a); CHECK(strstr(out.data, "occurs") != NULL); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub/dir/f.txt"); cJSON_AddStringToObject(a, "old_string", "e"); cJSON_AddStringToObject(a, "new_string", "E"); cJSON_AddBoolToObject(a, "replace_all", true);
    CHECK(tools_execute("edit_file", a, &out) == TOOL_OK); cJSON_Delete(a); sb_clear(&out);
    d = read_whole_file("sub/dir/f.txt", &n, 0); CHECK_STR(d, "onE\n2\nthrEE\n"); free(d);

    /* list_dir */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "sub");
    CHECK(tools_execute("list_dir", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(strstr(out.data, "dir/") != NULL); sb_clear(&out);

    /* grep */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "pattern", "thr.E"); cJSON_AddStringToObject(a, "path", ".");
    CHECK(tools_execute("grep", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(strstr(out.data, "f.txt:3:thrEE") != NULL); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "pattern", "zzz");
    CHECK(tools_execute("grep", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(strstr(out.data, "No matches") != NULL); sb_clear(&out);

    /* exit: the program is ended after the turn, and only the main agent may ask */
    tools_exit_clear();
    CHECK(tools_execute("exit", NULL, &out) == TOOL_OK && tools_exit_asked()); sb_clear(&out);
    tools_exit_clear();
    CHECK(!tools_exit_asked());
    {
        cJSON *defs = tools_definitions(); bool found = false; cJSON *t;
        cJSON_ArrayForEach(t, defs) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            if (cJSON_IsString(nm) && !strcmp(nm->valuestring, "exit")) found = true;
        }
        CHECK(found); cJSON_Delete(defs);
    }

    /* bash: output + exit code, stderr merged, timeout */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", "echo out; echo err 1>&2; exit 3");
    CHECK(tools_execute("bash", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(strstr(out.data, "out\n") && strstr(out.data, "err\n") && strstr(out.data, "exit code: 3")); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", "sleep 5; echo late"); cJSON_AddNumberToObject(a, "timeout", 1);
    CHECK(tools_execute("bash", a, &out) == TOOL_OK); cJSON_Delete(a);
    CHECK(strstr(out.data, "timed out") && strstr(out.data, "exit code: 124") && !strstr(out.data, "late")); sb_clear(&out);
    /* shell quoting in grep pattern with quotes */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "pattern", "it's"); cJSON_AddStringToObject(a, "path", ".");
    CHECK(tools_execute("grep", a, &out) == TOOL_OK); cJSON_Delete(a); sb_clear(&out);

    /* non-interactive denial when not yolo */
    g_cfg.mode = MODE_MANUAL; g_cfg.interactive = false;
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", "echo hi");
    CHECK(tools_execute("bash", a, &out) == TOOL_DENIED); cJSON_Delete(a);
    CHECK(strstr(out.data, "denied") != NULL); sb_clear(&out);
    g_cfg.mode = MODE_AUTO;

    /* unknown tool */
    a = cJSON_CreateObject();
    CHECK(tools_execute("nope", a, &out) == TOOL_ERROR); cJSON_Delete(a); sb_clear(&out);

    /* read binary / missing */
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "missing.txt");
    CHECK(tools_execute("read_file", a, &out) == TOOL_ERROR); cJSON_Delete(a); sb_clear(&out);
    write_whole_file("bin.dat", "ab\0cd", 5);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "bin.dat");
    CHECK(tools_execute("read_file", a, &out) == TOOL_ERROR); cJSON_Delete(a); CHECK(strstr(out.data, "binary") != NULL); sb_clear(&out);

    /* todo_write: the list is checked, then kept (and an empty one clears it) */
    cJSON *tl = cJSON_Parse("{\"todos\":[{\"content\":\"read\",\"status\":\"completed\"},{\"content\":\"edit\",\"status\":\"in_progress\"},{\"content\":\"test\",\"status\":\"pending\"}]}");
    CHECK(tools_execute("todo_write", tl, &out) == TOOL_OK && tools_todos_get() && cJSON_GetArraySize(tools_todos_get()) == 3);
    CHECK(strstr(out.data, "Tasks (1 of 3 done)") && strstr(out.data, "◐ edit")); sb_clear(&out); cJSON_Delete(tl);
    tl = cJSON_Parse("{\"todos\":[{\"content\":\"a\",\"status\":\"in_progress\"},{\"content\":\"b\",\"status\":\"in_progress\"}]}");
    CHECK(tools_execute("todo_write", tl, &out) == TOOL_ERROR && strstr(out.data, "only one task")); sb_clear(&out); cJSON_Delete(tl);
    tl = cJSON_Parse("{\"todos\":[{\"content\":\"a\",\"status\":\"doing\"}]}");
    CHECK(tools_execute("todo_write", tl, &out) == TOOL_ERROR && strstr(out.data, "status must be")); sb_clear(&out); cJSON_Delete(tl);
    tl = cJSON_Parse("{\"todos\":\"x\"}");
    CHECK(tools_execute("todo_write", tl, &out) == TOOL_ERROR && strstr(out.data, "must be an array")); sb_clear(&out); cJSON_Delete(tl);
    tl = cJSON_Parse("{\"todos\":[{\"status\":\"pending\"}]}");
    CHECK(tools_execute("todo_write", tl, &out) == TOOL_ERROR && strstr(out.data, "no content")); sb_clear(&out); cJSON_Delete(tl);
    tl = cJSON_Parse("{\"todos\":[]}");
    CHECK(tools_execute("todo_write", tl, &out) == TOOL_OK && strstr(out.data, "cleared") && tools_todos_get() && !cJSON_GetArraySize(tools_todos_get())); sb_clear(&out); cJSON_Delete(tl);
    tools_todos_set(NULL);

    /* permission rules: glob patterns, and deny beats ask beats allow (pure: nothing is written to the config) */
    CHECK(glob_match("rm *", "rm -rf b") && !glob_match("rm *", "arm x") && glob_match("a?c", "abc") && !glob_match("a?c", "ac"));
    CHECK(glob_match("*.env", "x/y/.env") && glob_match("*", "") && glob_match("", "") && !glob_match("", "a") && glob_match("a**b", "a/x/b"));
    CHECK(!glob_match("*x*y", "axbyz") && glob_match("*x*y", "axxy") && !glob_match("rm *", "rmdir x"));
    CHECK(perm_rule_verdict("deny bash rm *", "bash", "rm -rf b") == VERDICT_DENY);
    CHECK(perm_rule_verdict("deny bash rm *", "bash", "cd a && rm -rf b") == VERDICT_DENY);   /* a part of a chain */
    CHECK(perm_rule_verdict("deny bash rm *", "bash", "X=1 rm f") == VERDICT_DENY);           /* a leading assignment */
    CHECK(perm_rule_verdict("deny bash rm *", "bash", "/bin/rm f") == VERDICT_DENY);          /* a path in front of the program */
    CHECK(perm_rule_verdict("deny bash rm *", "bash", "ls $(rm x)") == VERDICT_ASK);          /* a subshell the rule cannot see into */
    CHECK(perm_rule_verdict("deny bash rm *", "bash", "ls -la") == VERDICT_NONE);
    CHECK(perm_rule_verdict("ask bash git push*", "bash", "git push origin") == VERDICT_ASK);
    CHECK(perm_rule_verdict("bash git status", "bash", "git status -s") == VERDICT_ALLOW);   /* the old allow rules are as they were */
    CHECK(perm_rule_verdict("bash git status", "bash", "git status; rm x") == VERDICT_NONE);
    CHECK(perm_rule_verdict("deny bash rm", "edit", "x") == VERDICT_NONE);                    /* a rule is for its own tool */
    CHECK(perm_rule_verdict("deny edit .env", "edit", ".env") == VERDICT_DENY);
    CHECK(perm_rule_verdict("deny edit *.pem", "edit", "keys/a.pem") == VERDICT_DENY);
    CHECK(perm_rule_verdict("deny edit .env", "edit", "a.env") == VERDICT_NONE);
    CHECK(perm_rule_verdict("edit", "edit", "a.c") == VERDICT_ALLOW);
    CHECK(perm_rule_verdict("deny fetch *.evil.com", "fetch", "https://x.evil.com/p") == VERDICT_DENY);   /* a host pattern */
    CHECK(perm_rule_verdict("ask fetch https://docs.x.org/*", "fetch", "https://docs.x.org/a") == VERDICT_ASK);   /* a URL pattern */
    CHECK(perm_rule_verdict("fetch docs.x.org", "fetch", "https://docs.x.org/a") == VERDICT_ALLOW);
    CHECK(perm_rule_verdict(NULL, "bash", "ls") == VERDICT_NONE && perm_rule_verdict("deny bash", "bash", "ls") == VERDICT_NONE);

    /* hooks: which line is an event's hook, for which tool, and its command */
    const char *hc = NULL;
    CHECK(hook_match("pre bash echo hi", "pre", "bash", &hc) && !strcmp(hc, "echo hi"));
    CHECK(hook_match("pre bash* make", "pre", "bashful", NULL) && hook_match("pre * exit 2", "pre", "fetch", NULL));
    CHECK(!hook_match("pre edit x", "pre", "bash", NULL) && !hook_match("post bash x", "pre", "bash", NULL) && !hook_match("prebash x", "pre", "bash", NULL));
    CHECK(!hook_match("pre bash", "pre", "bash", NULL) && !hook_match("pre bash ", "pre", "bash", NULL));   /* no command */
    CHECK(hook_match("start make", "start", NULL, &hc) && !strcmp(hc, "make") && !hook_match("stop x", "start", NULL, NULL));
    CHECK(hook_match("post edit f", "post", "edit", NULL) && !hook_match(NULL, "pre", "bash", NULL));

    /* background bash: output is read in pieces, a job is stopped and its group is gone (real processes; all stopped at the end) */
    {
        bool saved_nc = tools_no_confirm; tools_no_confirm = true;
        cJSON *bj = cJSON_CreateObject();
        cJSON_AddStringToObject(bj, "command", "echo a; sleep 0.3; echo b");
        cJSON_AddBoolToObject(bj, "run_in_background", 1);
        sbuf o; sb_init(&o);
        CHECK(tools_execute("bash", bj, &o) == TOOL_OK && strstr(o.data, "started background job bg"));
        char id[16] = ""; int pid = 0;
        const char *ip = strstr(o.data, "job bg"); if (ip) sscanf(ip + 4, "%15[^ ]", id);
        const char *pp = strstr(o.data, "(pid "); if (pp) pid = atoi(pp + 5);
        cJSON_Delete(bj); sb_free(&o);
        cJSON *jj = cJSON_CreateObject();
        cJSON_AddStringToObject(jj, "job", id); cJSON_AddStringToObject(jj, "action", "output");
        sbuf r; sb_init(&r);
        tools_execute("bash_job", jj, &r);
        CHECK(strstr(r.data, "[job ") && strstr(r.data, "running") && pid > 0);   /* still going: nothing has been read yet */
        sb_clear(&r); usleep(700000);
        tools_execute("bash_job", jj, &r);
        CHECK(strstr(r.data, "b\n") && strstr(r.data, "exit code 0"));
        sb_clear(&r);
        tools_execute("bash_job", jj, &r);   /* the next read: only what is new */
        CHECK(strstr(r.data, "no new output") && !strstr(r.data, "b\n"));
        sb_clear(&r); cJSON_Delete(jj);
        /* a long job, stopped: its group is gone afterwards */
        cJSON *sj = cJSON_CreateObject();
        cJSON_AddStringToObject(sj, "command", "sleep 30");
        cJSON_AddBoolToObject(sj, "run_in_background", 1);
        tools_execute("bash", sj, &r);
        int pid2 = 0; char id2[16] = ""; const char *q = strstr(r.data, "job bg"); if (q) sscanf(q + 4, "%15[^ ]", id2);
        const char *pq = strstr(r.data, "(pid "); if (pq) pid2 = atoi(pq + 5);
        sb_clear(&r); cJSON_Delete(sj);
        cJSON *kj = cJSON_CreateObject();
        cJSON_AddStringToObject(kj, "job", id2); cJSON_AddStringToObject(kj, "action", "kill");
        CHECK(tools_execute("bash_job", kj, &r) == TOOL_OK && strstr(r.data, "stopped"));
        CHECK(pid2 > 0 && kill(-pid2, 0) == -1 && errno == ESRCH);   /* the group is gone */
        sb_clear(&r);
        CHECK(tools_execute("bash_job", kj, &r) == TOOL_ERROR && strstr(r.data, "no background job"));   /* and the job is forgotten */
        cJSON_Delete(kj); sb_free(&r);
        tools_no_confirm = saved_nc;
        tools_bg_kill_all();
    }

    /* MCP: the names the model sees, the text of a result, and the mcp permission rule (pure: no server is started) */
    {
        char *n1 = mcp_tool_name("fake", "echo"), *n2 = mcp_tool_name("my srv", "a.b c"), *n3 = mcp_tool_name("s", "tool_with_a_very_long_name_that_goes_on_and_on_and_on_past_sixty_four");
        CHECK(!strcmp(n1, "mcp__fake__echo") && !strcmp(n2, "mcp__my_srv__a_b_c"));
        CHECK(strlen(n3) == 64 && mcp_is_tool(n3) && strchr(n3, '_') && strcmp(n3, mcp_tool_name("s", "tool_with_a_very_long_name_that_goes_on_and_on_and_on_past_sixty_five")));
        free(n1); free(n2); free(n3);
        cJSON *res = cJSON_Parse("{\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"image\",\"data\":\"x\"},{\"type\":\"text\",\"text\":\"b\"}]}");
        char *txt = mcp_content_text(res);
        CHECK(txt && !strcmp(txt, "a\n[image]\nb"));
        free(txt); cJSON_Delete(res);
        CHECK(perm_rule_verdict("mcp mcp__fake__echo", "mcp", "mcp__fake__echo") == VERDICT_ALLOW);
        CHECK(perm_rule_verdict("mcp mcp__fake__echo", "mcp", "mcp__fake__shout") == VERDICT_NONE);
        CHECK(perm_rule_verdict("deny mcp mcp__fake__*", "mcp", "mcp__fake__shout") == VERDICT_DENY);
        CHECK(perm_rule_verdict("ask mcp mcp__*", "mcp", "mcp__x__y") == VERDICT_ASK && perm_rule_verdict("deny bash rm *", "mcp", "rm") == VERDICT_NONE);
    }

    /* sub-agent files: a tool list limits the tools; none means all (the frontmatter is read by agents_load) */
    {
        agent_t ag; memset(&ag, 0, sizeof ag);
        ag.tools = xstrdup("read_file, grep");
        CHECK(agent_allows(&ag, "read_file") && agent_allows(&ag, "grep") && !agent_allows(&ag, "bash") && !agent_allows(&ag, "read_fil"));
        free(ag.tools); ag.tools = NULL;
        CHECK(agent_allows(&ag, "bash") && agent_allows(&ag, "web_fetch"));
        /* Claude Code's names for the same tools, and an agent whose list names none of ours */
        ag.tools = xstrdup("Read, Grep");
        CHECK(agent_allows(&ag, "read_file") && agent_allows(&ag, "grep") && !agent_allows(&ag, "bash") && agent_has_usable_tools(&ag));
        free(ag.tools); ag.tools = xstrdup("Write, Edit");
        CHECK(!agent_has_usable_tools(&ag));
        free(ag.tools); ag.tools = NULL;
        /* the model a file names: Claude Code's aliases mean the sub-agents' own model */
        ag.model = xstrdup("sonnet"); CHECK(agent_model_override(&ag) == NULL);
        free(ag.model); ag.model = xstrdup("haiku"); CHECK(agent_model_override(&ag) == NULL);
        free(ag.model); ag.model = xstrdup("inherit"); CHECK(agent_model_override(&ag) == NULL);
        free(ag.model); ag.model = xstrdup("qwen3:8b"); CHECK(agent_model_override(&ag) && !strcmp(agent_model_override(&ag), "qwen3:8b"));
        free(ag.model); ag.model = NULL; CHECK(agent_model_override(&ag) == NULL);
    }

    /* sandbox: the bwrap command quotes the directory and the command as one word each */
    {
        char *sc = sandbox_command("/p q", "echo 'a'");
        CHECK(sc && strstr(sc, "--bind '/p q' '/p q'") && strstr(sc, "--chdir '/p q'") && strstr(sc, "--ro-bind / /"));
        CHECK(sc && strstr(sc, "-- /bin/sh -c 'echo '") && strstr(sc, "'\\''a'"));   /* the quote inside is escaped */
        free(sc);
    }

    /* stream-json: one line per message, tool calls and results matched, system messages left out; input lines parsed */
    {
        cJSON *msgs = cJSON_Parse("["
            "{\"role\":\"user\",\"content\":\"hello\"},"
            "{\"role\":\"system\",\"content\":\"not in the stream\"},"
            "{\"role\":\"assistant\",\"content\":\"Running it.\",\"tool_calls\":[{\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}}]},"
            "{\"role\":\"tool\",\"tool_name\":\"bash\",\"content\":\"a.c\"},"
            "{\"role\":\"tool\",\"tool_call_id\":\"call_x\",\"content\":\"named\"}]");
        stream_st st; memset(&st, 0, sizeof st);
        sbuf o; sb_init(&o);
        stream_messages(&st, msgs, 0, "sid", &o);
        int lines = 0; for (char *p = o.data; p && *p; p++) if (*p == '\n') lines++;
        CHECK(lines == 4);   /* the system message is not one of them */
        CHECK(strstr(o.data, "{\"session_id\":\"sid\",\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"hello\"}}"));
        CHECK(strstr(o.data, "\"type\":\"tool_use\",\"id\":\"toolu_2_0\",\"name\":\"bash\",\"input\":{\"command\":\"ls\"}"));
        CHECK(strstr(o.data, "\"tool_use_id\":\"toolu_2_0\",\"content\":\"a.c\""));   /* the oldest call not answered */
        CHECK(strstr(o.data, "\"tool_use_id\":\"call_x\",\"content\":\"named\""));
        CHECK(st.npend == 0);
        /* a second flush from message 4 on writes nothing new, and the state carries over */
        sb_clear(&o); stream_messages(&st, msgs, 5, "sid", &o); CHECK(o.len == 0);
        sb_free(&o); stream_reset(&st); cJSON_Delete(msgs);
        char *t1 = stream_input_text("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"plain\"}}");
        char *t2 = stream_input_text("{\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"one\"},{\"type\":\"image\"},{\"type\":\"text\",\"text\":\"two\"}]}}");
        CHECK(t1 && !strcmp(t1, "plain") && t2 && !strcmp(t2, "one\ntwo"));
        CHECK(stream_input_text("not json") == NULL);
        free(t1); free(t2);
    }

    /* tool definitions are well-formed */
    cJSON *defs = tools_definitions();
    CHECK(cJSON_GetArraySize(defs) == 10);   /* the seven, bash_job, todo_write, and exit */
    cJSON *t; cJSON_ArrayForEach(t, defs) {
        cJSON *f = cJSON_GetObjectItem(t, "function");
        CHECK(cJSON_IsString(cJSON_GetObjectItem(f, "name")));
        CHECK(cJSON_IsObject(cJSON_GetObjectItem(cJSON_GetObjectItem(f, "parameters"), "properties")));
    }
    cJSON_Delete(defs);

    sb_free(&out);
    CHECK(chdir(old) == 0);
    char cmd[4200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
}

/* ---------- http client against a fake server ---------- */
/* forks a server that accepts one connection, reads request, sends response. Returns pid. */
static pid_t start_server(const char *response, int *port, char **req_out_path) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof a) != 0) return -1;
    socklen_t al = sizeof a; getsockname(s, (struct sockaddr*)&a, &al); *port = ntohs(a.sin_port);
    listen(s, 1);
    static char reqpath[] = "/tmp/crowtest_req_XXXXXX";
    static int made = 0;
    if (!made) { int fd = mkstemp(reqpath); close(fd); made = 1; }
    *req_out_path = reqpath;
    pid_t pid = fork();
    if (pid == 0) {
        int c = accept(s, NULL, NULL);
        char buf[65536]; ssize_t n, total = 0;
        struct timeval tv = { 0, 300000 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        while ((n = read(c, buf + total, sizeof buf - 1 - (size_t)total)) > 0) {
            total += n; buf[total] = 0;
            char *he = strstr(buf, "\r\n\r\n");
            if (he && !strcasestr(buf, "content-length:")) break;
            char *cl = strcasestr(buf, "content-length:");
            if (cl && he && total - (he + 4 - buf) >= atol(cl + 15)) break;
        }
        write_whole_file(reqpath, buf, (size_t)total);
        const char *r = response; size_t rl = strlen(r);
        /* send in small pieces to exercise the parser */
        while (rl) { size_t k = rl < 7 ? rl : 7; if (write(c, r, k) < 0) break; r += k; rl -= k; usleep(1000); }
        close(c); close(s); _exit(0);
    }
    close(s);
    return pid;
}

static int collect_line(const char *line, size_t len, void *ud) { sbuf *b = ud; sb_append(b, line, len); sb_putc(b, '|'); return 0; }
static int abort_line(const char *line, size_t len, void *ud) { (void)line; (void)len; int *n = ud; return ++*n >= 2; }

static void test_http(void) {
    int port; char *reqpath; char url[64];
    /* chunked NDJSON, streamed line delivery */
    pid_t p = start_server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nTransfer-Encoding: chunked\r\n\r\n"
        "6\r\n{\"a\":1\r\n2\r\n}\n\r\nA\r\n{\"b\":2}\n{\"\r\n6\r\nc\":3}\n\r\n0\r\n\r\n", &port, &reqpath);
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    sbuf lines; sb_init(&lines); http_result res;
    int rc = http_request(url, "POST", "/api/chat", "{\"x\":1}", NULL, collect_line, &lines, &res);
    CHECK(rc == 0); CHECK(res.status == 200); CHECK(!res.aborted);
    CHECK_STR(lines.data, "{\"a\":1}|{\"b\":2}|{\"c\":3}|");
    waitpid(p, NULL, 0);
    size_t n; char *req = read_whole_file(reqpath, &n, 0);
    CHECK(strstr(req, "POST /api/chat HTTP/1.1\r\n") != NULL);
    CHECK(strstr(req, "Content-Length: 7\r\n") != NULL);
    CHECK(strstr(req, "\r\n\r\n{\"x\":1}") != NULL);
    free(req); sb_free(&lines);

    /* content-length body into sbuf */
    p = start_server("HTTP/1.1 404 Not Found\r\nContent-Length: 16\r\n\r\n{\"error\":\"nope\"}", &port, &reqpath);
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    sbuf body; sb_init(&body);
    rc = http_request(url, "GET", "/api/tags", NULL, &body, NULL, NULL, &res);
    CHECK(rc == 0); CHECK(res.status == 404); CHECK_STR(body.data, "{\"error\":\"nope\"}");
    waitpid(p, NULL, 0); sb_free(&body);

    /* callback abort */
    p = start_server("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n1\n\r\n2\r\n2\n\r\n2\r\n3\n\r\n0\r\n\r\n", &port, &reqpath);
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    int cnt = 0;
    rc = http_request(url, "GET", "/", NULL, NULL, abort_line, &cnt, &res);
    CHECK(rc == 0); CHECK(res.aborted); CHECK(cnt == 2);
    waitpid(p, NULL, 0);

    /* connection refused */
    rc = http_request("http://127.0.0.1:1", "GET", "/", NULL, NULL, NULL, NULL, &res);
    CHECK(rc < 0); CHECK(strstr(res.err, "connect") != NULL);
    /* a host without a scheme, the way OLLAMA_HOST is written: plain http */
    p = start_server("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}", &port, &reqpath);
    snprintf(url, sizeof url, "127.0.0.1:%d", port);
    sbuf got; sb_init(&got);
    rc = http_request(url, "GET", "/api/version", NULL, &got, NULL, NULL, &res);
    CHECK(rc == 0 && res.status == 200); CHECK_STR(got.data, "{}");
    waitpid(p, NULL, 0); sb_free(&got);
    /* https goes through the same client: refused like plain http, not "unsupported" */
    rc = http_request("https://127.0.0.1:1", "GET", "/", NULL, NULL, NULL, NULL, &res);
    CHECK(rc < 0); CHECK(strstr(res.err, "connect") != NULL);

    /* extra headers go out with the request (the hosted APIs' keys) */
    p = start_server("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}", &port, &reqpath);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/v1", port);
    const char *extra[] = { "x-api-key: sk-test", "anthropic-version: 2023-06-01", NULL };
    http_headers = extra;
    rc = http_request(url, "POST", "/messages", "{}", NULL, NULL, NULL, &res);
    http_headers = NULL;
    CHECK(rc == 0 && res.status == 200);
    waitpid(p, NULL, 0);
    req = read_whole_file(reqpath, &n, 0);
    CHECK(strstr(req, "POST /v1/messages HTTP/1.1\r\n") != NULL);
    CHECK(strcasestr(req, "\r\nx-api-key: sk-test\r\n") != NULL && strcasestr(req, "\r\nanthropic-version: 2023-06-01\r\n") != NULL);
    free(req);
    unlink(reqpath);

    /* where a host setting points, and whether the way there is encrypted */
    char hn[64];
    CHECK(url_hostname("gpu-box:11434", hn, sizeof hn) && !strcmp(hn, "gpu-box"));
    CHECK(url_hostname("https://user@API.example.org:8443/v1", hn, sizeof hn) && !strcmp(hn, "api.example.org"));
    CHECK(url_hostname("http://[fd00::1]:8080/v1", hn, sizeof hn) && !strcmp(hn, "[fd00::1]"));
    CHECK(!url_hostname(":11434", hn, sizeof hn));
    CHECK(url_is_local("localhost") && url_is_local("http://127.0.0.1:11434") && url_is_local("[::1]:9") && url_is_local("0.0.0.0") && url_is_local(":11434") && url_is_local("http://LOCALHOST"));
    CHECK(!url_is_local("gpu-box:11434") && !url_is_local("http://10.0.0.5") && !url_is_local("http://127.evil.example/") && !url_is_local("https://api.x.ai/v1"));
    CHECK(url_cleartext("gpu-box:11434") && url_cleartext("http://x") && url_cleartext("HTTP://x") && !url_cleartext("https://x") && !url_cleartext("HTTPS://x"));
    /* what the host setting becomes: Ollama's port when http has none, the scheme's own for https */
    char *u;
    u = http_url("http://127.0.0.1:11434", "/api/chat"); CHECK_STR(u, "http://127.0.0.1:11434/api/chat"); free(u);
    u = http_url("localhost", "/api/tags"); CHECK_STR(u, "localhost:11434/api/tags"); free(u);   /* no scheme written in: libcurl's default is http */
    u = http_url("http://gpu-box", "/api/tags"); CHECK_STR(u, "http://gpu-box:11434/api/tags"); free(u);
    u = http_url("0.0.0.0:8080", "/x"); CHECK_STR(u, "0.0.0.0:8080/x"); free(u);
    u = http_url(":11500", "/x"); CHECK_STR(u, "127.0.0.1:11500/x"); free(u);
    u = http_url("gpu-box", "/api/chat"); CHECK_STR(u, "gpu-box:11434/api/chat"); free(u);
    u = http_url("http://[::1]", "/x"); CHECK_STR(u, "http://[::1]:11434/x"); free(u);
    u = http_url("http://[::1]:9000/", "/x"); CHECK_STR(u, "http://[::1]:9000/x"); free(u);
    u = http_url("https://ollama.example.org", "/api/chat"); CHECK_STR(u, "https://ollama.example.org/api/chat"); free(u);
    u = http_url("https://api.x.ai/v1/", "/chat/completions"); CHECK_STR(u, "https://api.x.ai/v1/chat/completions"); free(u);
    u = http_url("https://api.openai.com/v1", "models/gpt-5"); CHECK_STR(u, "https://api.openai.com/v1/models/gpt-5"); free(u);
}

/* ---------- misc util ---------- */
static void test_util(void) {
    setenv("HOME", "/home/tester", 1);
    char *e = expand_home("~/x/y"); CHECK_STR(e, "/home/tester/x/y"); free(e);
    e = expand_home("/abs"); CHECK_STR(e, "/abs"); free(e);
    e = expand_home("~"); CHECK_STR(e, "/home/tester"); free(e);
    char dir[] = "/tmp/crowtest_u_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
    char deep[300]; snprintf(deep, sizeof deep, "%s/a/b/c", dir);
    CHECK(mkdir_p(deep) == 0); CHECK(is_dir(deep));
    char f[400]; snprintf(f, sizeof f, "%s/f", deep);
    CHECK(write_whole_file(f, "hi", 2) == 0); size_t n; char *d = read_whole_file(f, &n, 0); CHECK(n == 2); CHECK_STR(d, "hi"); free(d);
    d = read_whole_file(f, &n, 1); CHECK(n == 1); free(d);   /* cap honoured */
    /* atomic replace: the file is swapped in whole, keeps its mode, and leaves no litter */
    CHECK(chmod(f, 0640) == 0);
    CHECK(write_whole_file_atomic(f, "replaced", 8) == 0);
    d = read_whole_file(f, &n, 0); CHECK(n == 8); CHECK_STR(d, "replaced"); free(d);
    struct stat sb; CHECK(stat(f, &sb) == 0 && (sb.st_mode & 07777) == 0640);
    DIR *dp = opendir(deep); int stray = 0; struct dirent *de;
    while (dp && (de = readdir(dp))) if (strstr(de->d_name, ".tmp")) stray++;
    if (dp) closedir(dp);
    CHECK(stray == 0);
    char nf[400]; snprintf(nf, sizeof nf, "%s/new", deep);
    CHECK(write_whole_file_atomic(nf, "", 0) == 0 && is_file(nf));   /* creates it too */
    CHECK(write_whole_file_atomic("/nope/nowhere/x", "x", 1) != 0);  /* and reports failure */
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
}

/* ---------- a shell command's fold head ---------- */
static void test_cmd_head(void) {
    char out[64]; int more;
    more = cmd_head("ls -la", 40, out, sizeof out); CHECK(more == 0); CHECK_STR(out, "ls -la");
    more = cmd_head("ls\n", 40, out, sizeof out); CHECK(more == 0); CHECK_STR(out, "ls");          /* a trailing newline is no line */
    more = cmd_head("a\nb", 40, out, sizeof out); CHECK(more == 1); CHECK_STR(out, "a");
    more = cmd_head("a\nb\n", 40, out, sizeof out); CHECK(more == 1);
    more = cmd_head("a\n\nb", 40, out, sizeof out); CHECK(more == 2);                        /* a blank line in the middle counts */
    more = cmd_head("a\n\n\n", 40, out, sizeof out); CHECK(more == 0);                       /* trailing blank lines do not */
    more = cmd_head("\n\n  set -e\n  make", 40, out, sizeof out); CHECK(more == 1); CHECK_STR(out, "set -e");   /* leading blanks go */
    more = cmd_head("cat <<EOF\r\nx\r\nEOF", 40, out, sizeof out); CHECK(more == 2); CHECK_STR(out, "cat <<EOF");   /* no \r left */
    more = cmd_head("echo\tx\x01y\nz", 40, out, sizeof out); CHECK_STR(out, "echo x y");   /* tab and controls become spaces */
    more = cmd_head("abcdefghijklmnop\nz", 10, out, sizeof out); CHECK(more == 1); CHECK_STR(out, "abcdefghi\xe2\x80\xa6");   /* cut, with an ellipsis */
    more = cmd_head("äöüäöüäöü\nz", 4, out, sizeof out); CHECK(more == 1); CHECK_STR(out, "äöü\xe2\x80\xa6");   /* cut on a character, counted in columns */
    more = cmd_head("abc\nz", 3, out, sizeof out); CHECK_STR(out, "abc");                     /* exactly fits: no ellipsis */
    char small[4]; more = cmd_head("abcdef\nz", 10, small, sizeof small); CHECK(more == 1); CHECK(strlen(small) < sizeof small);   /* a small buffer is never overrun */
}

/* ---------- URLs and HTML (web_fetch) ---------- */
static void test_web(void) {
    CHECK(url_ok("https://www.postgresql.org/docs/17/runtime-config-client.html"));
    CHECK(url_ok("http://localhost:8000/x"));
    CHECK(!url_ok("ftp://example.org/x"));
    CHECK(!url_ok("file:///etc/passwd"));
    CHECK(!url_ok("https://example.org/a b"));            /* whitespace */
    CHECK(!url_ok("https://example.org/a\nrm -rf /"));    /* control characters */
    CHECK(!url_ok("https://"));                           /* no host */
    CHECK(!url_ok("http://169.254.169.254/latest/meta-data/"));   /* cloud metadata */
    CHECK(!url_ok("http://metadata.google.internal/computeMetadata/v1/"));
    CHECK(!url_ok(NULL));

    char h[64];
    CHECK(url_host("https://Docs.Example.ORG/a/b?q=1", h, sizeof h)); CHECK_STR(h, "docs.example.org");
    CHECK(url_host("http://127.0.0.1:8000", h, sizeof h)); CHECK_STR(h, "127.0.0.1:8000");
    CHECK(url_host("https://user@example.org/x", h, sizeof h)); CHECK_STR(h, "example.org");
    CHECK(!url_host("not-a-url", h, sizeof h));

    /* script/style/comments dropped, entities decoded, blocks broken, <pre> fenced */
    const char *page =
        "<!doctype html><html><head><title>Keycloak &amp; you</title>"
        "<style>body{color:red}</style></head><body>"
        "<!-- a comment --><h1>Admin REST API</h1>"
        "<p>Use <code>GET /admin/realms/{realm}/users</code> to list users.</p>"
        "<ul><li>first</li><li>second</li></ul>"
        "<pre>curl -H 'Authorization: Bearer $TOKEN' \\\n  https://kc/admin</pre>"
        "<script>var x = '<p>not text</p>';</script>"
        "<p>See <a href=\"https://www.keycloak.org/docs\">the docs</a> or <a href=\"/downloads\">downloads</a>.</p>"
        "</body></html>";
    char *t = html_to_text(page, strlen(page), "https://kc.example.org");
    CHECK(strstr(t, "Keycloak & you") != NULL);              /* title kept, entity decoded */
    CHECK(strstr(t, "Admin REST API") != NULL);
    CHECK(strstr(t, "GET /admin/realms/{realm}/users") != NULL);
    CHECK(strstr(t, "- first") != NULL && strstr(t, "- second") != NULL);
    CHECK(strstr(t, "```") != NULL);                          /* <pre> fenced */
    CHECK(strstr(t, "Bearer $TOKEN") != NULL);
    CHECK(strstr(t, "\n  https://kc/admin") != NULL);         /* <pre> whitespace kept */
    CHECK(strstr(t, "the docs (https://www.keycloak.org/docs)") != NULL);
    CHECK(strstr(t, "(https://kc.example.org/downloads)") != NULL);   /* rooted href resolved against the page */
    CHECK(strstr(t, "color:red") == NULL);                    /* <style> gone */
    CHECK(strstr(t, "not text") == NULL);                     /* <script> gone */
    CHECK(strstr(t, "a comment") == NULL);
    CHECK(strstr(t, "<p>") == NULL && strstr(t, "</html>") == NULL);
    CHECK(strstr(t, "\n\n\n") == NULL);                       /* blank lines collapsed */
    free(t);

    /* --- search results --- */
    char *e2 = url_encode("keycloak admin rest api & \"more\"");
    CHECK_STR(e2, "keycloak+admin+rest+api+%26+%22more%22"); free(e2);
    e2 = url_decode("https%3A%2F%2Fx.org%2Fa+b", strlen("https%3A%2F%2Fx.org%2Fa+b"));
    CHECK_STR(e2, "https://x.org/a b"); free(e2);

    /* the shape every engine has: title link, the same href again as display URL and snippet,
     * behind a redirect wrapper, mixed with the engine's own navigation links */
    const char *serp =
        "<html><body>"
        "<a href=\"/html/?q=x&kl=us-en\">US (English)</a>"
        "<a class=\"result__a\" href=\"//duck.example/l/?uddg=https%3A%2F%2Fwww.keycloak.org%2Fdocs&rut=ab\">Keycloak Docs</a>"
        "<a href=\"//duck.example/l/?uddg=https%3A%2F%2Fwww.keycloak.org%2Fdocs&rut=ab\">www.keycloak.org/docs</a>"
        "<a href=\"//duck.example/l/?uddg=https%3A%2F%2Fwww.keycloak.org%2Fdocs&rut=ab\">Documentation for Keycloak, the open source identity and access management solution.</a>"
        "<a href=\"https://example.org/second\">Second hit</a>"
        "<a href=\"https://duck.example/settings\">Settings</a>"
        "</body></html>";
    int found = -1;
    char *res = search_results_text(serp, strlen(serp), "https://duck.example/html/?q=x", 10, &found);
    CHECK(found == 2);
    CHECK(strstr(res, "1. Keycloak Docs") != NULL);
    CHECK(strstr(res, "https://www.keycloak.org/docs") != NULL);            /* redirect unwrapped */
    CHECK(strstr(res, "duckduckgo") == NULL && strstr(res, "uddg") == NULL);
    CHECK(strstr(res, "Documentation for Keycloak, the open source") != NULL);   /* prose became the snippet */
    CHECK(strstr(res, "1. www.keycloak.org") == NULL);                      /* the display-URL repeat is not a result of its own */
    CHECK(strstr(res, "2. Second hit") != NULL);
    CHECK(strstr(res, "Settings") == NULL);                                 /* the engine's own pages dropped */
    free(res);
    res = search_results_text(serp, strlen(serp), "https://duck.example/html/?q=x", 1, &found);
    CHECK(found == 1 && strstr(res, "Second hit") == NULL);                 /* max_results honoured */
    free(res);
    res = search_results_text("<html><body>nothing here</body></html>", 38, "https://duck.example/", 10, &found);
    CHECK(found == 0 && res && !*res);                                      /* a page with no results says so */
    free(res);

    /* numeric entities, a bare '<', and text that is not really HTML */
    const char *odd = "<p>a &lt; b &#65;&#x42; &nosuch; 3 < 4</p>";
    t = html_to_text(odd, strlen(odd), NULL);
    CHECK(strstr(t, "a < b AB &nosuch; 3 < 4") != NULL);
    free(t);
}

/* ---------- permission modes ---------- */
static void test_modes_body(void *ud) {
    (void)ud;
    char dir[] = "/tmp/crowtest_m_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
    char old[4096]; CHECK(getcwd(old, sizeof old) != NULL); CHECK(chdir(dir) == 0);
    tools_reset_permissions();
    g_cfg.interactive = false;
    sbuf out; sb_init(&out); cJSON *a;

    /* plan mode: edits denied with an explanation, reads still work */
    g_cfg.mode = MODE_PLAN;
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "p.txt"); cJSON_AddStringToObject(a, "content", "x");
    CHECK(tools_execute("write_file", a, &out) == TOOL_DENIED); cJSON_Delete(a);
    CHECK(strstr(out.data, "plan mode") != NULL); CHECK(!is_file("p.txt")); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", ".");
    CHECK(tools_execute("list_dir", a, &out) == TOOL_OK); cJSON_Delete(a); sb_clear(&out);

    /* accept-edits: file writes go through, shell still needs confirmation */
    g_cfg.mode = MODE_ACCEPT_EDITS;
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "p.txt"); cJSON_AddStringToObject(a, "content", "hello\n");
    CHECK(tools_execute("write_file", a, &out) == TOOL_OK); cJSON_Delete(a); CHECK(is_file("p.txt")); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", "p.txt"); cJSON_AddStringToObject(a, "old_string", "hello"); cJSON_AddStringToObject(a, "new_string", "bye");
    CHECK(tools_execute("edit_file", a, &out) == TOOL_OK); cJSON_Delete(a); sb_clear(&out);
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", "echo hi");
    CHECK(tools_execute("bash", a, &out) == TOOL_DENIED); cJSON_Delete(a); sb_clear(&out);

    /* auto: everything runs */
    g_cfg.mode = MODE_AUTO;
    a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", "echo hi");
    CHECK(tools_execute("bash", a, &out) == TOOL_OK); cJSON_Delete(a); CHECK(strstr(out.data, "hi") != NULL); sb_clear(&out);

    /* names round-trip */
    CHECK(mode_parse("plan") == MODE_PLAN); CHECK(mode_parse("accept-edits") == MODE_ACCEPT_EDITS);
    CHECK(mode_parse("yolo") == MODE_AUTO); CHECK(mode_parse("nope") == -1);
    for (int m = 0; m < MODE_COUNT; m++) CHECK(mode_parse(mode_name(m)) == m);

    sb_free(&out); g_cfg.mode = MODE_MANUAL;
    CHECK(chdir(old) == 0);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
}
static void test_modes(void) { char *o = capture(test_modes_body, NULL); free(o); }

/* ---------- message queue: keystrokes typed while busy -> text ---------- */
static void test_queue(void) {
    #define KT(str) term_keys_to_text((const unsigned char *)(str), sizeof(str) - 1, NULL)
    #define KTC(str, cc) term_keys_to_text((const unsigned char *)(str), sizeof(str) - 1, &(cc))
    char *t;
    t = KT("hello world"); CHECK_STR(t, "hello world"); free(t);
    t = KT("  padded  \n"); CHECK_STR(t, "padded"); free(t);
    t = KT("helo\x7flo"); CHECK_STR(t, "hello"); free(t);                       /* backspace */
    t = KT("caf\xc3\xa9\x7f!"); CHECK_STR(t, "caf!"); free(t);               /* backspace removes a whole UTF-8 char */
    t = KT("wrong\x15right"); CHECK_STR(t, "right"); free(t);                  /* Ctrl-U */
    t = KT("keep this bad\x17good"); CHECK_STR(t, "keep this good"); free(t);  /* Ctrl-W */
    t = KT("a\x1b[Db\x1b[1;5Cc\x1bOAd"); CHECK_STR(t, "bacd"); free(t);      /* Left and Right move the caret, Up is dropped */
    /* the caret: edits go where it is, and the caret's offset is reported with the text */
    { size_t cc = 0; t = KTC("hello wrld\x1b[D\x1b[D\x1b[D" "o", cc);   /* insert before the caret */
      CHECK_STR(t, "hello world"); CHECK(cc == 8); free(t); }
    t = KT("abc\x1b[Hx"); CHECK_STR(t, "xabc"); free(t);                          /* Home: the line's start */
    t = KT("abc\x1bOHx\x1bOFy"); CHECK_STR(t, "xabcy"); free(t);                  /* SS3 Home and End */
    t = KT("abcd\x1b[D\x7f"); CHECK_STR(t, "abd"); free(t);                       /* backspace before the caret */
    t = KT("abcd\x1b[D\x1b[D\x1b[3~"); CHECK_STR(t, "abd"); free(t);             /* Delete after the caret */
    t = KT("one two three\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D\x17"); CHECK_STR(t, "one  three"); free(t);   /* Ctrl-W before the caret */
    t = KT("ab\x1b[D\r"); CHECK_STR(t, "a\nb"); free(t);                          /* a line break at the caret */
    t = KT("\x1b[D" "x\x1b[C"); CHECK_STR(t, "x"); free(t);                       /* moving left of the start stays there */
    { size_t cc = 0; t = KTC("  xy\x1b[D", cc);                              /* trimmed: the caret moves with the trim */
      CHECK_STR(t, "xy"); CHECK(cc == 1); free(t); }
    t = KT("line1\x1b\rline2\nline3"); CHECK_STR(t, "line1\nline2\nline3"); free(t);   /* Alt+Enter, Ctrl-J */
    t = KT("\x1b[200~pasted\r\nlines\x1b[201~"); CHECK_STR(t, "pasted\nlines"); free(t);   /* paste brackets */
    /* a paste while the model works is text, as the ANSI reader makes it: DEL, escape sequences and a second start do not act as keys */
    t = KT("\x1b[200~a\x7f" "b\x1b[201~"); CHECK_STR(t, "ab"); free(t);              /* DEL is not a backspace */
    t = KT("\x1b[200~ab\x1b[Dc\x1b[201~"); CHECK_STR(t, "ab[Dc"); free(t);          /* a left arrow in the paste is text */
    t = KT("\x1b[200~ab\x1b[201~\x1b[Dc"); CHECK_STR(t, "acb"); free(t);            /* a left arrow after the paste moves the caret */
    t = KT("x\x1b[200~y\x1b[201~\x7f"); CHECK_STR(t, "x"); free(t);                  /* a real backspace after the paste */
    t = KT("\x1b[200~a\r\nb\rc\x1b[201~"); CHECK_STR(t, "a\nb\nc"); free(t);       /* CRLF and CR are line breaks */
    t = KT("\x1b[200~a\tb\x1b[201~"); CHECK_STR(t, "a\tb"); free(t);                 /* tab stays */
    t = KT("\x1b[200~\x07\x1bxy\x00z\x1b[201~"); CHECK_STR(t, "xyz"); free(t);     /* controls and ESC go, the letters stay */
    t = KT("\x1b[200~日本 🙂\x1b[201~"); CHECK_STR(t, "日本 🙂"); free(t);             /* UTF-8 */
    t = KT("\x1b[200~a\xff" "b\x1b[201~"); CHECK_STR(t, "a\xef\xbf\xbd" "b"); free(t);   /* a bad byte is U+FFFD, as in the editor */
    t = KT("\x1b[200~ \n \x1b[201~"); CHECK_STR(t, ""); free(t);                   /* only whitespace: nothing */
    t = KT("\x1b[200~abc"); CHECK_STR(t, "abc"); free(t);                           /* unfinished: to the end */
    t = KT("\x1b[200~a\x1b[201~b\x1b[201~"); CHECK_STR(t, "ab"); free(t);          /* a stray end marker is dropped */
    t = KT("\x1b" "fskip"); CHECK_STR(t, "skip"); free(t);                        /* Alt+f with the caret at the end: nothing to the right */
    /* the editor's own keys, so the preview and the replay agree: Ctrl-A/E/B/F, Ctrl-D, Ctrl-K, Alt+b/f, Alt+Backspace, Ctrl+arrows */
    t = KT("abc\x01x\x05y"); CHECK_STR(t, "xabcy"); free(t);                    /* Ctrl-A line start, Ctrl-E line end */
    t = KT("abcd\x02\x02x\x06y"); CHECK_STR(t, "abxcyd"); free(t);             /* Ctrl-B, Ctrl-F */
    t = KT("abcd\x02\x04"); CHECK_STR(t, "abc"); free(t);                       /* Ctrl-D deletes after the caret */
    t = KT("abcd\x02\x02\x0b"); CHECK_STR(t, "ab"); free(t);                  /* Ctrl-K deletes to the end of the line */
    t = KT("ab cd\x02\x02\x15"); CHECK_STR(t, "cd"); free(t);                  /* Ctrl-U deletes from the start to the caret */
    t = KT("one two three\x1b" "bX"); CHECK_STR(t, "one two Xthree"); free(t);      /* Alt+b: the caret goes to the word's start */
    t = KT("one two\x1b\x7f"); CHECK_STR(t, "one"); free(t);                    /* Alt+Backspace: the word before the caret */
    t = KT("one two\x1b[1;5D\x1b[1;5DX"); CHECK_STR(t, "Xone two"); free(t);     /* Ctrl+Left twice: back past two words */
    t = KT("one two\x1b[1;5D\x1b[1;5C!"); CHECK_STR(t, "one two!"); free(t);     /* Ctrl+Right: forward to the end */
    t = KT("   \n  "); CHECK_STR(t, ""); free(t);
    /* queue API */
    term_queue_clear(); CHECK(term_queue_count() == 0 && term_queue_pop() == NULL && term_queue_peek() == NULL);
    term_queue_push("one"); term_queue_push(""); term_queue_push("two");
    CHECK(term_queue_count() == 2); CHECK_STR(term_queue_peek(), "one");
    t = term_queue_pop(); CHECK_STR(t, "one"); free(t);
    t = term_queue_pop(); CHECK_STR(t, "two"); free(t);
    CHECK(term_queue_count() == 0);
    /* a queued /command or !line waits for the REPL, but does not hold back the messages
       behind it: those are what the model is waiting for */
    term_queue_push("/save f.md"); term_queue_push("behind it"); term_queue_push("!ls");
    t = term_queue_pop_plain(); CHECK_STR(t, "behind it"); free(t);
    CHECK(term_queue_count() == 2 && term_queue_pop_plain() == NULL);
    CHECK_STR(term_queue_peek(), "/save f.md");
    /* a queued message waits; it is only taken out, never acted on in flight */
    term_queue_clear();
    term_queue_push("look at this too"); CHECK(term_queue_count() == 1);
    t = term_queue_pop_plain(); CHECK_STR(t, "look at this too"); free(t);
    CHECK(term_queue_count() == 0);
    #undef KT
    /* a wrapped row continues in the colours its line opened: the repaint re-sends them (term_sgr_track) */
    { sbuf st; sb_init(&st);
      term_sgr_track(&st, "\x1b[2mone two\x1b[0m", strlen("\x1b[2mone two\x1b[0m")); CHECK(st.len == 0);
      term_sgr_track(&st, "\x1b[2mfirst", strlen("\x1b[2mfirst")); CHECK_STR(st.data, "\x1b[2m");
      term_sgr_track(&st, "\x1b[38;5;132mx\x1b[m", strlen("\x1b[38;5;132mx\x1b[m")); CHECK(st.len == 0);
      term_sgr_track(&st, "\x1b[1m\x1b[32mok", strlen("\x1b[1m\x1b[32mok")); CHECK_STR(st.data, "\x1b[1m\x1b[32m");
      sb_free(&st); }
    /* the rate limits of a response: its anthropic-ratelimit-* headers, per model (util.c) */
    { ratelimit_info rl; memset(&rl, 0, sizeof rl);
      const char *h1 = "anthropic-ratelimit-input-tokens-remaining: 1999000\r\n";
      ratelimit_header(&rl, h1, strlen(h1)); CHECK(rl.have && rl.in_rem == 1999000 && rl.in_lim == 0);
      const char *h2 = "Anthropic-RateLimit-Requests-Limit:  1000 \r\n";
      ratelimit_header(&rl, h2, strlen(h2)); CHECK(rl.req_lim == 1000);
      const char *h3 = "anthropic-ratelimit-output-tokens-limit: 400000\r\n";
      ratelimit_header(&rl, h3, strlen(h3)); CHECK(rl.out_lim == 400000);
      const char *h4 = "anthropic-ratelimit-input-tokens-reset: 2026-10-15T10:00:00Z\r\n";
      ratelimit_header(&rl, h4, strlen(h4)); CHECK_STR(rl.reset, "2026-10-15T10:00:00Z");
      const char *h5 = "anthropic-ratelimit-tokens-remaining: 5\r\n";   /* the aggregate is not one of the three */
      ratelimit_header(&rl, h5, strlen(h5)); CHECK(rl.in_rem == 1999000);
      const char *h6 = "content-type: application/json\r\n";
      ratelimit_header(&rl, h6, strlen(h6)); CHECK(rl.out_rem == 0);
      const char *h7 = "anthropic-ratelimit-requests-remaining: many\r\n";   /* not a number: ignored */
      ratelimit_header(&rl, h7, strlen(h7)); CHECK(rl.req_rem == 0);
      ratelimit_info none; memset(&none, 0, sizeof none);
      ratelimit_set("claude-test-a", &none); CHECK(!ratelimit_get("claude-test-a", &rl));   /* nothing to keep */
      ratelimit_set("claude-test-a", &rl); ratelimit_info back; CHECK(ratelimit_get("claude-test-a", &back) && back.in_rem == 1999000);
      CHECK(!ratelimit_get("claude-test-b", &back)); }
    /* prompt history keeps the latest 100 queries */
    for (int i = 0; i < 150; i++) { char l[32]; snprintf(l, sizeof l, "query %d", i); hist_add(l); }
    CHECK(hist_count() == 100);
    CHECK_STR(hist_get(0), "query 50"); CHECK_STR(hist_get(99), "query 149"); CHECK(hist_get(100) == NULL);
    hist_add("query 149"); CHECK(hist_count() == 100);   /* consecutive duplicates collapse */
}

/* ---------- skills ---------- */
/* the skills this test made: the bundled ones (the checkout's skills/, which the build reads) are left out */
static int skills_load_unbundled(void) {
    int n = 0;
    skills_load();
    for (int i = 0; i < skills_count(); i++) if (strcmp(skill_get(i)->source, "bundled")) n++;
    return n;
}

static void test_skills(void) {
    char dir[] = "/tmp/crowtest_s_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
    char old[4096]; CHECK(getcwd(old, sizeof old) != NULL); CHECK(chdir(dir) == 0);
    setenv("XDG_CONFIG_HOME", dir, 1);   /* keep user skills out of the picture */
    CHECK(skills_load_unbundled() == 0);   /* the checkout's own skills/ are bundled: counted apart */
    CHECK(mkdir_p(".corbienest/skills/rev") == 0);
    const char *body = "---\nname: rev\ndescription: \"Review things\"\n---\n\nReview: $ARGUMENTS\nEnd.\n";
    CHECK(write_whole_file(".corbienest/skills/rev/SKILL.md", body, strlen(body)) == 0);
    const char *flat = "No frontmatter here.\n";
    CHECK(write_whole_file(".corbienest/skills/plain.md", flat, strlen(flat)) == 0);
    CHECK(mkdir_p(".corbienest/skills/tester") == 0);   /* a role: its frontmatter names a model */
    const char *role = "---\nname: tester\ndescription: Writes the tests\nmodel: qwen3-coder:30b\n---\nWrite the tests.\n";
    CHECK(write_whole_file(".corbienest/skills/tester/SKILL.md", role, strlen(role)) == 0);
    CHECK(skills_load_unbundled() == 3);
    const skill_t *s = skill_find("/rev"); CHECK(s != NULL);
    if (s) CHECK(s->model == NULL);   /* no model: inherit the one in use */
    s = skill_find("tester"); CHECK(s != NULL);
    if (s) { CHECK_STR(s->model, "qwen3-coder:30b"); CHECK_STR(s->body, "Write the tests.\n"); }
    s = skill_find("/rev"); CHECK(s != NULL);
    if (s) {
        CHECK_STR(s->desc, "Review things"); CHECK_STR(s->body, "Review: $ARGUMENTS\nEnd.\n"); CHECK_STR(s->source, "project");
        char *e = skill_expand(s, "a.c b.c");
        CHECK(strstr(e, "Review: a.c b.c\nEnd.") != NULL); CHECK(strstr(e, "<skill name=\"rev\"") != NULL); CHECK(!strstr(e, "Arguments:"));
        free(e);
    }
    s = skill_find("plain"); CHECK(s != NULL);
    if (s) {
        CHECK_STR(s->body, flat);
        char *e = skill_expand(s, "x"); CHECK(strstr(e, "Arguments: x") != NULL); free(e);   /* no $ARGUMENTS: appended */
    }
    CHECK(skill_find("missing") == NULL);
    char path[512]; CHECK(skill_scaffold("new-one", path, sizeof path) == 0); CHECK(is_file(path));
    CHECK(skill_scaffold("new-one", path, sizeof path) == 1);
    CHECK(skill_scaffold("bad name!", path, sizeof path) == -1);
    CHECK(skills_load_unbundled() == 4);   /* rev, plain, tester and the scaffolded one */
    char *sec = skills_prompt_section(); CHECK(sec && strstr(sec, "/new-one") && strstr(sec, "written in C")); free(sec);
    CHECK(chdir(old) == 0);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
}

/* ---------- finding files for @mentions ---------- */
static const char *match_at(file_match *m, int n, int i) { return i < n ? m[i].path : "(none)"; }
static bool match_has(file_match *m, int n, const char *path) {
    for (int i = 0; i < n; i++) if (!strcmp(m[i].path, path)) return true;
    return false;
}
static void test_files(void) {
    /* which word is a mention: an '@' that starts a word, up to the cursor */
    CHECK(mention_start("look at @src/te", 15) == 9);
    CHECK(mention_start("@", 1) == 1);
    {   /* what the input field colours: the mentions that name something that is there */
        char dir[] = "/tmp/corbie_span_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
        sbuf f, t; sb_init(&f); sb_init(&t); sb_printf(&f, "%s/a.c", dir);
        write_whole_file(f.data, "x", 1);
        sb_printf(&t, "see @%s, and @%s/nope.c or mail@%s @ @%s", f.data, dir, f.data, dir);
        size_t sp[4][2]; int n = mention_spans(t.data, t.len, sp, 4);
        CHECK(n == 2);
        CHECK(sp[0][0] == 4 && sp[0][1] == 5 + f.len);                 /* from the '@', without the comma */
        CHECK(sp[1][1] == t.len && t.data[sp[1][0]] == '@');           /* a directory counts; "mail@…" and a lone '@' do not */
        CHECK(mention_spans(t.data, t.len, sp, 1) == 1 && mention_spans("", 0, sp, 4) == 0);
        remove(f.data); rmdir(dir); sb_free(&f); sb_free(&t);
    }
    CHECK(mention_start("@a b", 4) == -1);                   /* the cursor is in the next word */
    CHECK(mention_start("@a b", 2) == 1);                    /* ... or still in the mention */
    CHECK(mention_start("mail me at a@b.org", 18) == -1);    /* not at the start of a word */
    CHECK(mention_start("no mention", 10) == -1);
    CHECK(mention_start("one\n@two", 8) == 5);               /* a new line starts a word too */

    char dir[] = "/tmp/crowtest_f_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
    char old[4096]; CHECK(getcwd(old, sizeof old) != NULL);
    char proj[64]; snprintf(proj, sizeof proj, "%s/proj", dir);
    CHECK(mkdir_p(proj) == 0); CHECK(chdir(proj) == 0);
    const char *files[] = { "Makefile", "README.md", "src/term.c", "src/tools.c", "src/deep/er/terminal_notes.md", "tests/test_unit.c",
                            "docs/term.md", "docs/guide/intro.md", ".git/config", "node_modules/pkg/term.js", "a b/spaced.txt", "../lib/parser.c", "../lib/parse_util.h" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char *d = xstrdup(files[i]), *sl = strrchr(d, '/');
        if (sl) { *sl = 0; CHECK(mkdir_p(d) == 0); }
        free(d);
        CHECK(write_whole_file(files[i], "x\n", 2) == 0);
    }
    file_match *m; int n;

    n = file_complete("", false, 50, &m);                     /* nothing typed yet: the directory itself, in order */
    CHECK(n == 5); CHECK_STR(match_at(m, n, 0), "Makefile"); CHECK_STR(match_at(m, n, 1), "README.md");
    CHECK_STR(match_at(m, n, 2), "docs/"); CHECK(n > 2 && m[2].dir); CHECK_STR(match_at(m, n, 4), "tests/");
    CHECK(!match_has(m, n, ".git/") && !match_has(m, n, "node_modules/") && !match_has(m, n, "a b/"));
    file_matches_free(m, n);

    n = file_complete("term", false, 50, &m);                 /* a name is a hint, not a path: found wherever it is */
    CHECK_STR(match_at(m, n, 0), "docs/term.md"); CHECK_STR(match_at(m, n, 1), "src/term.c");
    CHECK_STR(match_at(m, n, 2), "src/deep/er/terminal_notes.md");   /* the nearer ones first */
    CHECK(n == 3);                                            /* nothing out of .git or node_modules */
    file_matches_free(m, n);

    n = file_complete("te", false, 50, &m);                   /* in the directory itself first, then below, then "contains" */
    CHECK_STR(match_at(m, n, 0), "tests/"); CHECK(match_has(m, n, "src/term.c")); CHECK(match_has(m, n, "tests/test_unit.c"));
    file_matches_free(m, n);

    n = file_complete("UNIT", false, 50, &m);                 /* part of the name, in any case */
    CHECK(n == 1); CHECK_STR(match_at(m, n, 0), "tests/test_unit.c");
    file_matches_free(m, n);

    n = file_complete("tlc", false, 50, &m);                  /* last resort: the letters in that order (ToLs.C) */
    CHECK(n == 1); CHECK_STR(match_at(m, n, 0), "src/tools.c");
    file_matches_free(m, n);

    n = file_complete("src/t", false, 50, &m);                /* a directory in front: the search is below it */
    CHECK_STR(match_at(m, n, 0), "src/term.c"); CHECK_STR(match_at(m, n, 1), "src/tools.c");
    CHECK_STR(match_at(m, n, 2), "src/deep/er/terminal_notes.md"); CHECK(n == 3);
    file_matches_free(m, n);

    n = file_complete("../lib/pa", false, 50, &m);            /* ... also when it is outside the working directory */
    CHECK(n == 2); CHECK_STR(match_at(m, n, 0), "../lib/parse_util.h"); CHECK_STR(match_at(m, n, 1), "../lib/parser.c");
    file_matches_free(m, n);

    n = file_complete("..", false, 50, &m);                   /* what was typed is a directory: going into it comes first */
    CHECK_STR(match_at(m, n, 0), "../"); CHECK(n >= 1 && m[0].dir);
    file_matches_free(m, n);
    n = file_complete("src", false, 50, &m);
    CHECK_STR(match_at(m, n, 0), "src/"); CHECK(n == 1);      /* (and once only) */
    file_matches_free(m, n);

    n = file_complete("d", true, 50, &m);                     /* directories only (/cd) */
    CHECK_STR(match_at(m, n, 0), "docs/"); CHECK(match_has(m, n, "src/deep/")); CHECK(!match_has(m, n, "docs/term.md"));
    file_matches_free(m, n);

    n = file_complete("te", false, 2, &m); CHECK(n == 2); file_matches_free(m, n);     /* never more than asked for */
    n = file_complete("no-such-thing", false, 50, &m); CHECK(n == 0); file_matches_free(m, n);
    n = file_complete("nowhere/x", false, 50, &m); CHECK(n == 0); file_matches_free(m, n);

    /* "@name" as sent: the one file by that name */
    int hits = 0; sbuf names; sb_init(&names);
    char *f = file_find_named("test_unit.c", &hits, &names); CHECK_STR(f, "tests/test_unit.c"); CHECK(hits == 1); free(f);
    f = file_find_named("deep/er/terminal_notes.md", &hits, NULL); CHECK_STR(f, "src/deep/er/terminal_notes.md"); free(f);   /* or path ending */
    f = file_find_named("unit.c", &hits, NULL); CHECK(f == NULL && hits == 0);         /* a whole name, not a part of one */
    f = file_find_named("deep", &hits, NULL); CHECK(f == NULL && hits == 0);           /* files, not directories */
    f = file_find_named("../lib/nope.c", &hits, NULL); CHECK(f == NULL && hits == 0);  /* a place was named: no guessing */
    CHECK(write_whole_file("docs/guide/term.c", "x\n", 2) == 0);
    sleep(2);                                                                           /* (the listing is kept for a moment) */
    sb_clear(&names);
    f = file_find_named("term.c", &hits, &names); CHECK(f == NULL && hits == 2);        /* two of them: neither is guessed */
    CHECK(names.data && strstr(names.data, "src/term.c") && strstr(names.data, "docs/guide/term.c"));
    sb_free(&names);

    /* a "!line" is the shell's: which word the cursor is in, and whether a command goes there */
    bool prog = false;
    CHECK(shell_word("!", 1, &prog) == 1 && prog);
    CHECK(shell_word("!gi", 3, &prog) == 1 && prog);
    CHECK(shell_word("!git st", 7, &prog) == 5 && !prog);              /* an argument */
    CHECK(shell_word("!git ", 5, &prog) == 5 && !prog);                /* ... also one that has not been started */
    CHECK(shell_word("!cat a.c | gre", 14, &prog) == 11 && prog);      /* after a pipe a command again */
    CHECK(shell_word("!make && ./te", 13, &prog) == 9 && prog);
    CHECK(shell_word("!sort <in", 9, &prog) == 7 && !prog);            /* a redirection is followed by a file */
    CHECK(shell_word("!cc --out=bu", 12, &prog) == 10 && !prog);       /* and so may a '=' be */
    CHECK(shell_word("!echo \"two wor", 14, &prog) == -1);            /* inside quotes it is text: nothing to complete */
    CHECK(shell_word("!echo 'it' sr", 13, &prog) == 11 && !prog);      /* ... which ends where they do */
    CHECK(shell_word("!echo it\\'s sr", 14, &prog) == 12 && !prog);    /* an escaped quote opens nothing */
    CHECK(shell_word("!ls my\\ fi", 10, &prog) == 4 && !prog);         /* and an escaped blank is part of the word */
    CHECK(shell_word("!git st", 0, &prog) == -1);
    char *esc = shell_escape("src/deep/term-notes_v2.md"); CHECK_STR(esc, "src/deep/term-notes_v2.md"); free(esc);
    esc = shell_escape("~/a(1)&b$c.txt"); CHECK_STR(esc, "~/a\\(1\\)\\&b\\$c.txt"); free(esc);   /* what the shell would take apart */
    esc = shell_escape("caf\xc3\xa9.md"); CHECK_STR(esc, "caf\xc3\xa9.md"); free(esc);

    /* ... and the programs $PATH has for it */
    char *path_was = getenv("PATH") ? xstrdup(getenv("PATH")) : NULL;
    const char *bins[] = { "bin1/zzcrow-long", "bin1/zzcrow", "bin2/zzcrow", "bin2/zzcroak", "bin1/zzcrow.txt" };
    CHECK(mkdir_p("bin1/zzcrowdir") == 0 && mkdir_p("bin2") == 0);
    for (size_t i = 0; i < sizeof bins / sizeof *bins; i++)
        CHECK(write_whole_file(bins[i], "#!/bin/sh\n", 10) == 0 && chmod(bins[i], strstr(bins[i], ".txt") ? 0644 : 0755) == 0);
    sbuf pv; sb_init(&pv); sb_printf(&pv, "%s/bin1:%s/nowhere::%s/bin2", proj, proj, proj);
    setenv("PATH", pv.data, 1);
    n = command_complete("zzcro", 50, &m);                    /* the short names first, each once; no text file, no directory */
    CHECK(n == 3); CHECK_STR(match_at(m, n, 0), "zzcrow"); CHECK_STR(match_at(m, n, 1), "zzcroak"); CHECK_STR(match_at(m, n, 2), "zzcrow-long");
    file_matches_free(m, n);
    n = command_complete("zzcro", 2, &m); CHECK(n == 2); file_matches_free(m, n);
    n = command_complete("zz-none", 50, &m); CHECK(n == 0); file_matches_free(m, n);
    if (path_was) setenv("PATH", path_was, 1); else unsetenv("PATH");
    free(path_was); sb_free(&pv);

    CHECK(chdir(old) == 0);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
}

/* ---------- /api/show ---------- */
static void test_model_info(void) {
    model_info mi;
    model_info_parse("{\"model_info\":{\"general.architecture\":\"gptoss\",\"gptoss.context_length\":131072},"
                     "\"parameters\":\"top_k 20\\ndraft_num_predict              4\\ntemperature 1\","
                     "\"details\":{\"family\":\"gptoss\",\"parameter_size\":\"20.9B\"},"
                     "\"capabilities\":[\"completion\",\"tools\",\"thinking\"]}", &mi);
    CHECK(mi.context_length == 131072 && mi.draft == 4);
    CHECK_STR(mi.family, "gptoss");
    CHECK(mi.caps_known && mi.tools && mi.thinking);
    /* a chat-only model, no draft head */
    model_info_parse("{\"model_info\":{\"llama.context_length\":8192},\"details\":{\"family\":\"llama\"},\"capabilities\":[\"completion\"]}", &mi);
    CHECK(mi.context_length == 8192 && mi.draft == -1 && mi.caps_known && !mi.tools && !mi.thinking);
    /* an older server: no capabilities[] at all — the caller falls back on /api/tags */
    model_info_parse("{\"model_info\":{\"llama.context_length\":4096}}", &mi);
    CHECK(mi.context_length == 4096 && !mi.caps_known && !mi.family[0]);
    /* not JSON, or nothing at all */
    model_info_parse("<html>502</html>", &mi); CHECK(mi.context_length == 0 && mi.draft == -1 && !mi.caps_known);
    model_info_parse(NULL, &mi); CHECK(mi.context_length == 0 && mi.draft == -1);
}

/* ---------- effort: what a model can be set to, and what is sent as "think" ---------- */
static model_info mi_of(const char *show_json) { model_info mi; model_info_parse(show_json, &mi); return mi; }
static bool same_level(const char *a, const char *b) { return a && b ? !strcmp(a, b) : a == b; }
#define THINKS(mi, when, quiet, followup, effort, kind, lvl) do { const char *_l = NULL; \
    think_kind _k = think_decide((when), (quiet), (followup), (effort), &(mi), &_l); \
    if (_k == (kind) && same_level(_l, (lvl))) g_pass++; \
    else { g_fail++; fprintf(stderr, "  FAIL %s:%d: think_decide -> %d \"%s\", wanted %d \"%s\"\n", __FILE__, __LINE__, (int)_k, _l ? _l : "", (int)(kind), (lvl) ? (lvl) : ""); } } while (0)
/* the suggested next message: the cleaned first line of the model's reply, or nothing */
static void test_suggest(void) {
    char *t;
    t = suggest_clean("Run the tests again and show me the failures", false);
    CHECK_STR(t, "Run the tests again and show me the failures"); free(t);
    t = suggest_clean("  \"Now add a test for that.\"\n\nExplanation: more text", false);
    CHECK_STR(t, "Now add a test for that."); free(t);
    t = suggest_clean("User: yes, go ahead", false);
    CHECK_STR(t, "yes, go ahead"); free(t);
    t = suggest_clean("`commit it`", false);
    CHECK_STR(t, "commit it"); free(t);
    t = suggest_clean("ok\x1b[31m red\x07", false);            /* control bytes are dropped: the field draws them raw */
    CHECK_STR(t, "ok[31m red"); free(t);
    CHECK(suggest_clean("NO_SUGGESTION", false) == NULL);
    CHECK(suggest_clean("   \n", false) == NULL);
    CHECK(suggest_clean("", false) == NULL);
    CHECK(suggest_clean(NULL, false) == NULL);
    CHECK(suggest_clean("a complete sentence", true) == NULL);   /* cut off by the token cap: not offered */
    char long_msg[400]; memset(long_msg, 'x', sizeof long_msg - 1); long_msg[sizeof long_msg - 1] = 0;
    t = suggest_clean(long_msg, false);
    CHECK(t && strlen(t) == SUGGEST_MAX);
    free(t);
    /* a multi-byte character at the cut is not split */
    char big[300]; memset(big, 'a', 199); big[199] = (char)0xC3; big[200] = (char)0xA5; big[201] = 0;   /* å straddles byte 200 */
    t = suggest_clean(big, false);
    CHECK(t && strlen(t) == 199);
    free(t);
}

static void test_effort(void) {
    model_info none  = mi_of("{\"capabilities\":[\"completion\",\"tools\"]}");
    model_info onoff = mi_of("{\"details\":{\"family\":\"qwen3\"},\"capabilities\":[\"completion\",\"thinking\"]}");
    model_info oss   = mi_of("{\"details\":{\"family\":\"gptoss\"},\"capabilities\":[\"thinking\"]}");
    model_info q38   = mi_of("{\"details\":{\"family\":\"qwen35\"},\"capabilities\":[\"thinking\"],\"modelfile\":\"# made by ollama\\nFROM /blob\\nRENDERER qwen3.8\\nPARSER qwen3.5\\n\"}");
    model_info q35   = mi_of("{\"details\":{\"family\":\"qwen35\"},\"capabilities\":[\"thinking\"],\"modelfile\":\"FROM /blob\\nRENDERER qwen3.5\\n\"}");
    model_info decl  = mi_of("{\"details\":{\"family\":\"qwen35\"},\"capabilities\":[\"thinking\"],\"modelfile\":\"RENDERER qwen3.8\\n\","
                             "\"thinking\":{\"values\":[false,\"low\",\"medium\",\"xhigh\"],\"default\":\"medium\"}}");
    model_info cloud = mi_of("{\"capabilities\":[\"completion\"],\"thinking\":{\"values\":[\"low\",\"high\",\"max\"],\"default\":\"max\"}}");

    /* what each offers: the server's list when it sends one, else the table, else on/off */
    CHECK(!none.thinking && !none.think_off && !none.think_on && none.n_think_levels == 0);
    CHECK(onoff.think_off && onoff.think_on && onoff.n_think_levels == 0 && !onoff.think_declared); CHECK_STR(onoff.think_default, "on");
    CHECK(!oss.think_off && !oss.think_on && oss.n_think_levels == 3); CHECK_STR(oss.think_levels[0], "low"); CHECK_STR(oss.think_levels[2], "high"); CHECK_STR(oss.think_default, "medium");
    CHECK_STR(q38.renderer, "qwen3.8"); CHECK(q38.think_off && !q38.think_on && q38.n_think_levels == 3);
    CHECK(q35.think_on && q35.n_think_levels == 0);                       /* the same family without that renderer is on/off */
    CHECK(decl.think_declared && decl.think_off && !decl.think_on && decl.n_think_levels == 3); CHECK_STR(decl.think_levels[2], "xhigh");   /* declared beats the table */
    CHECK(cloud.thinking && !cloud.think_off && cloud.n_think_levels == 3); CHECK_STR(cloud.think_default, "max");   /* it says how it thinks: it thinks */

    CHECK(effort_name_ok("xhigh") && effort_name_ok("off") && !effort_name_ok("") && !effort_name_ok("High") && !effort_name_ok("a b") && !effort_name_ok("0123456789abcdefg"));
    CHECK(effort_supported(&oss, "high") == 1 && effort_supported(&oss, "off") == 0 && effort_supported(&oss, "on") == 0 && effort_supported(&oss, "max") == 0);
    CHECK(effort_supported(&onoff, "off") == 1 && effort_supported(&onoff, "on") == 1 && effort_supported(&onoff, "high") == -1 && effort_supported(&onoff, "xhigh") == 0);
    CHECK(effort_supported(&none, "off") == 0 && effort_supported(&none, "high") == 0);
    CHECK(effort_supported(&decl, "xhigh") == 1 && effort_supported(&decl, "high") == 0);

    bool mapped;
    CHECK_STR(effort_resolve(&decl, "high", &mapped), "xhigh"); CHECK(mapped);    /* saved under 0.33, run under 0.34.3 */
    CHECK_STR(effort_resolve(&oss, "max", &mapped), "high"); CHECK(mapped);
    CHECK_STR(effort_resolve(&oss, "low", &mapped), "low"); CHECK(!mapped);
    CHECK_STR(effort_resolve(&onoff, "high", &mapped), "high"); CHECK(!mapped);   /* passed on as it is */
    CHECK(effort_resolve(&decl, "ultra", NULL) == NULL && effort_resolve(&oss, "off", NULL) == NULL && effort_resolve(&onoff, "xhigh", NULL) == NULL);
    CHECK(effort_resolve(&none, "high", NULL) == NULL && effort_resolve(&oss, NULL, NULL) == NULL);

    /* the table of effort entries */
    CHECK(effort_get("m:7b") == NULL);
    effort_set("m:7b", "high"); effort_set("reg:5000/ns/other", "off"); effort_set("m:7b", "low");
    CHECK_STR(effort_get("m:7b"), "low"); CHECK_STR(effort_get("reg:5000/ns/other"), "off"); CHECK(g_cfg.n_efforts == 2);
    effort_set("plain", "on"); CHECK_STR(effort_get("plain:latest"), "on");   /* one model to the server */
    effort_set("plain:latest", "off"); CHECK_STR(effort_get("plain"), "off"); CHECK(g_cfg.n_efforts == 3);
    effort_set("m:7b", NULL); CHECK(effort_get("m:7b") == NULL && g_cfg.n_efforts == 2);
    effort_set("reg:5000/ns/other", NULL); effort_set("plain", NULL); effort_set("never-set", NULL); CHECK(g_cfg.n_efforts == 0);
    CHECK(model_same("a:latest", "a") && model_same("a", "a") && !model_same("a:7b", "a") && !model_same("a", NULL) && !model_same("reg:5000/ns/m", "reg:5000/ns/n"));
    CHECK(model_is_cloud("gpt-oss:120b-cloud") && model_is_cloud("kimi-k3:cloud") && !model_is_cloud("cloud") && !model_is_cloud("cloudy:7b") && !model_is_cloud(NULL));

    /* think_decide(when, quiet, followup, effort): when -1 auto / 0 off / 1 every call */
    /* a model that cannot think is never sent anything but false */
    THINKS(none, -1, false, false, NULL, THINK_OMIT, NULL);  THINKS(none, 1, false, false, NULL, THINK_OMIT, NULL);
    THINKS(none, 0, false, false, NULL, THINK_FALSE, NULL);  THINKS(none, 1, true, false, NULL, THINK_FALSE, NULL);
    THINKS(none, -1, true, false, NULL, THINK_OMIT, NULL);   THINKS(none, 1, false, false, "high", THINK_OMIT, NULL);
    /* on/off, nothing set: the old behaviour */
    THINKS(onoff, -1, false, false, NULL, THINK_OMIT, NULL); THINKS(onoff, -1, false, true, NULL, THINK_FALSE, NULL);
    THINKS(onoff, 1, false, false, NULL, THINK_TRUE, NULL);  THINKS(onoff, 1, false, true, NULL, THINK_TRUE, NULL);
    THINKS(onoff, 0, false, false, NULL, THINK_FALSE, NULL); THINKS(onoff, 1, true, false, NULL, THINK_FALSE, NULL);
    THINKS(onoff, -1, false, false, "on", THINK_TRUE, NULL); THINKS(onoff, -1, false, true, "on", THINK_FALSE, NULL);
    THINKS(onoff, 1, false, true, "off", THINK_FALSE, NULL);                                /* off for this model beats /think on */
    /* a name passed on in hope is "on" to such a model: it rests for the tool rounds like on does */
    THINKS(onoff, -1, false, false, "high", THINK_LEVEL, "high"); THINKS(onoff, -1, false, true, "high", THINK_FALSE, NULL);
    THINKS(onoff, 1, false, true, "high", THINK_LEVEL, "high");
    /* a level of the model's own is part of the prompt: every call that shares it carries it */
    THINKS(q38, -1, false, false, "low", THINK_LEVEL, "low"); THINKS(q38, -1, false, true, "low", THINK_LEVEL, "low");
    THINKS(q38, -1, true, true, "low", THINK_LEVEL, "low");                                  /* compaction: the conversation's own prompt */
    THINKS(q38, -1, true, false, "low", THINK_FALSE, NULL);                                  /* the memory update: a prompt of its own */
    THINKS(q38, 0, false, false, "low", THINK_FALSE, NULL);                                  /* /think off */
    THINKS(q38, -1, false, false, NULL, THINK_OMIT, NULL); THINKS(q38, -1, false, true, NULL, THINK_FALSE, NULL);
    THINKS(q38, 1, false, false, NULL, THINK_LEVEL, "medium");                               /* no "true" for it: its default, by name */
    THINKS(q38, -1, false, false, "max", THINK_LEVEL, "high");                               /* as hard as it goes */
    THINKS(decl, -1, false, true, "high", THINK_LEVEL, "xhigh");
    THINKS(decl, -1, false, false, "ultra", THINK_OMIT, NULL);                               /* a saved level it no longer has: left to the model */
    /* gpt-oss cannot stop: never false, and nothing that changes the prompt between two calls that share it */
    THINKS(oss, -1, false, false, NULL, THINK_OMIT, NULL); THINKS(oss, -1, false, true, NULL, THINK_OMIT, NULL);
    THINKS(oss, -1, true, true, NULL, THINK_OMIT, NULL);                                     /* compaction */
    THINKS(oss, -1, true, false, NULL, THINK_LEVEL, "low");                                  /* memory update: as little as it can */
    THINKS(oss, 0, false, false, NULL, THINK_LEVEL, "low"); THINKS(oss, 0, false, true, "high", THINK_LEVEL, "low");
    THINKS(oss, 1, false, false, NULL, THINK_LEVEL, "medium"); THINKS(oss, 1, true, true, NULL, THINK_LEVEL, "medium");
    THINKS(oss, -1, false, true, "high", THINK_LEVEL, "high"); THINKS(oss, -1, true, true, "high", THINK_LEVEL, "high");
    THINKS(oss, -1, true, false, "high", THINK_LEVEL, "low");
    THINKS(oss, -1, false, false, "off", THINK_OMIT, NULL);                                  /* from a hand-edited config: not something it can do, so left to the model */
    THINKS(cloud, 1, false, false, NULL, THINK_LEVEL, "max");
    /* glimmer writes its "off" into the top of the prompt like any level ("Reasoning strength: none."): no flip mid-prompt */
    model_info glim = mi_of("{\"details\":{\"family\":\"muse-glimmer\"},\"capabilities\":[\"thinking\"]}");
    model_info glimd = mi_of("{\"details\":{\"family\":\"muse-glimmer\"},\"capabilities\":[\"thinking\"],\"thinking\":{\"values\":[false,\"low\",\"medium\",\"high\",\"max\"],\"default\":\"high\"}}");
    CHECK(glim.think_off && glim.think_off_top && glim.n_think_levels == 4 && glimd.think_off_top && glimd.think_declared && !q38.think_off_top && !onoff.think_off_top);
    THINKS(glim, -1, false, false, NULL, THINK_OMIT, NULL); THINKS(glim, -1, false, true, NULL, THINK_OMIT, NULL);
    THINKS(glimd, 1, false, true, NULL, THINK_LEVEL, "high"); THINKS(glimd, 1, true, true, NULL, THINK_LEVEL, "high");   /* compaction under /think on */
    THINKS(glim, -1, true, false, NULL, THINK_FALSE, NULL);  /* the memory update has a prompt of its own */
    THINKS(glim, 0, false, true, NULL, THINK_FALSE, NULL);   /* and off for good is the same on every call */
}

/* ---------- the advisor: where it runs, what it is shown ---------- */
static void test_advisor(void) {
    advisor_plan p;
    char *old_model = g_cfg.model; int old_ctx = g_cfg.num_ctx, old_actx = g_cfg.advisor_ctx, old_guid = g_cfg.advisor_guidance;
    g_cfg.model = "small:7b"; g_cfg.num_ctx = 32768; g_cfg.advisor_ctx = 0; g_cfg.advisor_guidance = GUIDANCE_NORMAL;
    const size_t BRIEF_NORMAL = ADVISOR_GUIDANCE[GUIDANCE_NORMAL].brief_max;
    advisor_plan_for("big:70b", 131072, &p);           /* another local model: a window of its own, memory back at once */
    CHECK(!p.same && !p.cloud && p.window == 16384 && p.send_ctx == 16384 && p.reply == 5461); CHECK_STR(p.keep_alive, "0");
    CHECK(p.budget == (size_t)(16384 - 5461 - 256) * 3);
    g_cfg.advisor_ctx = 65536; advisor_plan_for("big:70b", 8192, &p);   /* never more than it was trained for */
    CHECK(p.window == 8192 && p.send_ctx == 8192 && p.reply == 2730);
    advisor_plan_for("big:70b", 0, &p); CHECK(p.window == 65536 && p.reply == ADVISOR_REPLY_MAX && p.budget == BRIEF_NORMAL);   /* however large: reading it is the wait */
    g_cfg.advisor_guidance = GUIDANCE_STRONG; advisor_plan_for("big:70b", 0, &p); CHECK(p.budget == ADVISOR_GUIDANCE[GUIDANCE_STRONG].brief_max && p.budget > BRIEF_NORMAL);   /* strong: shown more of it */
    g_cfg.advisor_guidance = GUIDANCE_NORMAL;
    g_cfg.advisor_ctx = 0; g_cfg.num_ctx = 0; advisor_plan_for("big:70b", 131072, &p);
    CHECK(p.window == ADVISOR_CTX_UNSET && p.send_ctx == 0);                                 /* left to the server: plan for little */
    g_cfg.num_ctx = 512; advisor_plan_for("small:7b", 131072, &p); CHECK(p.window == 512 && p.budget == ADVISOR_BRIEF_MIN);   /* too small to plan for: a floor, not an underflow */
    g_cfg.advisor_ctx = 100; advisor_plan_for("big:70b", 0, &p); CHECK(p.budget == ADVISOR_BRIEF_MIN); g_cfg.advisor_ctx = 0;
    g_cfg.num_ctx = 65536; advisor_plan_for("small:7b", 131072, &p);   /* the main model itself: called exactly as it always is */
    CHECK(p.same && !p.cloud && p.window == 65536 && p.send_ctx == 0 && p.keep_alive == NULL);
    g_cfg.model = "small"; advisor_plan_for("small:latest", 0, &p); CHECK(p.same); g_cfg.model = "small:7b";
    g_cfg.advisor_ctx = 8192; advisor_plan_for("gpt-oss:120b-cloud", 131072, &p);           /* a cloud model: nothing of ours to set */
    CHECK(p.cloud && !p.same && p.window == ADVISOR_CTX_CLOUD && p.send_ctx < 0); CHECK_STR(p.keep_alive, "");
    advisor_plan_for("tiny:cloud", 8192, &p); CHECK(p.window == 8192);
    /* a hosted API: runs elsewhere like a cloud model, with a larger answer (paid by the token used) */
    advisor_plan_for("xai:grok-4.7", 0, &p);
    CHECK(p.api && p.cloud && !p.same && p.send_ctx < 0 && p.window == ADVISOR_CTX_API && p.reply == ADVISOR_REPLY_API && p.budget == BRIEF_NORMAL); CHECK_STR(p.keep_alive, "");
    advisor_plan_for("anthropic:claude-opus-5", 1000000, &p); CHECK(p.api && p.window == 1000000 && p.reply == ADVISOR_REPLY_API);
    advisor_plan_for("openai:gpt-5.2", 0, &p); CHECK(p.api);
    advisor_plan_for("qwen3:32b", 0, &p); CHECK(!p.api && !p.cloud);   /* a tag is not a provider */
    g_cfg.model = old_model; g_cfg.num_ctx = old_ctx; g_cfg.advisor_ctx = old_actx;

    cJSON *msgs = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", "fix the parser"); cJSON_AddItemToArray(msgs, m);
    sbuf rules; sb_init(&rules); sb_puts(&rules, "RULES-HEAD\n"); for (int i = 0; i < 3000; i++) sb_puts(&rules, "rule "); sb_puts(&rules, "\nDONTS-AT-THE-END");
    char *b = advisor_brief(msgs, 0, 20000, "Working directory: /w · Linux x86_64", true, rules.data, "  Is my plan right?", 400);
    CHECK(strstr(b, "# Environment\nWorking directory: /w") == b);
    CHECK(strstr(b, "plan mode") != NULL);
    CHECK(strstr(b, "RULES-HEAD") && strstr(b, "DONTS-AT-THE-END") && strstr(b, "bytes cut"));   /* the rules lose their middle, not their end */
    CHECK(strstr(b, "# The agent's conversation so far\n") && strstr(b, "[user]\nfix the parser\n\n# What the agent asks you\nIs my plan right?\n"));
    const char *tail = "do not call tools.\n"; CHECK(strlen(b) > strlen(tail) && !strcmp(b + strlen(b) - strlen(tail), tail));   /* what a cut prompt keeps */
    CHECK(strlen(b) < 20000 && strstr(b, "under about 400 words"));
    free(b);
    b = advisor_brief(msgs, 0, 20000, "env", false, NULL, NULL, 700);
    CHECK(strstr(b, "under about 700 words") != NULL);
    CHECK(strstr(b, "(It did not say.") && !strstr(b, "plan mode") && !strstr(b, "project's rules"));
    free(b); sb_free(&rules); cJSON_Delete(msgs);

    CHECK_STR(strip_think_block("plain advice"), "plain advice");
    CHECK_STR(strip_think_block("  \n<think>hmm\nhmm</think>\n\nADVICE"), "ADVICE");
    CHECK_STR(strip_think_block("<think>never finished"), "");
    CHECK_STR(strip_think_block("say <think> later"), "say <think> later");

    /* a review or a check that finds nothing to change */
    CHECK(advisor_approves("LGTM") && advisor_approves("  LGTM.") && advisor_approves("**LGTM** — nothing to add") && advisor_approves("lgtm\n"));
    CHECK(advisor_approves("LGTM — the change is correct and complete, and the tests cover it."));   /* a sentence of its own is still a yes */
    CHECK(advisor_approves("LGTM: the fix is right and the tests still pass."));
    CHECK(!advisor_approves("LGTM, but the test for the empty field is missing: add it to tests/test_parse.c") && !advisor_approves("Not yet: run the tests") && !advisor_approves(""));
    CHECK(!advisor_approves("LGTM. You should still run the tests.") && !advisor_approves("LGTM once the import is fixed"));
    CHECK(!advisor_approves("LGTM overall. The parser now handles empty fields, the tests pass, and the change is small, which is good; one more thing to consider is the docs"));   /* too long to be only a yes */
    /* the levels, by name */
    CHECK(!ADVISOR_GUIDANCE[GUIDANCE_LIGHT].step_in && ADVISOR_GUIDANCE[GUIDANCE_NORMAL].step_in && ADVISOR_GUIDANCE[GUIDANCE_STRONG].step_in && ADVISOR_GUIDANCE[GUIDANCE_MAX].step_in);
    CHECK(tool_result_failed("bash", "make: *** [all] Error 1\nexit code: 2", false) && !tool_result_failed("bash", "ok\nexit code: 0", false));
    CHECK(!tool_result_failed("bash", "the log says exit code: 1 somewhere\nexit code: 0", false));   /* the command's own last line counts */
    CHECK(tool_result_failed("read_file", "error: no such file", true) && !tool_result_failed("read_file", "exit code: 3 is what the file says", false));
    CHECK(!tool_result_failed("advisor", "error: the advisor has been consulted 6 times", true) && !tool_result_failed("bash", NULL, false));
    CHECK(advisor_guidance_parse("strong") == GUIDANCE_STRONG && advisor_guidance_parse("MAX") == GUIDANCE_MAX && advisor_guidance_parse("ultra") == -1);
    CHECK(ADVISOR_GUIDANCE[GUIDANCE_NORMAL].uses == 3 && !ADVISOR_GUIDANCE[GUIDANCE_NORMAL].review && ADVISOR_GUIDANCE[GUIDANCE_STRONG].review && ADVISOR_GUIDANCE[GUIDANCE_MAX].check_first_edit);
    for (int i = 0; i < GUIDANCE_COUNT; i++) CHECK(ADVISOR_GUIDANCE[i].desc && *ADVISOR_GUIDANCE[i].desc && ADVISOR_GUIDANCE[i].name && *ADVISOR_GUIDANCE[i].name);
    CHECK(!ADVISOR_GUIDANCE[GUIDANCE_NORMAL].check_first_edit && ADVISOR_GUIDANCE[GUIDANCE_NORMAL].step_in && !ADVISOR_GUIDANCE[GUIDANCE_LIGHT].step_in && !ADVISOR_GUIDANCE[GUIDANCE_LIGHT].review);
    /* the review: strong only for a request that changed a lot, max for any change */
    CHECK(ADVISOR_GUIDANCE[GUIDANCE_STRONG].review && !ADVISOR_GUIDANCE[GUIDANCE_STRONG].review_any && !ADVISOR_GUIDANCE[GUIDANCE_STRONG].check_first_edit);
    CHECK(ADVISOR_GUIDANCE[GUIDANCE_MAX].review && ADVISOR_GUIDANCE[GUIDANCE_MAX].review_any && !ADVISOR_GUIDANCE[GUIDANCE_NORMAL].review_any);
    CHECK(diff_changed_lines("--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n") == 2);   /* the headers are not counted */
    CHECK(diff_changed_lines("--- a/x\n+++ b/x\n@@ -1,3 +1,3 @@\n-a\n-b\n+c\n d\n\\ No newline\n") == 3);
    CHECK(diff_changed_lines("+one\n+two") == 2 && diff_changed_lines(NULL) == 0 && diff_changed_lines("") == 0);
    for (int i = 1; i < GUIDANCE_COUNT; i++) CHECK(ADVISOR_GUIDANCE[i].uses > ADVISOR_GUIDANCE[i - 1].uses && ADVISOR_GUIDANCE[i].words > ADVISOR_GUIDANCE[i - 1].words);
    g_cfg.advisor_guidance = old_guid;

    /* the tool is on offer only while an advisor is set, and says so when called without one */
    char *old_adv = g_cfg.advisor;
    g_cfg.advisor = NULL;
    cJSON *defs = tools_definitions(); char *t = cJSON_PrintUnformatted(defs);
    CHECK(!strstr(t, "\"advisor\"") && !strstr(tools_summary_line(), "advisor")); free(t); cJSON_Delete(defs);
    sbuf out; sb_init(&out);
    CHECK(tools_execute("advisor", NULL, &out) == TOOL_ERROR && strstr(out.data, "no advisor is set")); sb_free(&out);
    g_cfg.advisor = "big:70b";
    defs = tools_definitions(); t = cJSON_PrintUnformatted(defs);
    CHECK(strstr(t, "\"name\":\"advisor\"") && strstr(tools_summary_line(), "task, advisor")); free(t);
    cJSON *last = cJSON_GetArrayItem(defs, cJSON_GetArraySize(defs) - 1);
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "function"), "name")->valuestring, "advisor");   /* last: the tools before it keep their place in the prompt */
    t = cJSON_PrintUnformatted(last); CHECK(strstr(t, "\"required\":[]") != NULL); free(t);             /* the question is optional */
    cJSON_Delete(defs);
    sb_init(&out); CHECK(tools_execute("advisor", NULL, &out) == TOOL_ERROR && strstr(out.data, "no advisor is set")); sb_free(&out);   /* set, but nothing to run it (main.c's hook) */
    g_cfg.advisor = old_adv;
}

/* strict enough to see a sequence cut at either end: every lead byte has its continuation bytes, and none stand alone */
static bool utf8_valid(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        p++;
        for (; n > 0; n--, p++) if ((*p & 0xC0) != 0x80) return false;
    }
    return true;
}

/* ---------- the conversation as quoted text (what the advisor is shown) ---------- */
static cJSON *tmsg(const char *role, const char *content) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role); cJSON_AddStringToObject(m, "content", content);
    return m;
}
static void test_transcript(void) {
    cJSON *msgs = cJSON_CreateArray();
    cJSON_AddItemToArray(msgs, tmsg("user", "old request"));
    cJSON_AddItemToArray(msgs, tmsg("assistant", "old answer"));
    cJSON_AddItemToArray(msgs, tmsg("user", "fix the parser"));                 /* [2] the request being worked on */
    cJSON *a = tmsg("assistant", "Let me look.");
    cJSON *calls = cJSON_AddArrayToObject(a, "tool_calls");
    cJSON *call = cJSON_CreateObject(); cJSON *fn = cJSON_AddObjectToObject(call, "function");
    cJSON_AddStringToObject(fn, "name", "read_file");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(fn, "arguments"), "path", "src/parser.py");
    cJSON_AddItemToArray(calls, call);
    cJSON_AddItemToArray(msgs, a);
    cJSON *t = tmsg("tool", "def parse(): pass\n"); cJSON_AddStringToObject(t, "tool_name", "read_file");
    cJSON_AddItemToArray(msgs, t);
    cJSON *ask = tmsg("assistant", "");                                          /* the turn that calls the advisor */
    calls = cJSON_AddArrayToObject(ask, "tool_calls");
    call = cJSON_CreateObject(); fn = cJSON_AddObjectToObject(call, "function");
    cJSON_AddStringToObject(fn, "name", "advisor");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(fn, "arguments"), "question", "SECRET_QUESTION");
    cJSON_AddItemToArray(calls, call);
    cJSON_AddItemToArray(msgs, ask);

    char *x = transcript_text(msgs, 2, 100000);
    CHECK(strstr(x, "[user]\nold request\n\n[agent]\nold answer\n\n[user]\nfix the parser\n\n") == x);
    CHECK(strstr(x, "[agent]\nLet me look.\n→ read_file({\"path\":\"src/parser.py\"})\n\n") != NULL);
    CHECK(strstr(x, "[result of read_file]\ndef parse(): pass\n\n") != NULL);
    CHECK(!strstr(x, "SECRET_QUESTION") && !strstr(x, "advisor"));   /* the question travels separately; an empty turn is no block */
    free(x);

    /* too small for everything: the newest blocks win, the request stays, and the gap is named */
    x = transcript_text(msgs, 2, 140);
    CHECK(strstr(x, "fix the parser") != NULL && strstr(x, "def parse()") != NULL);
    CHECK(!strstr(x, "old request") && !strstr(x, "old answer"));
    CHECK(strstr(x, "[… 2 earlier messages left out to fit …]\n\n[user]\nfix the parser") == x);
    free(x);
    x = transcript_text(msgs, -1, 110);   /* nothing pinned: just the tail */
    CHECK(!strstr(x, "fix the parser") && strstr(x, "def parse()") != NULL);
    free(x);

    /* a long result loses its middle, on UTF-8 boundaries, and says how much */
    sbuf big; sb_init(&big);
    sb_puts(&big, "HEAD-OF-FILE\n"); for (int i = 0; i < 2000; i++) sb_puts(&big, "æøå "); sb_puts(&big, "\nTAIL-OF-FILE");
    cJSON *bt = tmsg("tool", big.data); cJSON_AddStringToObject(bt, "tool_name", "bash");
    cJSON_AddItemToArray(msgs, bt);
    x = transcript_text(msgs, 2, 100000);
    CHECK(strstr(x, "HEAD-OF-FILE") && strstr(x, "TAIL-OF-FILE") && strstr(x, " bytes cut …]"));
    CHECK(strlen(x) < 5000);
    CHECK(utf8_valid(x));
    free(x); sb_free(&big);
    /* the cut points have to land inside a character some of the time: shift the text a byte at a time */
    for (int shift = 0; shift < 7; shift++) {
        sbuf t; sb_init(&t);
        for (int i = 0; i < shift; i++) sb_putc(&t, 'x');
        for (int i = 0; i < 800; i++) sb_puts(&t, "æøå€");   /* 2+2+2+3 bytes */
        sbuf o; sb_init(&o); sb_put_cut(&o, t.data, 1000);
        CHECK(utf8_valid(o.data) && o.len < 1100 && strstr(o.data, "bytes cut"));
        sb_free(&o); sb_free(&t);
    }

    cJSON *none = cJSON_CreateArray();
    x = transcript_text(none, -1, 1000); CHECK_STR(x, ""); free(x);
    cJSON_Delete(none); cJSON_Delete(msgs);
}

/* ---------- hosted APIs for the advisor: what goes out, what comes back ---------- */
static cJSON *jget(cJSON *o, const char *path) {   /* "a.b.0.c" */
    char buf[256]; snprintf(buf, sizeof buf, "%s", path);
    for (char *k = strtok(buf, "."); k && o; k = strtok(NULL, ".")) o = (*k >= '0' && *k <= '9') ? cJSON_GetArrayItem(o, atoi(k)) : cJSON_GetObjectItemCaseSensitive(o, k);
    return o;
}
static const char *jstr_at(cJSON *o, const char *path) { cJSON *v = jget(o, path); return cJSON_IsString(v) ? v->valuestring : NULL; }

/* a sub-agent's log: appended in order, and past the cap the front goes, whole lines only */
static void test_agent_log(void) {
    sbuf b; sb_init(&b);
    agent_log_add(&b, "one\n", 100); agent_log_add(&b, "two\n", 100);
    CHECK(!strcmp(b.data, "one\ntwo\n"));
    sb_free(&b); sb_init(&b);
    for (int i = 0; i < 50; i++) { char l[32]; snprintf(l, sizeof l, "line %02d\n", i); agent_log_add(&b, l, 120); }
    CHECK(b.len <= 120);
    CHECK(!strncmp(b.data, "… earlier lines dropped\n", 25));
    CHECK(strstr(b.data, "line 49\n") != NULL && strstr(b.data, "line 00") == NULL);
    CHECK(b.data[b.len - 1] == '\n');
    const char *p = strchr(b.data, '\n') + 1;                     /* after the note: a whole line starts */
    CHECK(!strncmp(p, "line ", 5));
    sb_free(&b);
    /* what a tool prints is plain text in the log: colour codes and carriage returns go */
    sb_init(&b);
    agent_log_add(&b, "\x1b[31mred\x1b[0m ok\r\n\tdone\x1b", 100);
    CHECK(!strcmp(b.data, "red ok\n\tdone"));
    sb_free(&b);
}

/* the running-tasks strip: how many rows fit, and one row fitted to the width */
static int u8cols(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++)
        if ((*p & 0xC0) != 0x80) w++;
    return w;
}
static int glyph_n(const char *s, const char *g) {
    int n = 0; size_t gl = strlen(g);
    for (const char *p = s; p && (p = strstr(p, g)); p += gl) n++;
    return n;
}
static bool ends_with(const char *s, const char *suf) {
    size_t n = s ? strlen(s) : 0, m = suf ? strlen(suf) : 0;
    return n >= m && !memcmp(s + n - m, suf, m);
}
static void lay_check(int room, int total, int rows, int shown, bool main_row, bool more) {
    int r = -1, s = -1; bool m = !main_row, o = !more;
    strip_layout(room, total, &r, &s, &m, &o);
    CHECK(r == rows && s == shown && m == main_row && o == more);
}
static void test_strip(void) {
    lay_check(10, 0, 0, 0, false, false);   /* nothing running */
    lay_check(10, 2, 3, 2, true, false);    /* ● main and both tasks */
    lay_check(2, 2, 2, 2, false, false);    /* the tasks fill the room: no main row */
    lay_check(4, 6, 4, 2, true, true);      /* main, two tasks, then "show all" */
    lay_check(1, 6, 1, 0, false, true);     /* only "show all" */
    lay_check(2, 6, 2, 1, false, true);     /* one task, then "show all" */
    lay_check(4, 3, 4, 3, true, false);     /* main and every task */
    lay_check(4, 4, 4, 4, false, false);    /* four tasks, no room for main */
    lay_check(0, 6, 0, 0, false, false);
    lay_check(-3, 6, 0, 0, false, false);
    lay_check(5, -1, 0, 0, false, false);
    strip_layout(4, 6, NULL, NULL, NULL, NULL);   /* the outs are optional */
    CHECK(true);

    char out[512];
    const char *right = "12s · ↓ 0";
    strip_row(out, sizeof out, 60, STRIP_ROW_TASK, "general", 16, "find the needle", 0, 0, right);
    CHECK(u8cols(out) == 60);
    CHECK(!strncmp(out, "  ◯ general", 11));
    CHECK(strstr(out, "find the needle") != NULL);
    CHECK(ends_with(out, right));
    CHECK(utf8_ok(out, strlen(out)));

    strip_row(out, sizeof out, 40, STRIP_ROW_TASK, "general", 16,
              "find the needle in the haystack", 0, 0, "9s · ↓ 1");
    CHECK(u8cols(out) == 40);
    CHECK(strstr(out, "…") != NULL);
    CHECK(ends_with(out, "9s · ↓ 1"));

    strip_row(out, sizeof out, 80, STRIP_ROW_TASK, "lean-tracks-economy", 19, NULL, 9, 10, "1s · ↓ 1");
    CHECK(u8cols(out) == 62 && u8cols(out) < 80);   /* the bar stays with the fraction; the row is not stretched */
    CHECK(glyph_n(out, "▰") == 18 && glyph_n(out, "▱") == 2);
    CHECK(strstr(out, "9/10") != NULL);

    strip_row(out, sizeof out, 80, STRIP_ROW_TASK, "lean-tracks-economy", 19, NULL, 10, 10, "1s · ↓ 1");
    CHECK(glyph_n(out, "▰") == 20 && glyph_n(out, "▱") == 0);
    strip_row(out, sizeof out, 80, STRIP_ROW_TASK, "lean-tracks-economy", 19, NULL, 0, 10, "1s · ↓ 1");
    CHECK(glyph_n(out, "▰") == 0 && glyph_n(out, "▱") == 20);
    strip_row(out, sizeof out, 80, STRIP_ROW_TASK, "lean-tracks-economy", 19, NULL, 26, 29, "1s · ↓ 1");
    CHECK(glyph_n(out, "▰") == 17 && glyph_n(out, "▱") == 3);
    strip_row(out, sizeof out, 80, STRIP_ROW_MAIN, "main", 4, NULL, 4, 10, NULL);
    CHECK(glyph_n(out, "▰") == 8 && glyph_n(out, "▱") == 12 && strstr(out, "4/10") != NULL);
    strip_row(out, sizeof out, 80, STRIP_ROW_MAIN, "main", 4, NULL, 1, 100, NULL);
    CHECK(glyph_n(out, "▰") == 1);   /* a start that would round to an empty bar still fills one cell */

    strip_row(out, sizeof out, 12, STRIP_ROW_MAIN, "main", 4, NULL, 4, 10, NULL);
    CHECK(glyph_n(out, "▰") == 0 && glyph_n(out, "▱") == 0);   /* no room for four cells: the bar is dropped */
    CHECK(strstr(out, "4/10") != NULL && u8cols(out) <= 12);

    strip_row(out, sizeof out, 40, STRIP_ROW_ALL, "show all · 8 running", 0, NULL, 0, 0, NULL);
    CHECK_STR(out, "  show all · 8 running");
    CHECK(u8cols(out) <= 40);

    strip_row(out, sizeof out, 40, STRIP_ROW_MAIN, "main", 10, NULL, 0, 0, NULL);
    CHECK_STR(out, "● main");

    strip_row(out, sizeof out, 5, STRIP_ROW_TASK, "lean-tracks-economy", 19,
              "doing a long thing", 0, 0, "9s · ↓ 1");
    CHECK(u8cols(out) <= 5 && utf8_ok(out, strlen(out)));

    strip_row(out, sizeof out, 40, STRIP_ROW_TASK, NULL, 0, NULL, 0, 0, NULL);
    CHECK(u8cols(out) <= 40 && utf8_ok(out, strlen(out)));
    strip_row(NULL, 0, 80, STRIP_ROW_TASK, "a", 4, "b", 1, 2, "c");
    char tiny[1] = { 'x' };
    strip_row(tiny, 1, 80, STRIP_ROW_MAIN, "main", 4, "hi", 1, 2, "x");
    CHECK(tiny[0] == 0);
}

/* a pasted block in the prompt is a token; the text goes back in on submit, whole tokens only */
static void test_paste(void) {
    const char *in = "look [Pasted text #1 +3 lines] and [Pasted text #2 +2 lines]!";
    size_t sp[4][2]; int ks[4], ns[4];
    size_t n = paste_tokens(in, strlen(in), sp, ks, ns, 4);
    CHECK(n == 2);
    CHECK(n == 2 && sp[0][0] == 5 && sp[0][1] == 30 && ks[0] == 1 && ns[0] == 3);
    CHECK(n == 2 && ks[1] == 2 && ns[1] == 2);
    CHECK(paste_tokens("[Pasted text #1 +3 lines", 24, NULL, NULL, NULL, 4) == 0);   /* not finished */
    CHECK(paste_tokens("[Pasted text #0 +3 lines]", 25, NULL, NULL, NULL, 4) == 0);  /* no paste 0 */
    const char *texts[2] = { "a\nb\nc", "d\ne" }; int lines[2] = { 3, 2 };
    char *o = paste_expand(in, strlen(in), texts, lines, 2);
    CHECK(!strcmp(o, "look a\nb\nc and d\ne!"));
    free(o);
    o = paste_expand("[Pasted text #1 +4 lines] x", 27, texts, lines, 2);   /* a token whose count was edited: literal */
    CHECK(!strcmp(o, "[Pasted text #1 +4 lines] x"));
    free(o);
    o = paste_expand("[Pasted text #9 +3 lines]", 25, texts, lines, 2);      /* a number with no paste: literal */
    CHECK(!strcmp(o, "[Pasted text #9 +3 lines]"));
    free(o);
    const char *ten = "[Pasted text #1 +3 lines]";                       /* a token that ends the input */
    o = paste_expand(ten, strlen(ten), texts, lines, 2);
    CHECK(!strcmp(o, "a\nb\nc"));
    free(o);
    CHECK(paste_tokens(ten, strlen(ten), NULL, NULL, NULL, 4) == 1);
    o = paste_expand("", 0, texts, lines, 2);
    CHECK(o && !*o);
    free(o);
}

/* pasted text: what makes a block, how a bracketed paste becomes text, and the token edge cases */
static void test_paste_more(void) {
    int lines = -1;
    /* a block is more than 3 lines or more than 800 bytes; each newline counts, and a last line without one too */
    CHECK(!paste_is_block("a\nb\nc", 5, &lines) && lines == 3);
    CHECK(!paste_is_block("a\nb\nc\n", 6, &lines) && lines == 3);
    CHECK(paste_is_block("a\nb\nc\nd", 7, &lines) && lines == 4);
    CHECK(paste_is_block("a\nb\nc\nd\n", 8, &lines) && lines == 4);
    CHECK(!paste_is_block("", 0, &lines) && lines == 0);
    CHECK(!paste_is_block("x", 1, NULL));                                  /* lines may be NULL */
    char *b801 = malloc(801); memset(b801, 'x', 801);
    CHECK(!paste_is_block(b801, 800, &lines) && lines == 1);             /* 800 bytes, one line: typed in */
    CHECK(paste_is_block(b801, 801, &lines) && lines == 1);              /* 801 bytes: a block */
    free(b801);
    /* the limit is in bytes: 266 three-byte characters are 798 bytes (typed in), 267 are 801 (a block) */
    { size_t n = 267 * 3; char *u = malloc(n);
      for (size_t i = 0; i < 267; i++) memcpy(u + 3 * i, "\xe6\x97\xa5", 3);   /* 日 */
      CHECK(paste_is_block(u, n, NULL));
      CHECK(!paste_is_block(u, 266 * 3, NULL));
      free(u); }

    /* a bracketed paste as text: CRLF and CR are line breaks; tab stays; controls, DEL and NUL go */
    size_t n = 0; char *t;
    t = paste_clean("a\r\nb", 4, &n); CHECK(!strcmp(t, "a\nb") && n == 3); free(t);
    t = paste_clean("a\rb", 3, &n); CHECK(!strcmp(t, "a\nb") && n == 3); free(t);
    t = paste_clean("a\r\r\nb", 5, &n); CHECK(!strcmp(t, "a\n\nb") && n == 4); free(t);   /* CR, then CRLF */
    t = paste_clean("a\r", 2, &n); CHECK(!strcmp(t, "a\n") && n == 2); free(t);           /* a trailing CR */
    t = paste_clean("a\tb", 3, &n); CHECK(!strcmp(t, "a\tb") && n == 3); free(t);         /* tab stays */
    t = paste_clean("a\x1b[Db", 5, &n); CHECK(!strcmp(t, "a[Db") && n == 4); free(t);     /* ESC goes, the rest is text */
    t = paste_clean("a\x7f" "b", 3, &n); CHECK(!strcmp(t, "ab") && n == 2); free(t);      /* DEL is not a backspace in text */
    t = paste_clean("a\0b", 3, &n); CHECK(!strcmp(t, "ab") && n == 2); free(t);           /* NUL goes */
    t = paste_clean("a\x07\x08\x0b" "b", 5, &n); CHECK(!strcmp(t, "ab") && n == 2); free(t);   /* BEL, BS, VT */
    t = paste_clean("", 0, &n); CHECK(t && !*t && n == 0); free(t);
    t = paste_clean("\n\n", 2, &n); CHECK(!strcmp(t, "\n\n") && n == 2); free(t);         /* newlines stay */
    t = paste_clean("日本 🙂 é", strlen("日本 🙂 é"), &n); CHECK(!strcmp(t, "日本 🙂 é")); free(t);   /* UTF-8 is text */
    t = paste_clean("x", 1, NULL); CHECK(!strcmp(t, "x")); free(t);                      /* out_len may be NULL */
    t = paste_clean("a\xff" "b", 3, &n); CHECK(!strcmp(t, "a\xef\xbf\xbd" "b") && n == 5); free(t);   /* a bad byte: U+FFFD */
    t = paste_clean("a\xe5" "x", 3, &n); CHECK(!strcmp(t, "a\xef\xbf\xbdx") && n == 5); free(t);   /* a Latin-1 å alone */
    t = paste_clean("\xe6\x97" "x", 3, &n); CHECK(!strcmp(t, "\xef\xbf\xbd\xef\xbf\xbd" "x") && n == 7); free(t);   /* a cut character: one U+FFFD per byte */
    { size_t big = 1u << 20; char *in = malloc(big); memset(in, 'q', big);
      t = paste_clean(in, big, &n); CHECK(n == big && t[big - 1] == 'q'); free(t); free(in); }

    /* the tokens, at their edges (every literal is measured with strlen, so no length can be miscounted) */
    const char *texts[2] = { "a\nb\nc", "d\ne" }; const int tl[2] = { 3, 2 };
    const char *s1;
    char *o;
    s1 = "[Pasted text #99999999999 +3 lines]";                       /* K with too many digits: literal */
    o = paste_expand(s1, strlen(s1), texts, tl, 2); CHECK(!strcmp(o, s1)); free(o);
    s1 = "[Pasted text #1 +99999999999 lines]";                       /* N with too many digits: literal */
    o = paste_expand(s1, strlen(s1), texts, tl, 2); CHECK(!strcmp(o, s1)); free(o);
    s1 = "[Pasted text #1 +3 lines]";                                 /* no pastes at all: literal */
    o = paste_expand(s1, strlen(s1), texts, tl, 0); CHECK(!strcmp(o, s1)); free(o);
    s1 = "[Pasted text #1 +3 lines][Pasted text #2 +2 lines]";       /* two in a row */
    o = paste_expand(s1, strlen(s1), texts, tl, 2); CHECK(!strcmp(o, "a\nb\nc" "d\ne")); free(o);
    s1 = "[Pasted text #1 +3 lines] [Pasted text #1 +3 lines]";      /* the same K twice */
    o = paste_expand(s1, strlen(s1), texts, tl, 2); CHECK(!strcmp(o, "a\nb\nc a\nb\nc")); free(o);
    s1 = "日[Pasted text #1 +3 lines]🙂";                              /* UTF-8 around a token */
    o = paste_expand(s1, strlen(s1), texts, tl, 2); CHECK(!strcmp(o, "日a\nb\nc🙂")); free(o);
    /* text that looks like a token is pasted text: it is not expanded again */
    const char *tricky[1] = { "see [Pasted text #1 +3 lines] here" }; const int t1[1] = { 3 };
    s1 = "[Pasted text #1 +3 lines]";
    o = paste_expand(s1, strlen(s1), tricky, t1, 1);
    CHECK(!strcmp(o, "see [Pasted text #1 +3 lines] here")); free(o);
    /* a long paste expands to its full length */
    { size_t L = 200000; char *big = malloc(L + 1); memset(big, 'z', L); big[L] = 0;
      const char *bt[1] = { big }; const int bl[1] = { 100000 };
      s1 = "[Pasted text #1 +100000 lines]";
      o = paste_expand(s1, strlen(s1), bt, bl, 1);
      CHECK(strlen(o) == L); free(o); free(big); }
    /* twenty tokens: the scan stops at the array's size */
    { char many[32 * 30]; size_t m = 0;
      for (int i = 1; i <= 20; i++) m += (size_t)sprintf(many + m, "[Pasted text #%d +4 lines] ", i);
      size_t sp[16][2]; int ks[16], ns[16];
      CHECK(paste_tokens(many, m, sp, ks, ns, 16) == 16);
      CHECK(paste_tokens(many, m, NULL, NULL, NULL, 40) == 20); }
    /* the notcurses field's paste store (term.c, shared with the ANSI editor): a block is a token, the sent text has
     * its tokens put back, and the prompt's pastes are forgotten at the submit */
    char tok[64];
    CHECK(term_paste_token("a\nb\nc\nd\ne", 9, tok, sizeof tok) && !strcmp(tok, "[Pasted text #1 +5 lines]"));
    CHECK(!term_paste_token("a\nb\nc", 5, tok, sizeof tok));              /* three lines: typed in, no token */
    char *sent = term_paste_expand("x [Pasted text #1 +5 lines] y", strlen("x [Pasted text #1 +5 lines] y"));
    CHECK(sent && !strcmp(sent, "x a\nb\nc\nd\ne y")); free(sent);
    sent = term_paste_expand("[Pasted text #1 +5 lines]", strlen("[Pasted text #1 +5 lines]"));   /* forgotten: literal now */
    CHECK(sent && !strcmp(sent, "[Pasted text #1 +5 lines]")); free(sent);
    CHECK(term_paste_token("t\tu\n1\n2\n3", strlen("t\tu\n1\n2\n3"), tok, sizeof tok) && !strcmp(tok, "[Pasted text #1 +4 lines]"));
    { int tl = -1; const char *pt = term_paste_text(1, &tl);   /* the paste's text and line count, for Ctrl-O and the view */
      CHECK(pt && !strcmp(pt, "t\tu\n1\n2\n3") && tl == 4);
      CHECK(term_paste_text(2, NULL) == NULL && term_paste_text(0, NULL) == NULL); }
    term_paste_reset();
    sent = term_paste_expand("[Pasted text #1 +4 lines]", 25); CHECK(sent && !strcmp(sent, "[Pasted text #1 +4 lines]")); free(sent);
    /* the rows the paste viewer shows (both screens): cut at the width, one column per code point, empty lines kept */
    { size_t n = 0; size_t (*r)[2];
      r = paste_rows("abcdef", 4, &n); CHECK(n == 2 && r[0][0] == 0 && r[0][1] == 4 && r[1][0] == 4 && r[1][1] == 2); free(r);
      r = paste_rows("ab\n\ncd", 10, &n); CHECK(n == 3 && r[1][1] == 0 && r[2][0] == 4 && r[2][1] == 2); free(r);   /* an empty line is a row */
      r = paste_rows("ab\n", 10, &n); CHECK(n == 2 && r[1][1] == 0); free(r);                                    /* a trailing newline: an empty last row */
      r = paste_rows("", 10, &n); CHECK(n == 1 && r[0][1] == 0); free(r);
      r = paste_rows("日本語", 2, &n); CHECK(n == 2 && r[0][1] == 6 && r[1][1] == 3); free(r);                    /* two code points a row, the bytes kept whole */
      r = paste_rows("🙂🙂🙂", 1, &n); CHECK(n == 3 && r[1][0] == 4 && r[2][1] == 4); free(r);                    /* a 4-byte character is one column */
      r = paste_rows("abc", 0, &n); CHECK(n == 3); free(r);                                                       /* width below 1 is 1 */
      char *big = malloc(5001); memset(big, 'x', 5000); big[5000] = 0;
      r = paste_rows(big, 80, &n); CHECK(n == 63 && r[62][0] + r[62][1] == 5000); free(r); free(big); }
    /* a token that is the whole input, and one with text after it */
    s1 = "[Pasted text #1 +3 lines]";
    CHECK(paste_tokens(s1, strlen(s1), NULL, NULL, NULL, 4) == 1);
    s1 = "[Pasted text #1 +3 lines]x";
    CHECK(paste_tokens(s1, strlen(s1), NULL, NULL, NULL, 4) == 1);
}

static void test_goal(void) {
    char err[160], why[200];
    goal_spec g;
    CHECK(goal_parse("\"./bench.sh\" ", &g, err, sizeof err) == false);   /* no goal after the metric */
    CHECK(strstr(err, "no goal") != NULL);
    CHECK(goal_parse("\"./bench.sh --fast\" min -n 2 -k 4 make it run in 10 ms", &g, err, sizeof err));
    CHECK_STR(g.metric, "./bench.sh --fast"); CHECK(g.lower && g.rounds == 2 && g.cands == 4); CHECK_STR(g.task, "make it run in 10 ms");
    goal_spec_free(&g);
    CHECK(goal_parse("\"cat score\" -k 3 -n 5 max faster", &g, err, sizeof err));   /* options in any order, after the metric */
    CHECK_STR(g.metric, "cat score"); CHECK(!g.lower && g.rounds == 5 && g.cands == 3); CHECK_STR(g.task, "faster");
    goal_spec_free(&g);
    CHECK(goal_parse("'wc -l x' do it", &g, err, sizeof err));   /* defaults: 3 rounds, 3 candidates, higher is better */
    CHECK(g.rounds == 3 && g.cands == ORCH_WORKERS_DEFAULT && !g.lower); CHECK_STR(g.task, "do it");
    goal_spec_free(&g);
    CHECK(!goal_parse("\"unclosed", &g, err, sizeof err) && strstr(err, "closing"));
    CHECK(!goal_parse("\"cat score\" -n 0 x", &g, err, sizeof err) && strstr(err, "-n"));
    CHECK(!goal_parse("\"cat score\" -n 11 x", &g, err, sizeof err) && strstr(err, "-n"));
    CHECK(!goal_parse("\"cat score\" -k 1 x", &g, err, sizeof err) && strstr(err, "-k"));
    CHECK(!goal_parse("\"cat score\" -k many x", &g, err, sizeof err) && strstr(err, "number"));
    CHECK(!goal_parse("   ", &g, err, sizeof err) && strstr(err, "metric"));
    CHECK(goal_parse("\"make bench\" -m xai:grok-4.7,deepseek-chat,claude-opus-5 -k 3 faster", &g, err, sizeof err));
    CHECK(g.nmodels == 3 && !strcmp(g.models[0], "xai:grok-4.7") && !strcmp(g.models[1], "deepseek-chat") && !strcmp(g.models[2], "claude-opus-5") && g.cands == 3);
    CHECK_STR(g.task, "faster");
    goal_spec_free(&g);
    CHECK(!goal_parse("\"x\" -m a,,b go", &g, err, sizeof err) && strstr(err, "empty model"));
    CHECK(!goal_parse("\"x\" -m go", &g, err, sizeof err) && strstr(err, "no goal"));   /* -m takes the next word as its list: no goal is left */
    CHECK(!goal_parse("\"x\" -m", &g, err, sizeof err) && strstr(err, "-m needs"));
    CHECK(!goal_parse("\"cat score\"   \n", &g, err, sizeof err) && strstr(err, "no goal"));
    double v;
    CHECK(goal_number("warming up\n1.5 s\n  42.25  \n\n", &v) && v == 42.25);
    CHECK(goal_number("12", &v) && v == 12);
    CHECK(goal_number("-3e2\r\n", &v) && v == -300);
    CHECK(!goal_number("42 ms", &v));          /* a unit after the number is not a score */
    CHECK(!goal_number("42\nno number here", &v));
    CHECK(!goal_number("nan", &v) && !goal_number("inf", &v) && !goal_number("", &v) && !goal_number(NULL, &v));
    CHECK(goal_better(2.0, 1.0, false) && !goal_better(1.0, 1.0, false) && goal_better(0.5, 1.0, true) && !goal_better(2.0, 1.0, true));
    CHECK(goal_verdict("I made it faster.\n\nGOAL: REACHED\n", why, sizeof why) == GOAL_REACHED);
    CHECK(goal_verdict("**GOAL: IMPOSSIBLE** faster than light is not a thing.", why, sizeof why) == GOAL_IMPOSSIBLE);
    CHECK_STR(why, "faster than light is not a thing.");
    CHECK(goal_verdict("GOAL: IMPOSSIBLE: the disk is full\nGOAL: REACHED", why, sizeof why) == GOAL_REACHED && why[0] == 0);   /* the last line wins */
    CHECK(goal_verdict("the goal is not reached yet, GOAL: REACHED mentioned mid-line", why, sizeof why) == GOAL_NONE);
    CHECK(goal_verdict("nothing about it", why, sizeof why) == GOAL_NONE && goal_verdict(NULL, NULL, 0) == GOAL_NONE);
}

static void test_orchestrate(void) {
    orch_task *t = NULL;
    int n = orch_parse_plan("Here is the plan.\n\nTASK 1: add the flag\nEdit src/a.c: add --dry.\nDONE WHEN: make passes\n\n"
                            "## **Task 2:** document it\nTask 2 depends on the flag being there.\n- README.md\n\ntask 7 - run the tests\nmake test\n", &t);
    CHECK(n == 3);
    CHECK_STR(t[0].title, "add the flag"); CHECK_STR(t[0].body, "Edit src/a.c: add --dry.\nDONE WHEN: make passes");
    CHECK_STR(t[1].title, "document it"); CHECK(strstr(t[1].body, "Task 2 depends on the flag") && strstr(t[1].body, "- README.md"));   /* a sentence is not a header, markdown is no obstacle */
    CHECK_STR(t[2].title, "run the tests"); CHECK_STR(t[2].body, "make test");                                                       /* numbered as they come */
    CHECK(t[0].after == 0 && t[1].after == 1u && t[2].after == 2u);   /* a plan that says nothing runs in order */
    orch_tasks_free(t, n);
    n = orch_parse_plan("TASK 1: a\nAFTER: none\ndo a\n\nTASK 2: b\n**AFTER:** none\ndo b\n\nTASK 3: c\nAfter: tasks 1 and 2\ndo c\n\nTASK 4: d\ndo d after lunch\n\nTASK 5: e\nAFTER: 5, 9, 1\ndo e\n", &t);
    CHECK(n == 5 && t[0].after == 0 && t[1].after == 0 && t[2].after == 3u && t[3].after == 4u);   /* none, none, both, and — unsaid — the one before */
    CHECK(t[4].after == 1u);                                             /* itself and a task that is not there do not count */
    CHECK_STR(t[2].body, "do c"); CHECK_STR(t[3].body, "do d after lunch");   /* the line is no part of the instructions; a sentence stays */
    orch_tasks_free(t, n);
    /* LAYER: n (/plan): a task with none takes the layer before it, the first is in layer 1, and a layer never goes back */
    n = orch_parse_plan("TASK 1: pom\nLAYER: 1\ndo pom\n\nTASK 2: entity\n**Layer:** 2\ndo entity\n\nTASK 3: repo\ndo repo\n\n"
                        "TASK 4: service\nLAYER: 3\ndo service\n\nTASK 5: back\nLAYER: 2\ndo back\n\nTASK 6: x\nLAYER: 4\nAFTER: none\ndo x\n", &t);
    CHECK(n == 6 && t[0].layer == 1 && t[1].layer == 2 && t[2].layer == 2 && t[3].layer == 3 && t[4].layer == 3 && t[5].layer == 4);
    CHECK_STR(t[2].body, "do repo"); CHECK(t[5].after == 0);   /* the layer line is no part of the instructions; AFTER still reads */
    orch_tasks_free(t, n);
    n = orch_parse_plan("TASK 1: a\ndo a\n\nTASK 2: b\ndo b\n", &t);
    CHECK(n == 2 && t[0].layer == 1 && t[1].layer == 1);   /* a plan with no layers is one layer */
    orch_tasks_free(t, n);
    n = orch_parse_plan("TASK 1: a\nLAYER: 2\ndo a\n\nTASK 2: b\nLAYER: x\ndo b\n", &t);
    CHECK(n == 2 && t[0].layer == 2 && t[1].layer == 2);   /* a layer that is no number is not one: the layer before it stands */
    orch_tasks_free(t, n);
    n = orch_parse_plan("TASK 1\nJust the instructions.\n\nTASK 2: —\n", &t);
    CHECK(n == 2); CHECK_STR(t[0].title, ""); CHECK_STR(t[0].body, "Just the instructions.");
    orch_tasks_free(t, n);
    /* ROLE: and OWNS: belong to their task, not its instructions; CONTRACT: is the plan's, read before the first task */
    char *contract = NULL;
    n = orch_parse_plan_ex("Plan.\nCONTRACT: package com.x; entity Hours(id, project, hours)\n- Repo: HoursRepo with findByWeek\n\n"
                           "TASK 1: entities\nROLE: entity\nOWNS: src/main/java/com/x/model/, src/test/java/com/x/model\ndo the entities\n\n"
                           "TASK 2: service\nROLE: service\nOWNS: `src/main/java/com/x/service/HoursService.java`\nAFTER: 1\ndo the service\n\n"
                           "TASK 3: plain\nROLE:\ndo it\n", &t, &contract);
    CHECK(n == 3 && contract && strstr(contract, "entity Hours") && strstr(contract, "Repo: HoursRepo") && !strstr(contract, "TASK 1"));
    CHECK_STR(t[0].role, "entity"); CHECK(t[0].nowns == 2 && !strcmp(t[0].owns[0], "src/main/java/com/x/model/") && !strcmp(t[0].owns[1], "src/test/java/com/x/model"));
    CHECK_STR(t[1].role, "service"); CHECK(t[1].nowns == 1 && !strcmp(t[1].owns[0], "src/main/java/com/x/service/HoursService.java"));
    CHECK(t[1].after == 1u && t[2].after == 2u && t[2].role == NULL && t[2].nowns == 0);   /* an empty ROLE: is no role */
    CHECK_STR(t[1].body, "do the service");
    free(contract); orch_tasks_free(t, n);
    n = orch_parse_plan_ex("TASK 1: a\ndo a\n", &t, &contract);
    CHECK(n == 1 && contract == NULL && t[0].role == NULL && t[0].owns == NULL);   /* a plan without the new lines parses as before */
    orch_tasks_free(t, n);
    /* OWNS: paths clash when they name one file, or a directory and what is in it; a prefix that is no directory does not */
    CHECK(!owns_overlap_one("src/a", "src/ab") && !owns_overlap_one("pom.xml", "src/pom.xml"));
    CHECK(owns_overlap_one("src/a", "src/a") && owns_overlap_one("src/main/java/x/", "src/main/java/x/service/S.java"));
    CHECK(owns_overlap_one("./src/", "src/x") && owns_overlap_one("pom.xml/", "pom.xml"));
    CHECK(owns_overlap_one(".", "anything/at/all") && owns_overlap_one("", "x"));   /* the whole project clashes with all */
    char *la[] = { "x/a.java", "pom.xml" }, *lb[] = { "pom.xml" }, *lc[] = { "y/" };
    CHECK(owns_overlap(la, 2, lb, 1) && !owns_overlap(la, 2, lc, 1) && !owns_overlap(la, 2, NULL, 0));
    CHECK(orch_parse_plan("I would rather talk first.\nThe task is unclear.", &t) == 0 && t == NULL);
    CHECK(orch_parse_plan("", &t) == 0 && orch_parse_plan(NULL, &t) == 0);
    sbuf many; sb_init(&many);
    for (int i = 1; i <= ORCH_MAX_TASKS + 3; i++) sb_printf(&many, "TASK %d: step %d\ndo %d\n", i, i, i);
    n = orch_parse_plan(many.data, &t);
    char last[32]; snprintf(last, sizeof last, "do %d", ORCH_MAX_TASKS);
    CHECK(n == ORCH_MAX_TASKS); CHECK_STR(t[n - 1].body, last);   /* no more than that, and the last one kept whole */
    orch_tasks_free(t, n); sb_free(&many);
}

/* base64 (RFC 4648 test vectors, and bytes that are not text) */
static void test_base64(void) {
    char *a = base64_encode("", 0);   CHECK_STR(a, ""); free(a);
    a = base64_encode("f", 1);        CHECK_STR(a, "Zg=="); free(a);
    a = base64_encode("fo", 2);       CHECK_STR(a, "Zm8="); free(a);
    a = base64_encode("foo", 3);      CHECK_STR(a, "Zm9v"); free(a);
    a = base64_encode("foobar", 6);   CHECK_STR(a, "Zm9vYmFy"); free(a);
    a = base64_encode("\0\xff\xfe", 3); CHECK_STR(a, "AP/+"); free(a);   /* a NUL and high bytes survive */
    a = base64_encode("a\0", 2);     CHECK_STR(a, "YQA="); free(a);
}

#ifdef WITH_ZLIB
#include <zlib.h>
/* the gzip of a body is a gzip stream (RFC 1952: the magic, deflate) that inflates back to the same bytes */
static void test_gzip_round_trip(void) {
    const char *plain = "{\"model\":\"x\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    size_t n = strlen(plain), zn = 0;
    char *z = gzip_body(plain, n, &zn);
    CHECK(z && zn > 10 && (unsigned char)z[0] == 0x1f && (unsigned char)z[1] == 0x8b && z[2] == 8);
    /* a body that is all NULs and high bytes: the length is what travels, not a string */
    char raw[300]; for (int i = 0; i < 300; i++) raw[i] = (char)(i % 7 == 0 ? 0 : (i * 37) & 0xff);
    size_t zr = 0; char *zrz = gzip_body(raw, sizeof raw, &zr);
    CHECK(zrz && zr > 0);
    z_stream d; memset(&d, 0, sizeof d);
    CHECK(inflateInit2(&d, 15 + 16) == Z_OK);
    unsigned char back[400]; d.next_in = (Bytef *)zrz; d.avail_in = (uInt)zr; d.next_out = back; d.avail_out = sizeof back;
    CHECK(inflate(&d, Z_FINISH) == Z_STREAM_END && d.total_out == sizeof raw && !memcmp(back, raw, sizeof raw));
    inflateEnd(&d);
    free(zrz);
    if (z) {
        z_stream e; memset(&e, 0, sizeof e);
        CHECK(inflateInit2(&e, 15 + 16) == Z_OK);
        unsigned char out[200]; e.next_in = (Bytef *)z; e.avail_in = (uInt)zn; e.next_out = out; e.avail_out = sizeof out;
        CHECK(inflate(&e, Z_FINISH) == Z_STREAM_END && e.total_out == n && !memcmp(out, plain, n));
        inflateEnd(&e);
    }
    free(z);
    CHECK(gzip_available());
}
#else
static void test_gzip_round_trip(void) { CHECK(!gzip_available()); CHECK(gzip_body("x", 1, &(size_t){0}) == NULL); }
#endif

/* which answers mean the server cannot take a gzipped body, and what the probe concludes from two answers */
static void test_gzip_judge(void) {
    CHECK(gzip_refused(415, NULL));
    CHECK(gzip_refused(400, "{\"error\":{\"message\":\"Unsupported Content-Encoding: gzip\"}}"));
    CHECK(gzip_refused(400, "The request body is not valid JSON"));
    CHECK(!gzip_refused(400, "model: claude-x was not found"));          /* a plain 400: not about the encoding */
    CHECK(!gzip_refused(400, "max_tokens is too large"));
    CHECK(!gzip_refused(500, "Content-Encoding: gzip"));                  /* a server error is not a refusal */
    CHECK(!gzip_refused(200, NULL) && !gzip_refused(401, "invalid api key"));
    CHECK(gzip_probe_verdict(200, 200, "{}") == 1);                        /* both fine: gzip works */
    CHECK(gzip_probe_verdict(200, 415, "") == 0);                          /* refused: off */
    CHECK(gzip_probe_verdict(200, 400, "unsupported encoding") == 0);
    CHECK(gzip_probe_verdict(200, 500, "") == -1);                         /* the gzipped call failed another way: no conclusion */
    CHECK(gzip_probe_verdict(401, 200, "") == -1);                         /* the plain call failed: nothing to compare with */
    CHECK(gzip_probe_verdict(401, 415, "") == -1);
}

/* the saved setting: round trip through the config's own lines, and a re-set replaces the entry */
static void test_gzip_setting(void) {
    bool on = true;
    CHECK(!gzip_get("zz-unset", &on));
    gzip_set("zz-host", true);
    CHECK(gzip_get("zz-host", &on) && on);
    gzip_set("zz-host", false);
    CHECK(gzip_get("zz-host", &on) && !on);
    CHECK(g_cfg.n_gzips >= 1);
    int count = 0; for (int i = 0; i < g_cfg.n_gzips; i++) if (!strcmp(g_cfg.gzips[i].name, "zz-host")) count++;
    CHECK(count == 1);   /* one entry per provider */
}

/* the hosts a user adds (/model add): the rules, the lookup by prefix, replacing and removing */
static void test_hosts(void) {
    char err[200] = "";
    int nb = 0; while (provider_at(nb) && !provider_at(nb)->host) nb++;   /* the built-in table, before any host */
    CHECK(nb == 5);
    CHECK(provider_name_ok("openrouter", err, sizeof err) && provider_name_ok("my-host2", err, sizeof err));
    CHECK(!provider_name_ok("a", err, sizeof err) && !provider_name_ok("Upper", err, sizeof err) && !provider_name_ok("1abc", err, sizeof err) && !provider_name_ok("my_host", err, sizeof err));
    CHECK(!provider_name_ok("xai", err, sizeof err) && !provider_name_ok("grok", err, sizeof err) && !provider_name_ok("claude", err, sizeof err) && !provider_name_ok("moonshot", err, sizeof err));
    CHECK(!provider_name_ok("abcdefghijklmnopqrstuvwxy", err, sizeof err));   /* 25 characters */
    CHECK(!provider_name_ok("grok", err, sizeof err) && strstr(err, "built-in provider's name"));
    CHECK(provider_host_url_ok("https://openrouter.ai/api/v1", err, sizeof err) && provider_host_url_ok("http://127.0.0.1:8080/v1", err, sizeof err) && provider_host_url_ok("http://localhost:1234/v1", err, sizeof err));
    CHECK(!provider_host_url_ok("http://example.com/v1", err, sizeof err) && strstr(err, "plain http"));
    CHECK(!provider_host_url_ok("example.com/v1", err, sizeof err) && !provider_host_url_ok("https://", err, sizeof err) && !provider_host_url_ok("https://a b.com", err, sizeof err) && !provider_host_url_ok("", err, sizeof err));

    int before = provider_host_count();
    CHECK(provider_host_add("openrouter", "https://openrouter.ai/api/v1", NULL, err, sizeof err) == 0);
    const char *model = NULL;
    const provider_def *p = provider_find("openrouter:openai/gpt-5.2", &model);
    CHECK(p && p->host && p->style == PROVIDER_CHAT_COMPLETIONS && !p->example && !p->known_as && p->label);
    CHECK_STR(model, "openai/gpt-5.2");
    CHECK_STR(p->key_env, "OPENROUTER_API_KEY");
    CHECK_STR(provider_base_url(p), "https://openrouter.ai/api/v1");
    CHECK(provider_host_count() == before + 1 && provider_at(nb + before) && !strcmp(provider_at(nb + before)->name, "openrouter"));
    p = provider_find("OpenRouter:x", &model); CHECK(p && p->host);   /* the prefix is matched as the built-ins' are */
    p = provider_find("openrouter:meta-llama/llama-3.3-70b-instruct:free", &model);
    CHECK_STR(model, "meta-llama/llama-3.3-70b-instruct:free");   /* split at the first colon */
    CHECK(provider_host_add("my-host", "http://127.0.0.1:9/v1", NULL, err, sizeof err) == 0);
    p = provider_find("my-host:x", NULL); CHECK(p && !strcmp(p->key_env, "MY_HOST_API_KEY"));   /* NAME_API_KEY */
    CHECK(provider_host_add("bad-key", "https://x.example/v1", "1BAD", err, sizeof err) == -1 && !provider_find("bad-key:x", NULL));
    CHECK(provider_host_add("xai", "https://x.example/v1", NULL, err, sizeof err) == -1);   /* a built-in's name */
    CHECK(provider_host_add("plain", "http://example.com/v1", NULL, err, sizeof err) == -1 && strstr(err, "plain http"));
    CHECK(provider_host_add("openrouter", "https://proxy.example/v1", "OR_KEY", err, sizeof err) == 0);   /* the same name replaces */
    CHECK_STR(provider_base_url(provider_find("openrouter:x", NULL)), "https://proxy.example/v1");
    CHECK_STR(provider_find("openrouter:x", NULL)->key_env, "OR_KEY");
    CHECK(provider_host_count() == before + 2);
    CHECK(provider_find("xai:grok-4.7", NULL) != NULL && provider_find("xai:grok-4.7", NULL)->host == false);
    CHECK(provider_host_remove("my-host") && !provider_find("my-host:x", NULL));
    CHECK(!provider_host_remove("my-host"));
    CHECK(provider_host_remove("openrouter"));
    CHECK(provider_host_count() == before && provider_at(nb + before) == NULL && !provider_find("openrouter:x", NULL));
}

static void test_provider(void) {
    const char *model = NULL;
    const provider_def *p = provider_find("xai:grok-4.7", &model);
    CHECK(p && !strcmp(p->name, "xai") && p->style == PROVIDER_CHAT_COMPLETIONS); CHECK_STR(model, "grok-4.7");
    p = provider_find("grok:grok-4.7", &model); CHECK(p && !strcmp(p->name, "xai"));
    p = provider_find("claude:claude-opus-5", &model); CHECK(p && p->style == PROVIDER_MESSAGES); CHECK_STR(model, "claude-opus-5");
    CHECK(provider_find("OpenAI:gpt-5.2", NULL) != NULL);
    p = provider_find("chatgpt:gpt-5.2", &model); CHECK(p && !strcmp(p->name, "openai")); CHECK_STR(model, "gpt-5.2");
    CHECK(!provider_find("qwen3:32b", NULL) && !provider_find("gpt-oss:120b-cloud", NULL) && !provider_find("xai:", NULL) && !provider_find(NULL, NULL));
    CHECK(provider_at(0) && !strcmp(provider_at(0)->name, "xai") && provider_at(0)->example && !provider_at(99) && !provider_at(-1));
    cJSON *ml = provider_parse_models("{\"data\":[{\"id\":\"claude-opus-5\",\"display_name\":\"Claude Opus 5\"},{\"id\":\"gpt-5.2\"},{\"object\":\"model\"}],\"has_more\":false}");
    CHECK(ml && cJSON_GetArraySize(ml) == 2);
    CHECK_STR(jstr_at(cJSON_GetArrayItem(ml, 0), "id"), "claude-opus-5"); CHECK_STR(jstr_at(cJSON_GetArrayItem(ml, 0), "name"), "Claude Opus 5");
    CHECK_STR(jstr_at(cJSON_GetArrayItem(ml, 1), "id"), "gpt-5.2"); CHECK(!jstr_at(cJSON_GetArrayItem(ml, 1), "name"));
    cJSON_Delete(ml);
    CHECK(!provider_parse_models("{\"error\":{\"message\":\"nope\"}}") && !provider_parse_models("not json") && !provider_parse_models(NULL));
    {   /* prices: the list by name or dated snapshot, and what /usage price sets */
        double pi = 0, po = 0;
        CHECK(model_price("anthropic:claude-opus-5", &pi, &po) == 1 && pi == 5 && po == 25);
        CHECK(model_price("claude:claude-opus-5-5", &pi, &po) == 1 && pi == 4 && po == 20);            /* the longer name wins, under either provider name */
        CHECK(model_price("anthropic:claude-haiku-5-5-20260901", &pi, &po) == 1 && pi == 0.10 && po == 0.50);
        CHECK(model_price("openai:gpt-5-mini", &pi, &po) == 1 && pi == 0.25 && po == 2);
        CHECK(model_price("openai:gpt-5-2025-08-07", &pi, &po) == 1 && pi == 1.25);
        CHECK(model_price("openai:gpt-5.9", &pi, &po) == 0 && model_price("openai:gpt-5-turbo", &pi, &po) == 0);   /* "gpt-5" says nothing about these */
        CHECK(model_price("xai:grok-4.7", &pi, &po) == 1 && pi == 2 && po == 6);
        CHECK(model_price("qwen3:32b", &pi, &po) == 0 && model_price("anthropic:claude-opus-5x", &pi, &po) == 0 && model_price(NULL, &pi, &po) == 0);
        /* prompt caching: a read costs a fraction of the input, a write more; the reply's usage splits them out */
        CHECK(model_cache_read_ratio("anthropic:claude-opus-5") == 0.10 && model_cache_read_ratio("claude:claude-sonnet-5-5") == 0.05);
        CHECK(model_cache_read_ratio("claude:claude-fable-5-1-20260901") == 0.025 && model_cache_read_ratio("xai:grok-4.7") == 0.25);
        /* OpenAI and xAI: their cached input at their own rate; a model with none is billed as input */
        CHECK(model_cache_read_ratio("openai:gpt-5.5-2026-07-01") == 0.10 && model_cache_read_ratio("openai:gpt-4.1-mini") == 0.25);
        CHECK(model_cache_read_ratio("openai:gpt-5.5-pro") == 1 && model_cache_read_ratio("xai:grok-4.5") == 0.15);
        { int kn = 0; double c = usage_cost_usd("openai:gpt-5.5", 1000000, 1000000, 800000, 0, &kn);   /* 200k at $5 = 1.0, 800k cached at $0.50 = 0.4, 1M out at $30 = 30 */
          CHECK(kn == 1 && c > 31.3999 && c < 31.4001); }
        CHECK(model_cache_write_ratio("anthropic:claude-opus-5") == 1.25 && model_cache_write_ratio("openai:gpt-5") == 1);
        { /* Opus 5 at $5 in / $25 out: 1M output, 100k plain input, 800k read, 100k written = 0.5 + 0.4 + 0.625 + 25 */
            int kn = 0; double c = usage_cost_usd("anthropic:claude-opus-5", 1000000, 1000000, 800000, 100000, &kn);
            CHECK(kn == 1 && c > 26.5249 && c < 26.5251);
            CHECK(usage_cost_usd("qwen3:32b", 1000, 1000, 0, 0, &kn) == 0 && kn == 0);
        }
        {
            chat_stats cst; char cerr[256];
            cJSON *cr = provider_turn_reply("anthropic:claude-opus-5", "{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],\"stop_reason\":\"end_turn\","
                "\"usage\":{\"input_tokens\":10,\"cache_read_input_tokens\":500,\"cache_creation_input_tokens\":40,\"output_tokens\":5}}", 200, &cst, cerr, sizeof cerr);
            CHECK(cr != NULL && cst.prompt_tokens == 550 && cst.cache_read == 500 && cst.cache_write == 40 && cst.eval_tokens == 5);
            cJSON_Delete(cr);
        }
        {
            chat_stats ost; char oerr[256];
            char *otxt = provider_parse_reply("openai:gpt-5.5", "{\"choices\":[{\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
                "\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":7,\"prompt_tokens_details\":{\"cached_tokens\":800}}}", 200, &ost, oerr, sizeof oerr);
            CHECK(otxt && !strcmp(otxt, "hi"));
            CHECK(ost.prompt_tokens == 1000 && ost.cache_read == 800 && ost.eval_tokens == 7);
            free(otxt);
        }
        price_set("grok:grok-4.7", 3, 9);
        CHECK(model_price("xai:grok-4.7", &pi, &po) == 2 && pi == 3 && po == 9);
        price_set("xai:grok-4.7", -1, -1);
        CHECK(model_price("xai:grok-4.7", &pi, &po) == 1 && pi == 2);
    }
    p = provider_find("openai:x", NULL);
    unsetenv("OPENAI_BASE_URL"); CHECK_STR(provider_base_url(p), "https://api.openai.com/v1");
    setenv("OPENAI_BASE_URL", "http://127.0.0.1:9/v1", 1); CHECK_STR(provider_base_url(p), "http://127.0.0.1:9/v1"); unsetenv("OPENAI_BASE_URL");
    CHECK_STR(provider_base_url(provider_find("anthropic:x", NULL)), "https://api.anthropic.com");

    /* what each can be set to */
    model_info mi;
    provider_parse_model("openai:gpt-5.2", "{\"id\":\"gpt-5.2\",\"object\":\"model\"}", &mi);
    CHECK(mi.thinking && mi.think_off && !mi.think_on && mi.n_think_levels == 6 && !strcmp(mi.think_levels[0], "minimal") && !strcmp(mi.think_levels[5], "max"));
    provider_parse_model("xai:grok-4.7", NULL, &mi);
    CHECK(mi.thinking && mi.n_think_levels == 4 && !strcmp(mi.think_levels[3], "xhigh"));
    CHECK_STR(effort_resolve(&mi, "max", NULL), "xhigh");   /* as hard as it goes, in its own name */
    const char *OPUS = "{\"id\":\"claude-opus-5\",\"max_input_tokens\":1000000,\"max_tokens\":128000,\"capabilities\":{"
        "\"thinking\":{\"supported\":true,\"types\":{\"enabled\":{\"supported\":false},\"adaptive\":{\"supported\":true}}},"
        "\"effort\":{\"supported\":true,\"low\":{\"supported\":true},\"medium\":{\"supported\":true},\"high\":{\"supported\":true},\"xhigh\":{\"supported\":true},\"max\":{\"supported\":true}}}}";
    provider_parse_model("anthropic:claude-opus-5", OPUS, &mi);
    CHECK(mi.context_length == 1000000 && mi.think_adaptive && !mi.think_budget && mi.n_think_levels == 5 && !mi.think_off && !strcmp(mi.think_default, "high"));
    CHECK(mi.tools && mi.max_output == 128000);   /* the endpoint's max_tokens is the reply cap; tools are on for a Messages model */
    model_info opus = mi;
    const char *HAIKU = "{\"id\":\"claude-haiku-4-5\",\"max_input_tokens\":200000,\"capabilities\":{"
        "\"thinking\":{\"supported\":true,\"types\":{\"enabled\":{\"supported\":true},\"adaptive\":{\"supported\":false}}},\"effort\":{\"supported\":false}}}";
    provider_parse_model("anthropic:claude-haiku-4-5", HAIKU, &mi);
    CHECK(mi.think_budget && !mi.think_adaptive && mi.n_think_levels == 0 && mi.think_off && mi.think_on && !strcmp(mi.think_default, "off"));
    CHECK(mi.tools && mi.max_output == 0);   /* this fixture names no max_tokens, so the cap stays the general one */
    model_info haiku = mi;
    provider_parse_model("anthropic:claude-x", "{\"id\":\"claude-x\"}", &mi);   /* a proxy that says nothing: nothing is sent that it might refuse */
    CHECK(!mi.thinking && !mi.think_adaptive);

    /* Chat Completions */
    model_info oai; provider_parse_model("openai:gpt-5.2", NULL, &oai);
    provider_request rq = { "SYS", "BRIEF", 16000, "high", NULL, 0 };
    char *body = provider_request_body("openai:gpt-5.2", &oai, &rq);
    cJSON *j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "model"), "gpt-5.2");
    CHECK_STR(jstr_at(j, "messages.0.role"), "system"); CHECK_STR(jstr_at(j, "messages.0.content"), "SYS");
    CHECK_STR(jstr_at(j, "messages.1.role"), "user"); CHECK_STR(jstr_at(j, "messages.1.content"), "BRIEF");
    CHECK(cJSON_GetNumberValue(jget(j, "max_completion_tokens")) == 16000 && !jget(j, "max_tokens"));   /* the name reasoning models take */
    CHECK_STR(jstr_at(j, "reasoning_effort"), "high");
    CHECK(!jget(j, "stream") && !jget(j, "tools") && !jget(j, "temperature"));
    cJSON_Delete(j); free(body);
    rq.effort = "off"; body = provider_request_body("openai:gpt-5.2", &oai, &rq); j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "reasoning_effort"), "none"); cJSON_Delete(j); free(body);
    rq.effort = NULL; body = provider_request_body("openai:gpt-5.2", &oai, &rq); j = cJSON_Parse(body);
    CHECK(!jget(j, "reasoning_effort")); cJSON_Delete(j); free(body);   /* left to the model */

    /* the Messages API */
    rq.effort = "xhigh";
    body = provider_request_body("anthropic:claude-opus-5", &opus, &rq); j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "model"), "claude-opus-5"); CHECK_STR(jstr_at(j, "system"), "SYS");
    CHECK(cJSON_GetArraySize(jget(j, "messages")) == 1); CHECK_STR(jstr_at(j, "messages.0.role"), "user");
    CHECK(cJSON_GetNumberValue(jget(j, "max_tokens")) == 16000);
    CHECK_STR(jstr_at(j, "thinking.type"), "adaptive"); CHECK_STR(jstr_at(j, "output_config.effort"), "xhigh");
    CHECK_STR(jstr_at(j, "fallbacks"), "default"); CHECK(provider_fallbacks("anthropic:claude-opus-5"));
    CHECK(provider_fallbacks("anthropic:claude-sonnet-5-5") && !provider_fallbacks("anthropic:claude-sonnet-5"));
    CHECK(!provider_fallbacks("anthropic:claude-haiku-5-5") && !provider_fallbacks("ollama:qwen3:32b"));
    CHECK(!jget(j, "temperature") && !jget(j, "thinking.budget_tokens"));
    cJSON_Delete(j); free(body);
    rq.effort = NULL;
    body = provider_request_body("anthropic:claude-sonnet-5", &opus, &rq); j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "thinking.type"), "adaptive"); CHECK(!jget(j, "output_config") && !jget(j, "fallbacks"));   /* no effort: the API's own; no fallbacks where there are none */
    cJSON_Delete(j); free(body);
    rq.effort = "on";
    body = provider_request_body("anthropic:claude-haiku-4-5", &haiku, &rq); j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "thinking.type"), "enabled"); CHECK(cJSON_GetNumberValue(jget(j, "thinking.budget_tokens")) == 8000 && !jget(j, "output_config"));
    cJSON_Delete(j); free(body);
    rq.effort = NULL;
    body = provider_request_body("anthropic:claude-haiku-4-5", &haiku, &rq); j = cJSON_Parse(body);
    CHECK(!jget(j, "thinking")); cJSON_Delete(j); free(body);

    /* answers */
    chat_stats st; char err[512];
    memset(&st, 0, sizeof st);
    char *t = provider_parse_reply("openai:gpt-5.2", "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ADVICE: read it\"},\"finish_reason\":\"stop\"}],"
                                   "\"usage\":{\"prompt_tokens\":1200,\"completion_tokens\":300}}", 200, &st, err, sizeof err);
    CHECK_STR(t, "ADVICE: read it"); CHECK(st.prompt_tokens == 1200 && st.eval_tokens == 300); CHECK_STR(st.done_reason, "stop"); free(t);
    memset(&st, 0, sizeof st);
    t = provider_parse_reply("xai:grok-4.7", "{\"choices\":[{\"message\":{\"content\":\"\"},\"finish_reason\":\"length\"}]}", 200, &st, err, sizeof err);
    CHECK_STR(t, ""); CHECK_STR(st.done_reason, "length"); free(t);   /* spent it all thinking: an empty answer, cut */
    t = provider_parse_reply("openai:gpt-5.2", "{\"choices\":[{\"message\":{\"content\":null,\"refusal\":\"I can't help with that.\"},\"finish_reason\":\"stop\"}]}", 200, &st, err, sizeof err);
    CHECK(!t && strstr(err, "declined") && strstr(err, "can't help"));
    memset(&st, 0, sizeof st);
    t = provider_parse_reply("anthropic:claude-opus-5", "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"x\"},{\"type\":\"text\",\"text\":\"Verdict: \"},{\"type\":\"text\",\"text\":\"fine.\"}],"
                             "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":900,\"cache_read_input_tokens\":100,\"output_tokens\":42}}", 200, &st, err, sizeof err);
    CHECK_STR(t, "Verdict: fine."); CHECK(st.prompt_tokens == 1000 && st.eval_tokens == 42); free(t);
    memset(&st, 0, sizeof st);
    t = provider_parse_reply("anthropic:claude-opus-5", "{\"content\":[{\"type\":\"text\",\"text\":\"Half an ans\"}],\"stop_reason\":\"max_tokens\"}", 200, &st, err, sizeof err);
    CHECK_STR(t, "Half an ans"); CHECK_STR(st.done_reason, "length"); free(t);
    t = provider_parse_reply("anthropic:claude-fable-5-1", "{\"content\":[],\"stop_reason\":\"refusal\",\"stop_details\":{\"type\":\"refusal\",\"category\":\"cyber\"}}", 200, &st, err, sizeof err);
    CHECK(!t && strstr(err, "declined") && strstr(err, "cyber"));
    /* errors say what to do about them, and never how the key reads */
    t = provider_parse_reply("anthropic:claude-opus-5", "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}", 401, &st, err, sizeof err);
    CHECK(!t && strstr(err, "ANTHROPIC_API_KEY") && strstr(err, "invalid x-api-key"));
    t = provider_parse_reply("xai:grok-9", "{\"code\":\"Some resource has not been found\",\"error\":\"The model grok-9 does not exist.\"}", 404, &st, err, sizeof err);
    CHECK(!t && strstr(err, "does not know a model 'grok-9'") && strstr(err, "does not exist"));
    /* the answers the real APIs give a wrong key: xAI says 400; OpenAI echoes a masked piece of the key, which goes no further */
    t = provider_parse_reply("xai:grok-4.7", "{\"code\":\"Client specified an invalid argument\",\"error\":\"Incorrect API key provided. You can obtain an API key from https://console.x.ai.\"}", 400, &st, err, sizeof err);
    CHECK(!t && strstr(err, "xAI refused the key in XAI_API_KEY (400"));
    t = provider_parse_reply("openai:gpt-5.2", "{\"error\":{\"message\":\"Incorrect API key provided: sk-proj-********wxyz. You can find your API key at https://platform.openai.com/account/api-keys.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_api_key\"}}", 401, &st, err, sizeof err);
    CHECK(!t && strstr(err, "refused the key in OPENAI_API_KEY") && !strstr(err, "wxyz") && !strstr(err, "sk-proj") && strstr(err, "Incorrect API key provided. You can find your API key"));
    t = provider_parse_reply("openai:gpt-5.2", "{\"error\":{\"message\":\"Rate limit\\nreached\",\"type\":\"requests\"}}", 429, &st, err, sizeof err);
    CHECK(!t && strstr(err, "rate-limiting") && !strchr(err, '\n'));
    t = provider_parse_reply("openai:gpt-5.2", "<html>bad gateway</html>", 502, &st, err, sizeof err);
    CHECK(!t && strstr(err, "answered 502") && strstr(err, "bad gateway"));

    /* a key goes over TLS or stays on this machine */
    setenv("OPENAI_API_KEY", "sk-test", 1);
    setenv("OPENAI_BASE_URL", "http://proxy.lan:4000/v1", 1);
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == -1 && strstr(err, "OPENAI_BASE_URL is plain http to another machine (proxy.lan)") && strstr(err, "unencrypted"));
    setenv("OPENAI_BASE_URL", "proxy.lan:4000/v1", 1);   /* no scheme: libcurl would send it as http */
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == -1 && strstr(err, "(proxy.lan)"));
    setenv("OPENAI_BASE_URL", "http://127.evil.example/v1", 1);   /* a name that merely starts like a loopback address */
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == -1 && strstr(err, "(127.evil.example)"));
    setenv("OPENAI_BASE_URL", "http://proxy.lan:4000/v1", 1);
    bool ab0; t = provider_chat("openai:gpt-5.2", &oai, &rq, &st, &ab0, err, sizeof err);
    CHECK(!t && !ab0 && strstr(err, "unencrypted"));
    setenv("OPENAI_BASE_URL", "http://10.0.0.5/v1", 1);
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == -1 && strstr(err, "(10.0.0.5)"));
    setenv("OPENAI_BASE_URL", "http://[fd00::1]:8080/v1", 1);
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == -1 && strstr(err, "([fd00::1])"));
    setenv("OPENAI_BASE_URL", "http://localhost:1/v1", 1);   /* this machine: allowed, and so it is tried (and nothing listens there) */
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == 1 && !strstr(err, "unencrypted"));
    setenv("OPENAI_BASE_URL", "http://[::1]:1/v1", 1);
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == 1 && !strstr(err, "unencrypted"));
    setenv("OPENAI_BASE_URL", "https://127.0.0.1:1/v1", 1);
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == 1 && !strstr(err, "unencrypted"));
    unsetenv("OPENAI_BASE_URL"); unsetenv("OPENAI_API_KEY");

    /* without a key there is no request at all */
    unsetenv("XAI_API_KEY");
    CHECK(provider_model_info("xai:grok-4.7", &mi, err, sizeof err) == -1 && strstr(err, "XAI_API_KEY is not set"));
    bool ab; t = provider_chat("xai:grok-4.7", &mi, &rq, &st, &ab, err, sizeof err);
    CHECK(!t && !ab && strstr(err, "XAI_API_KEY is not set"));

    /* a working-model turn: the stored conversation stays user / assistant / tool; the wire is Messages */
    cJSON *tools = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"bash\",\"description\":\"run\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}}},"
                               "{\"type\":\"function\",\"function\":{\"name\":\"advisor\",\"description\":\"ask\",\"parameters\":{\"type\":\"object\"}}}]");
    cJSON *msgs = cJSON_Parse("[{\"role\":\"system\",\"content\":\"SYS\"},{\"role\":\"user\",\"content\":\"do the thing\"},"
                              "{\"role\":\"assistant\",\"content\":\"Running it.\",\"tool_calls\":[{\"id\":\"toolu_bash\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"echo hi\\\"}\"}}]},"
                              "{\"role\":\"tool\",\"tool_name\":\"bash\",\"tool_call_id\":\"toolu_bash\",\"content\":\"hi\"},{\"role\":\"user\",\"content\":\"and now?\"}]");
    body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, msgs, tools, 64000, NULL, false, -1, true);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "model"), "claude-haiku-4-5");
    CHECK(cJSON_IsTrue(jget(j, "stream")) && cJSON_GetNumberValue(jget(j, "max_tokens")) == 64000);
    CHECK(cJSON_IsArray(jget(j, "system")) && !jget(j, "thinking") && !jget(j, "output_config") && !jget(j, "fallbacks"));
    CHECK_STR(jstr_at(j, "system.0.type"), "text"); CHECK_STR(jstr_at(j, "system.0.text"), "SYS");
    CHECK_STR(jstr_at(j, "system.0.cache_control.type"), "ephemeral");
    CHECK_STR(jstr_at(j, "tools.0.name"), "bash"); CHECK(jget(j, "tools.0.input_schema.properties.command") && !jget(j, "tools.0.function"));
    CHECK(cJSON_GetArraySize(jget(j, "messages")) == 3);   /* the tool result and the next user line are one user message */
    CHECK_STR(jstr_at(j, "messages.0.content.0.text"), "do the thing");
    CHECK_STR(jstr_at(j, "messages.1.content.0.text"), "Running it.");
    CHECK_STR(jstr_at(j, "messages.1.content.1.type"), "tool_use"); CHECK_STR(jstr_at(j, "messages.1.content.1.id"), "toolu_bash");
    CHECK_STR(jstr_at(j, "messages.1.content.1.input.command"), "echo hi");
    CHECK_STR(jstr_at(j, "messages.2.content.0.type"), "tool_result"); CHECK_STR(jstr_at(j, "messages.2.content.0.tool_use_id"), "toolu_bash");
    CHECK_STR(jstr_at(j, "messages.2.content.0.content"), "hi"); CHECK_STR(jstr_at(j, "messages.2.content.1.text"), "and now?");
    CHECK_STR(jstr_at(j, "messages.2.content.1.cache_control.type"), "ephemeral");
    cJSON_Delete(j); free(body);

    /* a reply with thinking is stored as blocks and sent back unchanged, cache breakpoint on the copy */
    memset(&st, 0, sizeof st);
    cJSON *msg = provider_turn_reply("anthropic:claude-haiku-4-5",
        "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"weighing\",\"signature\":\"sig\"},{\"type\":\"text\",\"text\":\"Running it.\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"bash\",\"input\":{\"command\":\"echo hi\"}}],\"stop_reason\":\"tool_use\","
        "\"usage\":{\"input_tokens\":10,\"cache_creation_input_tokens\":3,\"cache_read_input_tokens\":7,\"output_tokens\":4}}",
        200, &st, err, sizeof err);
    CHECK_STR(jstr_at(msg, "role"), "assistant"); CHECK_STR(jstr_at(msg, "content"), "Running it.");
    CHECK_STR(jstr_at(msg, "tool_calls.0.id"), "toolu_1"); CHECK_STR(jstr_at(msg, "tool_calls.0.function.name"), "bash");
    CHECK_STR(jstr_at(msg, "tool_calls.0.function.arguments"), "{\"command\":\"echo hi\"}");
    CHECK_STR(jstr_at(msg, "provider_content.0.type"), "thinking"); CHECK_STR(jstr_at(msg, "provider_content.0.thinking"), "weighing");
    CHECK_STR(jstr_at(msg, "provider_content.0.signature"), "sig"); CHECK_STR(jstr_at(msg, "provider_content.2.id"), "toolu_1");
    CHECK(st.prompt_tokens == 20 && st.eval_tokens == 4); CHECK_STR(st.done_reason, "stop");
    cJSON *follow = cJSON_Parse("[{\"role\":\"user\",\"content\":\"go\"}]");
    cJSON_AddItemToArray(follow, msg);
    body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, follow, tools, 64000, NULL, false, -1, false);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "messages.1.content.0.type"), "thinking"); CHECK_STR(jstr_at(j, "messages.1.content.0.thinking"), "weighing");
    CHECK_STR(jstr_at(j, "messages.1.content.0.signature"), "sig"); CHECK_STR(jstr_at(j, "messages.1.content.2.id"), "toolu_1");
    CHECK_STR(jstr_at(j, "messages.1.content.2.cache_control.type"), "ephemeral");
    CHECK(!jget(msg, "provider_content.0.cache_control") && !jget(msg, "provider_content.2.cache_control"));   /* the stored blocks are not the copy */
    cJSON_Delete(j); free(body); cJSON_Delete(follow);

    /* quiet: no thinking and no effort, even on a model that would otherwise send both. A real level still goes out when asked */
    body = provider_turn_body("anthropic:claude-opus-5", &opus, msgs, NULL, 256, NULL, false, -1, false);
    j = cJSON_Parse(body);
    CHECK(cJSON_IsFalse(jget(j, "stream")) && !jget(j, "thinking") && !jget(j, "output_config"));
    CHECK_STR(jstr_at(j, "fallbacks"), "default");   /* opus still asks for a fallback model; that is not thinking */
    cJSON_Delete(j); free(body);
    body = provider_turn_body("anthropic:claude-opus-5", &opus, msgs, NULL, 32000, "xhigh", true, -1, false);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "thinking.type"), "adaptive"); CHECK_STR(jstr_at(j, "output_config.effort"), "xhigh");
    cJSON_Delete(j); free(body);
    body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, msgs, NULL, 64000, NULL, true, -1, false);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "thinking.type"), "enabled"); CHECK(cJSON_GetNumberValue(jget(j, "thinking.budget_tokens")) == 32000);
    CHECK(!jget(j, "output_config") && !jget(j, "fallbacks"));
    cJSON_Delete(j); free(body);

    /* extended thinking refuses a temperature: it goes out only when no thinking object does */
    body = provider_turn_body("anthropic:claude-opus-5", &opus, msgs, NULL, 32000, NULL, true, 0.2, false);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "thinking.type"), "adaptive"); CHECK(!jget(j, "temperature"));
    cJSON_Delete(j); free(body);
    body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, msgs, NULL, 64000, NULL, true, 0.2, false);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "thinking.type"), "enabled"); CHECK(!jget(j, "temperature"));
    cJSON_Delete(j); free(body);
    body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, msgs, NULL, 64000, NULL, false, 0.2, false);
    j = cJSON_Parse(body);
    CHECK(!jget(j, "thinking") && cJSON_GetNumberValue(jget(j, "temperature")) > 0.19);
    cJSON_Delete(j); free(body);

    /* a call the request is not offering (the advisor, once it was turned off) goes out as text, or the next call is a 400 */
    cJSON *bash_only = cJSON_Parse("[{\"type\":\"function\",\"function\":{\"name\":\"bash\",\"description\":\"run\",\"parameters\":{\"type\":\"object\"}}}]");
    cJSON *dem = cJSON_Parse("[{\"role\":\"user\",\"content\":\"ask\"},"
                             "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"call_adv_1\",\"function\":{\"name\":\"advisor\",\"arguments\":{\"question\":\"why\"}}}]},"
                             "{\"role\":\"tool\",\"tool_name\":\"advisor\",\"tool_call_id\":\"call_adv_1\",\"content\":\"advice text\"}]");
    body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, dem, bash_only, 64000, NULL, false, -1, false);
    j = cJSON_Parse(body);
    CHECK_STR(jstr_at(j, "messages.1.content.0.type"), "text");
    CHECK(jstr_at(j, "messages.1.content.0.text") && strstr(jstr_at(j, "messages.1.content.0.text"), "[called advisor]") && !jget(j, "messages.1.content.1"));
    CHECK_STR(jstr_at(j, "messages.2.content.0.type"), "text");
    CHECK(jstr_at(j, "messages.2.content.0.text") && strstr(jstr_at(j, "messages.2.content.0.text"), "[advisor result]\nadvice text"));
    cJSON_Delete(j); free(body); cJSON_Delete(dem);

    /* more tool_use blocks than the table holds: the message is rebuilt, so no tool_use is left without its tool_result */
    {
        sbuf big; sb_init(&big);
        sb_puts(&big, "[{\"role\":\"user\",\"content\":\"go\"},{\"role\":\"assistant\",\"content\":\"\",\"provider_content\":[");
        for (int k = 0; k < 66; k++) sb_printf(&big, "%s{\"type\":\"tool_use\",\"id\":\"toolu_%d\",\"name\":\"bash\",\"input\":{}}", k ? "," : "", k);
        sb_puts(&big, "],\"tool_calls\":[");
        for (int k = 0; k < 66; k++) sb_printf(&big, "%s{\"id\":\"toolu_%d\",\"function\":{\"name\":\"bash\",\"arguments\":{}}}", k ? "," : "", k);
        sb_puts(&big, "]}");
        for (int k = 0; k < 66; k++) sb_printf(&big, ",{\"role\":\"tool\",\"tool_name\":\"bash\",\"tool_call_id\":\"toolu_%d\",\"content\":\"r\"}", k);
        sb_puts(&big, "]");
        cJSON *many = cJSON_Parse(big.data); sb_free(&big);
        body = provider_turn_body("anthropic:claude-haiku-4-5", &haiku, many, bash_only, 64000, NULL, false, -1, false);
        j = cJSON_Parse(body);
        int uses = 0, results = 0;
        cJSON *mm, *bb;
        cJSON_ArrayForEach(mm, jget(j, "messages")) {
            cJSON_ArrayForEach(bb, jget(mm, "content")) {
                const char *ty = jstr_at(bb, "type");
                if (ty && !strcmp(ty, "tool_use")) uses++;
                if (ty && !strcmp(ty, "tool_result")) results++;
            }
        }
        CHECK(uses == 64 && results == 64);
        cJSON_Delete(j); free(body); cJSON_Delete(many);
    }
    cJSON_Delete(bash_only);

    /* a refusal is the turn's answer. The advisor's parser still reports it as a failure and returns nothing */
    memset(&st, 0, sizeof st);
    const char *REF = "{\"content\":[],\"stop_reason\":\"refusal\",\"stop_details\":{\"type\":\"refusal\",\"category\":\"cyber\"}}";
    msg = provider_turn_reply("anthropic:claude-fable-5", REF, 200, &st, err, sizeof err);
    CHECK(msg && strstr(jstr_at(msg, "content"), "claude-fable-5 declined to answer (refusal: cyber)") && !jget(msg, "tool_calls"));
    CHECK_STR(st.done_reason, "stop");
    cJSON_Delete(msg);
    t = provider_parse_reply("anthropic:claude-fable-5", REF, 200, &st, err, sizeof err);
    CHECK(!t && strstr(err, "declined") && strstr(err, "cyber"));
    cJSON_Delete(msgs); cJSON_Delete(tools);
}

/* ---------- what a request body must satisfy: valid UTF-8, every call answered ---------- */
static void test_wellformed(void) {
    /* tail_incomplete: a chunk that ends inside an escape sequence or a UTF-8 character */
    CHECK(!tail_incomplete("abc", 3));
    CHECK(tail_incomplete("x\x1b[38;5", 7));                              /* CSI cut in the parameters */
    CHECK(!tail_incomplete("x\x1b[0m", 6));                               /* CSI complete */
    CHECK(tail_incomplete("\xe2\x94", 2));                                /* a 3-byte character cut short */
    CHECK(!tail_incomplete("\xe2\x94\x80", 3));                          /* … complete */
    CHECK(tail_incomplete("\x1b]8;;u", 6));                                /* OSC without its end */
    CHECK(!tail_incomplete("\x1b]8;;u\x07", 7));                          /* … ended by BEL */
    CHECK(tail_incomplete("a\x1b", 2));                                   /* a lone ESC */
    CHECK(!tail_incomplete("a\x1b" "7", 3));                                 /* ESC 7 is two bytes */
    CHECK(tail_incomplete("a\x1b(", 3));                                  /* ESC ( wants its final byte */
    CHECK(!tail_incomplete("a\x1b(B", 4));
    /* utf8_ok: good sequences pass, every kind of bad byte fails */
    CHECK(utf8_ok("plain ascii", 11));
    CHECK(utf8_ok("æøå €  𝄞", strlen("æøå €  𝄞")));                      /* 2-, 3- and 4-byte characters */
    CHECK(!utf8_ok("a\xC3", 2));                                       /* a character cut short */
    CHECK(!utf8_ok("\x80rest", 5));                                    /* a bare continuation byte */
    CHECK(!utf8_ok("\xC0\xAF", 2));                                   /* overlong */
    CHECK(!utf8_ok("\xED\xA0\x80", 3));                              /* UTF-16 surrogate */
    CHECK(!utf8_ok("\xF4\x90\x80\x80", 4));                         /* above U+10FFFF */
    CHECK(!utf8_ok("\xFF", 1));

    /* utf8_fix: invalid bytes become U+FFFD, valid text is untouched */
    char *f = utf8_fix("ok \xC3 mid \xE2\x82\xAC end \xFF");
    CHECK_STR(f, "ok \xEF\xBF\xBD mid \xE2\x82\xAC end \xEF\xBF\xBD");
    CHECK(utf8_ok(f, strlen(f)));
    free(f);

    /* utf8_cut: never splits a character, never longer than asked */
    const char *u = "ab\xC3\xA6\xC3\xA6";                               /* ab, æ, æ: 6 bytes */
    CHECK(utf8_cut(u, 6, 3) == 2);                                         /* 3 would end inside the first æ */
    CHECK(utf8_cut(u, 6, 4) == 4);
    CHECK(utf8_cut(u, 6, 9) == 6);

    /* messages_repair: a stopped request's calls get their results */
    const char *orphan =
        "[{\"role\":\"user\",\"content\":\"go\"},"
        "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
            "{\"id\":\"t1\",\"function\":{\"name\":\"bash\",\"arguments\":{}}},"
            "{\"id\":\"t2\",\"function\":{\"name\":\"read_file\",\"arguments\":{}}}]},"
        "{\"role\":\"tool\",\"tool_name\":\"bash\",\"tool_call_id\":\"t1\",\"content\":\"done\"},"
        "{\"role\":\"user\",\"content\":\"continue\"}]";
    cJSON *msgs = cJSON_Parse(orphan);
    CHECK(msgs != NULL);
    CHECK(messages_repair(msgs) == 1);                                     /* t2 only: t1 was answered */
    CHECK(cJSON_GetArraySize(msgs) == 5);
    cJSON *r = cJSON_GetArrayItem(msgs, 3);                                /* t2's result sits after the existing result */
    CHECK_STR(jstr_at(r, "tool_call_id"), "t2");
    CHECK_STR(jstr_at(r, "tool_name"), "read_file");
    CHECK_STR(jstr_at(cJSON_GetArrayItem(msgs, 4), "role"), "user");          /* the user's message stays last */
    CHECK(messages_repair(msgs) == 0);                                     /* a second pass finds nothing to mend */

    /* a call answered by a later result of a *different* reply is not answered for this one */
    cJSON *twice = cJSON_Parse(
        "[{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"x\",\"function\":{\"name\":\"bash\"}}]},"
        "{\"role\":\"user\",\"content\":\"hi\"},"
        "{\"role\":\"tool\",\"tool_call_id\":\"x\",\"content\":\"late\"}]");
    CHECK(messages_repair(twice) == 1);
    CHECK(cJSON_GetArraySize(twice) == 4);
    CHECK(messages_repair(twice) == 0);                                    /* a conversation with every call answered: nothing added */
    CHECK(cJSON_GetArraySize(twice) == 4);
    CHECK_STR(jstr_at(cJSON_GetArrayItem(twice, 1), "tool_call_id"), "x");
    cJSON_Delete(twice);

    /* invalid UTF-8 already in the conversation (a saved session, an old cut) is mended in place */
    cJSON *bad = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "tool");
    cJSON_AddStringToObject(m, "content", "output \xE2\x82 cut");
    cJSON_AddItemToArray(bad, m);
    CHECK(messages_repair(bad) == 1);
    CHECK(utf8_ok(jstr_at(m, "content"), strlen(jstr_at(m, "content"))));
    CHECK_STR(jstr_at(m, "content"), "output \xEF\xBF\xBD\xEF\xBF\xBD cut");   /* E2 and 82: two bad bytes */
    CHECK(messages_repair(bad) == 0);
    cJSON_Delete(bad);
    cJSON_Delete(msgs);

    /* messages_drop_thinking: a changed prefix drops thinking and redacted_thinking, and leaves the turn */
    cJSON *th = cJSON_Parse(
        "[{\"role\":\"assistant\",\"content\":\"a\",\"provider_content\":["
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s1\"},"
            "{\"type\":\"text\",\"text\":\"a\"}]},"
         "{\"role\":\"tool\",\"tool_name\":\"bash\",\"tool_call_id\":\"t\",\"content\":\"done\"},"
         "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"t\",\"function\":{\"name\":\"bash\",\"arguments\":\"{}\"}}],"
            "\"provider_content\":["
            "{\"type\":\"redacted_thinking\",\"data\":\"x\"},"
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s2\"},"
            "{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"bash\",\"input\":{},\"caller\":{\"type\":\"direct\"}}]}]");
    CHECK(th != NULL);
    CHECK(messages_has_thinking(th, 0) && messages_has_thinking(th, 2) && !messages_has_thinking(th, 3));
    CHECK(!messages_has_thinking(NULL, 0) && messages_drop_thinking(NULL, 0) == 0 && messages_drop_thinking(th, -1) == 0);
    CHECK(messages_drop_thinking(th, 2) == 2);                                 /* the later turn only: redacted_thinking and thinking */
    CHECK(messages_has_thinking(th, 0) && !messages_has_thinking(th, 2));      /* the earlier block stays: no gap is opened in front of it */
    CHECK_STR(jstr_at(cJSON_GetArrayItem(th, 2), "provider_content.0.type"), "tool_use");
    CHECK_STR(jstr_at(cJSON_GetArrayItem(th, 2), "provider_content.0.id"), "t");
    CHECK_STR(jstr_at(cJSON_GetArrayItem(th, 2), "provider_content.0.caller.type"), "direct");
    CHECK_STR(jstr_at(cJSON_GetArrayItem(th, 2), "tool_calls.0.id"), "t");     /* the call the conversation speaks stays too */
    CHECK(messages_drop_thinking(th, 0) == 1);                                 /* the one block the first turn still held */
    CHECK_STR(jstr_at(cJSON_GetArrayItem(th, 0), "provider_content.0.type"), "text");
    CHECK(messages_drop_thinking(th, 0) == 0);
    cJSON *only = cJSON_Parse("[{\"role\":\"assistant\",\"content\":\"\",\"provider_content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s\"}]}]");
    CHECK(messages_drop_thinking(only, 0) == 1);
    CHECK(!cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(only, 0), "provider_content"));   /* an empty block list is not a reply */
    CHECK(!messages_has_thinking(only, 0));
    cJSON_Delete(only);
    cJSON_Delete(th);

    /* a block records the prefix it was signed against. Rewriting an earlier tool result cuts from
     * the first block that saw the old text; the block before that result still matches. A block
     * with no record is not guessed at. */
    cJSON *live = cJSON_Parse(
        "[{\"role\":\"user\",\"content\":\"go\"},"
         "{\"role\":\"assistant\",\"content\":\"a\",\"provider_content\":["
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s1\"},{\"type\":\"text\",\"text\":\"a\"}]},"
         "{\"role\":\"tool\",\"tool_name\":\"bash\",\"tool_call_id\":\"t\",\"content\":\"0123456789 the full output\"},"
         "{\"role\":\"assistant\",\"content\":\"b\",\"provider_content\":["
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s2\"},{\"type\":\"text\",\"text\":\"b\"}]}]");
    CHECK(live != NULL);
    messages_note_thinking(live, 0);                                          /* a user message: nothing to record */
    CHECK(!cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(live, 0), "think_bind"));
    messages_note_thinking(live, 1);
    messages_note_thinking(live, 3);
    CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(live, 1), "think_bind")));
    CHECK(messages_thinking_cut(live) == -1);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(live, 2), "content"),
                         "[earlier output elided to save context; bash returned 24 bytes]\n0123…");
    CHECK(messages_thinking_cut(live) == 3);                                  /* s1 is before the edit, s2 is after it */
    CHECK(messages_drop_thinking(live, 3) == 1);
    CHECK(messages_thinking_cut(live) == -1);
    CHECK(messages_has_thinking(live, 1) && !messages_has_thinking(live, 3));
    CHECK(!cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(live, 3), "think_bind"));
    cJSON_Delete(live);
    cJSON *bare = cJSON_Parse(
        "[{\"role\":\"tool\",\"content\":\"full\"},"
         "{\"role\":\"assistant\",\"content\":\"a\",\"provider_content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"s\"}]}]");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(bare, 0), "content"), "changed");
    CHECK(messages_thinking_cut(bare) == -1);                                 /* no think_bind: not this call's to decide */
    CHECK(messages_thinking_cut(NULL) == -1);
    cJSON_Delete(bare);
}

/* ---------- the notcurses frontend's pure half: SGR, the palette, the cells ---------- */
static void test_copy_key(void) {
    CHECK(nc_copy_key(false, 'c', true, false));    /* Ctrl+c */
    CHECK(nc_copy_key(false, 'C', true, false));    /* Ctrl+C, notcurses' capital for a letter with Ctrl */
    CHECK(nc_copy_key(false, 3, false, false));     /* the control character */
    CHECK(nc_copy_key(false, 'c', false, true));    /* Cmd+c (super) */
    CHECK(nc_copy_key(false, 'C', false, true));
    CHECK(!nc_copy_key(true, 'c', true, false));    /* a release copies nothing: one copy per press */
    CHECK(!nc_copy_key(false, 'c', false, false));  /* a plain c is typed */
    CHECK(!nc_copy_key(false, 'd', true, false));   /* Ctrl+D is not copy */
    CHECK(!nc_copy_key(false, 'v', true, false));
}

static void test_nc_pure(void) {
    nc_style st = { NC_DEFAULT, NC_DEFAULT, 0 };
    /* SGR: colours, styles, resets */
    CHECK(nc_sgr_parse("\x1b[31m", 5, &st) == 5 && st.fg == 0xbf616au);
    CHECK(nc_sgr_parse("\x1b[1002m", 7, &st) == 7 && st.bg == 0x2c3445u);   /* the theme roles, as the notcurses screen draws them */
    CHECK(nc_sgr_parse("\x1b[1001m", 7, &st) == 7 && st.fg == 0xefbcffu);
    st.bg = NC_DEFAULT; st.fg = NC_DEFAULT;
    CHECK(nc_sgr_parse("\x1b[1;3m", 6, &st) == 6 && (st.styles & NC_ST_BOLD) && (st.styles & NC_ST_ITALIC));
    CHECK(nc_sgr_parse("\x1b[22m", 5, &st) == 5 && !(st.styles & NC_ST_BOLD));
    CHECK(nc_sgr_parse("\x1b[38;5;208m", strlen("\x1b[38;5;208m"), &st) == strlen("\x1b[38;5;208m") && st.fg == g_theme->accent);   /* the theme's orange */
    CHECK(nc_sgr_parse("\x1b[48;2;1;2;3m", strlen("\x1b[48;2;1;2;3m"), &st) == strlen("\x1b[48;2;1;2;3m") && st.bg == 0x010203u);
    /* the location line's rose (256-colour 132, LOC_ROSE in term.c): the index maps to #af5f87, and no style comes with it */
    st = (nc_style){ NC_DEFAULT, NC_DEFAULT, 0 };
    CHECK(nc_sgr_parse("\x1b[38;5;132m", strlen("\x1b[38;5;132m"), &st) == strlen("\x1b[38;5;132m") && st.fg == 0xaf5f87u && st.styles == 0);
    CHECK(nc_sgr_parse("\x1b[m", 3, &st) == 3 && st.fg == NC_DEFAULT && st.styles == 0);   /* ESC[m is a reset */
    CHECK(nc_sgr_parse("\x1b[97m", 5, &st) == 5 && st.fg == g_theme->text);   /* a bright white is the theme's text: the basic codes are the theme's */
    CHECK(nc_sgr_parse("\x1b[31m", 5, &st) == 5 && st.fg == g_theme->bad);   /* 31 is the theme's red on both screens */
    /* a CSI that is not SGR is consumed and changes nothing; a private mode too */
    st.fg = 0x123456;
    CHECK(nc_sgr_parse("\x1b[2K", 4, &st) == 4 && st.fg == 0x123456);
    CHECK(nc_sgr_parse("\x1b[?25h", 6, &st) == 6 && st.fg == 0x123456);
    CHECK(nc_sgr_parse("abc", 3, &st) == 0);                                     /* not an escape at all */
    CHECK(nc_sgr_parse("\x1b[31", 4, &st) == 4);                                  /* unterminated: the rest is the escape */
    /* the palette: the 6x6x6 cube and the grey ramp */
    CHECK(nc_color256(16) == 0x000000u && nc_color256(231) == 0xffffffu && nc_color256(232) == 0x080808u);
    /* cells: text, a tab to the next stop, a multi-byte character, a combining mark, colours carried */
    nc_cell cells[64];
    nc_style carry = { NC_DEFAULT, NC_DEFAULT, 0 };
    int n = nc_cells("\x1b[1mab\x1b[0m\tc", strlen("\x1b[1mab\x1b[0m\tc"), cells, 64, &carry);
    CHECK(n == 2 + 6 + 1);                                                        /* a, b, six spaces to column 8, c */
    CHECK(cells[0].st.styles & NC_ST_BOLD && strcmp(cells[0].egc, "a") == 0);
    CHECK(!(cells[2].st.styles & NC_ST_BOLD) && cells[2].egc[0] == ' ');
    CHECK(strcmp(cells[n - 1].egc, "c") == 0);
    n = nc_cells("\xc3\xa6x", strlen("\xc3\xa6x"), cells, 64, NULL);                              /* æ then x */
    CHECK(n == 2 && strcmp(cells[0].egc, "\xc3\xa6") == 0 && cells[0].w == 1 && strcmp(cells[1].egc, "x") == 0);
    n = nc_cells("e\xcc\x81x", strlen("e\xcc\x81x"), cells, 64, NULL);                             /* e + combining acute: one cell */
    CHECK(n == 2 && strlen(cells[0].egc) == 3);
    n = nc_cells("\xe4\xb8\x80", strlen("\xe4\xb8\x80"), cells, 64, NULL);                          /* a wide character: two columns */
    CHECK(n == 1 && cells[0].w == 2);
    CHECK(nc_text_width("\xe4\xb8\x80x") == 3);
    /* wrapping: a line rows at width, a wide character is not split */
    n = nc_cells("abcdef", 6, cells, 64, NULL);
    CHECK(nc_line_rows(cells, n, 4) == 2 && nc_line_rows(cells, n, 6) == 1 && nc_line_rows(cells, 0, 4) == 1);
    n = nc_cells("abc\xe4\xb8\x80", strlen("abc\xe4\xb8\x80"), cells, 64, NULL);
    CHECK(nc_line_rows(cells, n, 4) == 2);                                        /* abc then the wide one on row 2 */
    /* a copied line is its visible cells: the theme roles and SGR codes are not text (a copy once took them) */
    n = nc_cells("\x1b[1002m\x1b[1001mhi\x1b[0m", strlen("\x1b[1002m\x1b[1001mhi\x1b[0m"), cells, 64, NULL);
    CHECK(n == 2 && strcmp(cells[0].egc, "h") == 0 && strcmp(cells[1].egc, "i") == 0);
}

/* menu rows: a block item is its label, then its description indented under it; a one-line item puts the description
 * in the label's column and wraps under it. Both wrap to the width. */
static void test_menu_rows(void) {
    sbuf rows[MENU_LINES_MAX]; int kind[MENU_LINES_MAX];
    /* block: the description is 15 cells at width 18 (the indent is 3): "simple, but a" / "crash leaves it" / "behind" */
    int n = menu_item_rows("Lock file", "simple, but a crash leaves it behind", 18, 0, true, rows, kind, MENU_LINES_MAX);
    CHECK(n == 4);
    CHECK(kind[0] == 0 && kind[1] == 3 && kind[2] == 3 && kind[3] == 3);   /* 3: the block indent */
    CHECK_STR(rows[0].data, "Lock file"); CHECK_STR(rows[1].data, "simple, but a");
    CHECK_STR(rows[2].data, "crash leaves it"); CHECK_STR(rows[3].data, "behind");
    for (int r = 0; r < n; r++) sb_free(&rows[r]);
    CHECK(menu_item_rows("Lock file", "simple, but a crash leaves it behind", 18, 0, true, NULL, NULL, 0) == 4);   /* counting only */
    /* block: a label that does not fit wraps into title rows */
    n = menu_item_rows("one two three", NULL, 5, 0, true, rows, kind, MENU_LINES_MAX);
    CHECK(n == 3 && kind[0] == 0 && kind[1] == 0 && kind[2] == 0);
    CHECK_STR(rows[0].data, "one"); CHECK_STR(rows[1].data, "two"); CHECK_STR(rows[2].data, "three");
    for (int r = 0; r < n; r++) sb_free(&rows[r]);
    /* block: the description is the width less the indent: 9 cells at width 12 */
    n = menu_item_rows("x", "the quick brown fox jumps", 12, 0, true, rows, kind, MENU_LINES_MAX);
    CHECK(n == 4);
    CHECK_STR(rows[1].data, "the quick"); CHECK_STR(rows[2].data, "brown fox"); CHECK_STR(rows[3].data, "jumps");
    for (int r = 0; r < n; r++) sb_free(&rows[r]);
    /* one line: the label in 12 cells, the description beside it in the rest (24 cells at width 30) */
    n = menu_item_rows("Lock file", "survives a crash", 30, 12, false, rows, kind, MENU_LINES_MAX);
    CHECK(n == 1 && kind[0] == 0);
    CHECK_STR(rows[0].data, "Lock file    survives a crash");   /* the label padded to 12, a space, the description */
    for (int r = 0; r < n; r++) sb_free(&rows[r]);
    /* one line: a description too long for its column (11 cells at width 24) wraps under it, past the label column */
    n = menu_item_rows("Lock file", "simple, but a crash leaves it behind", 24, 12, false, rows, kind, MENU_LINES_MAX);
    CHECK(n == 4 && kind[0] == 0 && kind[1] == 13 && kind[2] == 13 && kind[3] == 13);
    CHECK_STR(rows[0].data, "Lock file    simple, but");
    CHECK(strspn(rows[1].data, " ") == 13); CHECK_STR(rows[1].data + 13, "a crash");
    CHECK_STR(rows[2].data + 13, "leaves it"); CHECK_STR(rows[3].data + 13, "behind");
    for (int r = 0; r < n; r++) sb_free(&rows[r]);
    /* a short item with a description is two rows (block); without one, one */
    CHECK(menu_item_rows("short", "a description", 30, 0, true, NULL, NULL, 0) == 2);
    CHECK(menu_item_rows("short", NULL, 30, 0, true, NULL, NULL, 0) == 1);
    CHECK(menu_item_rows("short", "", 30, 0, false, NULL, NULL, 0) == 1);
    /* a blank row goes between two items when either has a description */
    const char *d1[] = { "", "x" }, *d2[] = { "", "" };
    CHECK(menu_item_sep(d1, 0, 1) && menu_item_sep(d1, 1, 0));
    CHECK(!menu_item_sep(d2, 0, 1) && !menu_item_sep(NULL, 0, 1));
    /* the label column: the widest label, at most 44 and half the row */
    const char *labs[] = { "a", "a much longer label", "bb" };
    CHECK(menu_label_width(labs, 3, 60) == 19);
    CHECK(menu_label_width(labs, 3, 30) == 15);
}

static void test_wrap(void) {
    /* word wrap: a row breaks after its last space; a word longer than a row is cut */
    size_t st = 0; int r = 0, c = 0;
    term_wrap("hello world foo", 15, 8, 0, 15, 1, &st, &r, &c);
    CHECK(st == 6);
    term_wrap("hello world foo", 15, 8, 0, 15, 2, &st, &r, &c);
    CHECK(st == 12 && r == 2 && c == 3);
    term_wrap("abcdefghijkl", 12, 5, 0, 12, 1, &st, &r, &c);
    CHECK(st == 5 && r == 2 && c == 2);
    term_wrap("ab cd", 5, 5, 0, 5, -1, NULL, &r, &c);
    CHECK(r == 0 && c == 5);
    term_wrap("x\ny", 3, 9, 0, 3, 1, &st, &r, &c);
    CHECK(st == 2);
}

/* ---------- themes ---------- */
static char *map_all(const char *in) {
    theme_map m; memset(&m, 0, sizeof m);
    sbuf o; sb_init(&o);
    theme_map_bytes(&m, in, strlen(in), &o);
    return o.data ? o.data : xstrdup("");
}
static void test_theme(void) {
    const theme_t *def = g_theme;
    CHECK(theme_set("nope") == false && g_theme == def);   /* an unknown name keeps the theme in use */
    CHECK(theme_set("default") && g_theme == def && !strcmp(g_theme->name, "default"));
    CHECK(theme_count() >= 2 && !strcmp(theme_name(0), "default") && theme_set("bright") && !strcmp(g_theme->name, "bright"));   /* the picker's list */
    CHECK(theme_set("default") && g_theme == def);
    /* a basic code becomes the theme's RGB; the sequence keeps its other parameters */
    char *a = map_all("\x1b[31mred\x1b[0m");
    CHECK_STR(a, "\x1b[38;2;191;97;106mred\x1b[0m");   /* #BF616A */
    free(a);
    a = map_all("\x1b[1;36mx\x1b[m");
    CHECK_STR(a, "\x1b[1;38;2;86;182;194mx\x1b[m");     /* bold stays; 36 is the info colour #56B6C2 */
    free(a);
    /* a theme role is the theme's colour when the bytes leave, so the conversation recolours with the theme */
    theme_set("default");
    a = map_all("\x1b[1002m\x1b[1001mhi");
    CHECK_STR(a, "\x1b[48;2;44;52;69m\x1b[38;2;239;188;255mhi");   /* user_bg #2C3445, user_text #EFBCFF */
    free(a);
    theme_set("bright");
    a = map_all("\x1b[1002m\x1b[1001mhi");
    CHECK_STR(a, "\x1b[48;2;227;236;247m\x1b[38;2;130;80;223mhi");  /* the bright theme's own pair */
    free(a);
    theme_set("default");
    CHECK(theme_role(1001, &(unsigned){0}, &(bool){0}) && !theme_role(31, &(unsigned){0}, &(bool){0}));
    /* the window's ground: the bright theme paints it on the full-screen ANSI screen only; the default leaves it */
    CHECK(!strcmp(theme_base_sgr(), ""));
    theme_set("bright");
    CHECK(!strcmp(theme_base_sgr(), ""));                    /* not in full-screen mode: nothing is painted */
    theme_paint_window(true);
    CHECK_STR(theme_base_sgr(), "\x1b[0;38;2;31;35;40;48;2;255;255;255m");   /* text #1F2328 on window #FFFFFF */
    a = map_all("x\x1b[0my\x1b[39mz\x1b[49mw");
    CHECK_STR(a, "x\x1b[0;38;2;31;35;40;48;2;255;255;255my\x1b[38;2;31;35;40mz\x1b[48;2;255;255;255mw");
    free(a);
    a = map_all("x\x1b[my");
    CHECK(strstr(a, "48;2;255;255;255") != NULL);            /* a bare ESC[m is a reset too */
    free(a);
    theme_paint_window(false);
    a = map_all("x\x1b[0my");
    CHECK_STR(a, "x\x1b[0my");                              /* off: a reset is a reset */
    free(a);
    theme_set("default");
    a = map_all("\x1b[38;5;208mo\x1b[90mg");
    CHECK_STR(a, "\x1b[38;2;240;136;62mo\x1b[38;2;124;135;153mg");   /* the orange, and 90 grey */
    free(a);
    /* an extended colour is numbers, not codes: 38;2;95;215;95 is the green of the field's edge, not a magenta */
    a = map_all("\x1b[38;2;95;215;95m\x1b[48;5;236m");
    CHECK_STR(a, "\x1b[38;2;95;215;95m\x1b[48;5;236m");
    free(a);
    /* what is not a themed colour goes as it came: resets, cursor moves, erases, 256-colour codes other than the accent */
    a = map_all("\x1b[0m\x1b[2K\x1b[5;9H\x1b[38;5;132mx");
    CHECK_STR(a, "\x1b[0m\x1b[2K\x1b[5;9H\x1b[38;5;132mx");
    free(a);
    /* a sequence the write ends inside is held and finished by the next write */
    theme_map m; memset(&m, 0, sizeof m);
    sbuf o; sb_init(&o);
    theme_map_bytes(&m, "ab\x1b[3", 5, &o);
    CHECK(o.len == 2 && m.np > 0);
    theme_map_bytes(&m, "1mcd", 4, &o);
    CHECK(o.data && !strcmp(o.data, "ab\x1b[38;2;191;97;106mcd"));
    CHECK(m.np == 0);
    sb_free(&o);
    /* a sequence split at a write's end: nothing of it goes out before its end, and the next write completes it */
    memset(&m, 0, sizeof m);
    sb_init(&o);
    theme_map_bytes(&m, "x\x1b[3", strlen("x\x1b[3"), &o);
    CHECK(o.len == 1 && o.data[0] == 'x' && theme_map_pending(&m));   /* the partial escape is held, not written */
    theme_map_bytes(&m, "2mz", 3, &o);
    CHECK(!theme_map_pending(&m) && !strcmp(o.data, "x\x1b[38;2;163;190;140mz"));   /* 32 is the green */
    sb_free(&o);
    /* an ESC that is not a CSI goes out as it came */
    a = map_all("\x1b" "7x\x1b" "8");
    CHECK_STR(a, "\x1b" "7x\x1b" "8");
    free(a);
    /* the basic codes of the theme, for both screens: 30..37 and the bright 9x */
    unsigned rgb = 0;
    CHECK(theme_fg_code(32, &rgb) && rgb == g_theme->ok && theme_fg_code(92, &rgb) && rgb == g_theme->ok);
    CHECK(theme_fg_code(90, &rgb) && rgb == g_theme->gray && !theme_fg_code(40, &rgb));   /* a background is not a fg */
    CHECK(th_fg(0x5FD75Fu) && !strcmp(th_fg(0x5FD75Fu), "\x1b[38;2;95;215;95m"));
    CHECK(!strcmp(th_bg(0x151922u), "\x1b[48;2;21;25;34m"));
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.temperature = -1; g_cfg.think = -1; g_cfg.max_iters = 10; g_cfg.interactive = false;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "sbuf", test_sbuf }, { "util", test_util }, { "cmd head", test_cmd_head }, { "markdown", test_md }, { "highlight", test_highlight }, { "git branch", test_gitbranch },
        { "text_tool_calls", test_text_tool_calls }, { "tools", test_tools }, { "write preview", test_write_preview }, { "modes", test_modes },
        { "queue", test_queue }, { "skills", test_skills }, { "files", test_files }, { "http", test_http },
        { "web", test_web }, { "model_info", test_model_info }, { "transcript", test_transcript },
        { "effort", test_effort }, { "suggest", test_suggest }, { "advisor", test_advisor }, { "provider", test_provider }, { "hosts", test_hosts }, { "base64", test_base64 }, { "gzip round trip", test_gzip_round_trip }, { "gzip verdicts", test_gzip_judge }, { "gzip setting", test_gzip_setting }, { "orchestrate", test_orchestrate }, { "goal", test_goal }, { "agent log", test_agent_log }, { "strip", test_strip }, { "paste", test_paste }, { "paste more", test_paste_more }, { "wellformed", test_wellformed }, { "copy key", test_copy_key }, { "notcurses pure", test_nc_pure }, { "word wrap", test_wrap }, { "menu rows", test_menu_rows }, { "themes", test_theme },
    };
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        int before = g_fail;
        fprintf(stderr, "test %-16s ", tests[i].name);
        tests[i].fn();
        fprintf(stderr, "%s\n", g_fail == before ? "ok" : "FAILED");
    }
    fprintf(stderr, "%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
