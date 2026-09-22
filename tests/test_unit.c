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

    /* what the host setting becomes: Ollama's port when http has none, the scheme's own for https */
    char *u;
    u = http_url("http://127.0.0.1:11434", "/api/chat"); CHECK_STR(u, "http://127.0.0.1:11434/api/chat"); free(u);
    u = http_url("localhost", "/api/tags"); CHECK_STR(u, "http://localhost:11434/api/tags"); free(u);
    u = http_url("http://gpu-box", "/api/tags"); CHECK_STR(u, "http://gpu-box:11434/api/tags"); free(u);
    u = http_url("0.0.0.0:8080", "/x"); CHECK_STR(u, "http://0.0.0.0:8080/x"); free(u);
    u = http_url(":11500", "/x"); CHECK_STR(u, "http://127.0.0.1:11500/x"); free(u);
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
    CHECK(advisor_guidance_parse("strong") == GUIDANCE_STRONG && advisor_guidance_parse("MAX") == GUIDANCE_MAX && advisor_guidance_parse("ultra") == -1);
    CHECK(ADVISOR_GUIDANCE[GUIDANCE_NORMAL].uses == 3 && !ADVISOR_GUIDANCE[GUIDANCE_NORMAL].review && ADVISOR_GUIDANCE[GUIDANCE_STRONG].review && ADVISOR_GUIDANCE[GUIDANCE_MAX].check_first_edit);
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

static void test_provider(void) {
    const char *model = NULL;
    const provider_def *p = provider_find("xai:grok-4.7", &model);
    CHECK(p && !strcmp(p->name, "xai") && p->style == PROVIDER_CHAT_COMPLETIONS); CHECK_STR(model, "grok-4.7");
    p = provider_find("grok:grok-4.7", &model); CHECK(p && !strcmp(p->name, "xai"));
    p = provider_find("claude:claude-opus-5", &model); CHECK(p && p->style == PROVIDER_MESSAGES); CHECK_STR(model, "claude-opus-5");
    CHECK(provider_find("OpenAI:gpt-5.2", NULL) != NULL);
    CHECK(!provider_find("qwen3:32b", NULL) && !provider_find("gpt-oss:120b-cloud", NULL) && !provider_find("xai:", NULL) && !provider_find(NULL, NULL));
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
    model_info opus = mi;
    const char *HAIKU = "{\"id\":\"claude-haiku-4-5\",\"max_input_tokens\":200000,\"capabilities\":{"
        "\"thinking\":{\"supported\":true,\"types\":{\"enabled\":{\"supported\":true},\"adaptive\":{\"supported\":false}}},\"effort\":{\"supported\":false}}}";
    provider_parse_model("anthropic:claude-haiku-4-5", HAIKU, &mi);
    CHECK(mi.think_budget && !mi.think_adaptive && mi.n_think_levels == 0 && mi.think_off && mi.think_on && !strcmp(mi.think_default, "off"));
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
    CHECK(provider_model_info("openai:gpt-5.2", &mi, err, sizeof err) == -1 && strstr(err, "OPENAI_BASE_URL is plain http:// to another machine (proxy.lan)") && strstr(err, "unencrypted"));
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
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.temperature = -1; g_cfg.think = -1; g_cfg.max_iters = 10; g_cfg.interactive = false;
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "sbuf", test_sbuf }, { "util", test_util }, { "markdown", test_md },
        { "text_tool_calls", test_text_tool_calls }, { "tools", test_tools }, { "modes", test_modes },
        { "queue", test_queue }, { "skills", test_skills }, { "http", test_http },
        { "web", test_web }, { "model_info", test_model_info }, { "transcript", test_transcript },
        { "effort", test_effort }, { "advisor", test_advisor }, { "provider", test_provider },
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
