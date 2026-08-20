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

    /* tool definitions are well-formed */
    cJSON *defs = tools_definitions();
    CHECK(cJSON_GetArraySize(defs) == 7);
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
    p = start_server("HTTP/1.1 404 Not Found\r\nContent-Length: 17\r\n\r\n{\"error\":\"nope\"}", &port, &reqpath);
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
    /* https unsupported */
    rc = http_request("https://example.com", "GET", "/", NULL, NULL, NULL, NULL, &res);
    CHECK(rc < 0);
    unlink(reqpath);
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
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
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
    #define KT(str) term_keys_to_text((const unsigned char *)(str), sizeof(str) - 1)
    char *t;
    t = KT("hello world"); CHECK_STR(t, "hello world"); free(t);
    t = KT("  padded  \n"); CHECK_STR(t, "padded"); free(t);
    t = KT("helo\x7flo"); CHECK_STR(t, "hello"); free(t);                       /* backspace */
    t = KT("caf\xc3\xa9\x7f!"); CHECK_STR(t, "caf!"); free(t);               /* backspace removes a whole UTF-8 char */
    t = KT("wrong\x15right"); CHECK_STR(t, "right"); free(t);                  /* Ctrl-U */
    t = KT("keep this bad\x17good"); CHECK_STR(t, "keep this good"); free(t);  /* Ctrl-W */
    t = KT("a\x1b[Db\x1b[1;5Cc\x1bOAd"); CHECK_STR(t, "abcd"); free(t);      /* arrows / CSI / SS3 dropped */
    t = KT("line1\x1b\rline2\nline3"); CHECK_STR(t, "line1\nline2\nline3"); free(t);   /* Alt+Enter, Ctrl-J */
    t = KT("\x1b[200~pasted\r\nlines\x1b[201~"); CHECK_STR(t, "pasted\nlines"); free(t);   /* paste brackets */
    t = KT("\x1b" "fskip"); CHECK_STR(t, "skip"); free(t);                        /* Alt+f ignored */
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
    term_queue_clear();
    /* "new since the mark": what stops the work in flight. Only plain messages count, and
       only ones queued after main.c last marked the queue seen. */
    term_queue_mark(); CHECK(!term_queue_new());
    term_queue_push("/status"); CHECK(!term_queue_new());
    term_queue_push("look at this too"); CHECK(term_queue_new());
    term_queue_mark(); CHECK(!term_queue_new());          /* still queued, but no longer new */
    t = term_queue_pop_plain(); free(t);
    CHECK(!term_queue_new());
    term_queue_clear();
    #undef KT
    /* prompt history keeps the latest 100 queries */
    for (int i = 0; i < 150; i++) { char l[32]; snprintf(l, sizeof l, "query %d", i); hist_add(l); }
    CHECK(hist_count() == 100);
    CHECK_STR(hist_get(0), "query 50"); CHECK_STR(hist_get(99), "query 149"); CHECK(hist_get(100) == NULL);
    hist_add("query 149"); CHECK(hist_count() == 100);   /* consecutive duplicates collapse */
}

/* ---------- skills ---------- */
static void test_skills(void) {
    char dir[] = "/tmp/crowtest_s_XXXXXX"; CHECK(mkdtemp(dir) != NULL);
    char old[4096]; CHECK(getcwd(old, sizeof old) != NULL); CHECK(chdir(dir) == 0);
    setenv("XDG_CONFIG_HOME", dir, 1);   /* keep user skills out of the picture */
    CHECK(skills_load() == 0);
    CHECK(mkdir_p(".corbienest/skills/rev") == 0);
    const char *body = "---\nname: rev\ndescription: \"Review things\"\n---\n\nReview: $ARGUMENTS\nEnd.\n";
    CHECK(write_whole_file(".corbienest/skills/rev/SKILL.md", body, strlen(body)) == 0);
    const char *flat = "No frontmatter here.\n";
    CHECK(write_whole_file(".corbienest/skills/plain.md", flat, strlen(flat)) == 0);
    CHECK(skills_load() == 2);
    const skill_t *s = skill_find("/rev"); CHECK(s != NULL);
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
    CHECK(skills_load() == 3);
    char *sec = skills_prompt_section(); CHECK(sec && strstr(sec, "/new-one") && strstr(sec, "written in C")); free(sec);
    CHECK(chdir(old) == 0);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.temperature = -1; g_cfg.think = -1; g_cfg.max_iters = 10; g_cfg.interactive = false;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "sbuf", test_sbuf }, { "util", test_util }, { "markdown", test_md },
        { "text_tool_calls", test_text_tool_calls }, { "tools", test_tools }, { "modes", test_modes },
        { "queue", test_queue }, { "skills", test_skills }, { "http", test_http },
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
