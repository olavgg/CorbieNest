#define _GNU_SOURCE 1   /* pipe2 */
/* Model Context Protocol servers over stdio (see DOCUMENTATION.md, "MCP servers").
 *
 * A server is a program the user names: corbienest starts it (no shell: the command and its arguments as written),
 * talks JSON-RPC 2.0 with one message per line on its standard input and output, and offers its tools to the model as
 * "mcp__SERVER__TOOL". The servers are kept in the config directory, never in the project tree: a cloned repository
 * must not start processes. mcp/<hash>.json is this project's; mcp/global.json is the user's own, for every project.
 * A name in both is the project's. */
#include "common.h"
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char **environ;

#define MCP_MAX_SERVERS 8
#define MCP_MAX_TOOLS 64
#define MCP_CONNECT_TIMEOUT 5      /* s for a server to answer initialize and tools/list */
#define MCP_CALL_TIMEOUT 120       /* s for one tools/call */
#define MCP_LINE_MAX (16 * 1024 * 1024)
#define MCP_PROTOCOL "2024-11-05"

typedef struct {
    char *name, *command, *origin, *status;   /* origin: "project" or "global"; status: what /mcp says */
    char **args; int nargs;
    char **env;  int nenv;                    /* "KEY=VALUE" */
    pid_t pid;
    int wfd, rfd;
    sbuf rbuf;
    bool running, dead;
    int ntools, next_id;
} mcp_server;

typedef struct {
    char *exposed;          /* mcp__server__tool, as the model sees it */
    mcp_server *srv;
    char *tool;             /* the server's own name for it */
    char *desc;
    cJSON *schema;
} mcp_tool;

static mcp_server g_srv[MCP_MAX_SERVERS];
static int g_nsrv = 0;
static mcp_tool g_tool[MCP_MAX_TOOLS];
static int g_ntool = 0;
static volatile pid_t g_mcp_pgid[MCP_MAX_SERVERS];   /* the groups still running, for the signal handler */
static bool g_mcp_atexit = false;

/* ---------- names ---------- */
static unsigned long long fnv(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
/* The name a tool is offered under. The providers take [a-zA-Z0-9_-]{1,64}: any other character becomes _, and a name
 * longer than 64 is cut and given a short hash of the whole, so two long names do not meet. Malloc'd. */
char *mcp_tool_name(const char *server, const char *tool) {
    char full[600]; snprintf(full, sizeof full, "mcp__%s__%s", server, tool);
    char out[80]; size_t n = 0;
    for (const char *p = full; *p && n < sizeof out - 1; p++) out[n++] = (isalnum((unsigned char)*p) || *p == '_' || *p == '-') ? *p : '_';
    out[n] = 0;
    if (n > 64) { char tag[12]; snprintf(tag, sizeof tag, "_%08llx", fnv(full) & 0xffffffffULL); size_t keep = 64 - strlen(tag); out[keep] = 0; strcat(out, tag); }
    return xstrdup(out);
}
bool mcp_is_tool(const char *name) { return name && !strncmp(name, "mcp__", 5); }

/* The text of a tools/call result: its text blocks joined; other kinds are named. Malloc'd. */
char *mcp_content_text(cJSON *result) {
    sbuf b; sb_init(&b);
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;
    cJSON *blk;
    cJSON_ArrayForEach(blk, content) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(blk, "type");
        const char *ty = cJSON_IsString(t) ? t->valuestring : "";
        if (b.len) sb_putc(&b, '\n');
        if (!strcmp(ty, "text")) { cJSON *tx = cJSON_GetObjectItemCaseSensitive(blk, "text"); if (cJSON_IsString(tx)) sb_puts(&b, tx->valuestring); }
        else if (!strcmp(ty, "image")) sb_puts(&b, "[image]");
        else if (!strcmp(ty, "audio")) sb_puts(&b, "[audio]");
        else if (!strcmp(ty, "resource") || !strcmp(ty, "resource_link")) sb_puts(&b, "[resource]");
        else sb_printf(&b, "[%s]", *ty ? ty : "content");
    }
    if (!b.len && result && !content) { cJSON *sc = cJSON_GetObjectItemCaseSensitive(result, "structuredContent"); if (sc) { char *j = cJSON_PrintUnformatted(sc); sb_puts(&b, j ? j : ""); free(j); } }
    return sb_detach(&b);
}

/* ---------- the config files ---------- */
static const char *mcp_dir(void) { static char d[1200]; snprintf(d, sizeof d, "%s/mcp", config_dir()); return d; }
static char *mcp_project_path(void) {
    char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    char *p = xmalloc(1400); snprintf(p, 1400, "%s/%016llx.json", mcp_dir(), fnv(cwd));
    return p;
}
static char *mcp_global_path(void) { char *p = xmalloc(1400); snprintf(p, 1400, "%s/global.json", mcp_dir()); return p; }

/* Reads one file of servers into the table, each tagged with its origin; a name already there is skipped. */
static void load_file(const char *path, const char *origin) {
    size_t n; char *d = read_whole_file(path, &n, 1024 * 1024);
    if (!d) return;
    cJSON *o = cJSON_Parse(d); free(d);
    cJSON *srv = o ? cJSON_GetObjectItemCaseSensitive(o, "servers") : NULL;
    if (!cJSON_IsObject(srv)) srv = NULL;
    cJSON *s;
    cJSON_ArrayForEach(s, srv) {
        if (g_nsrv >= MCP_MAX_SERVERS) break;
        if (!s->string || !cJSON_IsObject(s)) continue;
        bool dup = false; for (int i = 0; i < g_nsrv; i++) if (!strcmp(g_srv[i].name, s->string)) dup = true;
        cJSON *cmd = cJSON_GetObjectItemCaseSensitive(s, "command");
        if (dup || !cJSON_IsString(cmd) || !cmd->valuestring[0] || !s->string[0]) continue;
        mcp_server *m = &g_srv[g_nsrv++];
        memset(m, 0, sizeof *m);
        m->wfd = m->rfd = -1;
        m->name = xstrdup(s->string); m->command = xstrdup(cmd->valuestring); m->origin = xstrdup(origin);
        cJSON *args = cJSON_GetObjectItemCaseSensitive(s, "args"), *a;
        cJSON_ArrayForEach(a, args) if (cJSON_IsString(a)) { m->args = xrealloc(m->args, sizeof(char*) * (size_t)(m->nargs + 1)); m->args[m->nargs++] = xstrdup(a->valuestring); }
        cJSON *env = cJSON_GetObjectItemCaseSensitive(s, "env"), *e;
        cJSON_ArrayForEach(e, env) if (cJSON_IsString(e)) {
            sbuf kv; sb_init(&kv); sb_printf(&kv, "%s=%s", e->string, e->valuestring);
            m->env = xrealloc(m->env, sizeof(char*) * (size_t)(m->nenv + 1)); m->env[m->nenv++] = sb_detach(&kv);
        }
        m->status = xstrdup("not started");
    }
    cJSON_Delete(o);
}
static void mcp_forget_servers(void) {
    for (int i = 0; i < g_nsrv; i++) {
        mcp_server *m = &g_srv[i];
        free(m->name); free(m->command); free(m->origin); free(m->status);
        for (int k = 0; k < m->nargs; k++) free(m->args[k]);
        for (int k = 0; k < m->nenv; k++) free(m->env[k]);
        free(m->args); free(m->env); sb_free(&m->rbuf);
        memset(m, 0, sizeof *m);
    }
    g_nsrv = 0;
}
static void mcp_forget_tools(void) {
    for (int i = 0; i < g_ntool; i++) { free(g_tool[i].exposed); free(g_tool[i].tool); free(g_tool[i].desc); cJSON_Delete(g_tool[i].schema); }
    g_ntool = 0;
}

/* Writes a server into the project's file, or the global one; false if the name is taken there. */
bool mcp_config_add(const char *name, const char *command, char **args, int nargs, bool global) {
    char *path = global ? mcp_global_path() : mcp_project_path();
    size_t n; char *d = read_whole_file(path, &n, 1024 * 1024);
    cJSON *o = d ? cJSON_Parse(d) : NULL; free(d);
    if (!o) o = cJSON_CreateObject();
    cJSON *srv = cJSON_GetObjectItemCaseSensitive(o, "servers");
    if (!cJSON_IsObject(srv)) { cJSON_DeleteItemFromObjectCaseSensitive(o, "servers"); srv = cJSON_AddObjectToObject(o, "servers"); }
    if (cJSON_GetObjectItemCaseSensitive(srv, name)) { cJSON_Delete(o); free(path); return false; }
    cJSON *s = cJSON_AddObjectToObject(srv, name);
    cJSON_AddStringToObject(s, "command", command);
    cJSON *as = cJSON_AddArrayToObject(s, "args");
    for (int i = 0; i < nargs; i++) cJSON_AddItemToArray(as, cJSON_CreateString(args[i]));
    char *txt = cJSON_Print(o);
    mkdir_p(mcp_dir());
    bool ok = txt && write_whole_file_atomic(path, txt, strlen(txt));
    free(txt); cJSON_Delete(o); free(path);
    return ok;
}
/* Takes a server out of the project's file, or the global one. */
bool mcp_config_remove(const char *name, bool global) {
    char *path = global ? mcp_global_path() : mcp_project_path();
    size_t n; char *d = read_whole_file(path, &n, 1024 * 1024);
    cJSON *o = d ? cJSON_Parse(d) : NULL; free(d);
    bool ok = false;
    cJSON *srv = o ? cJSON_GetObjectItemCaseSensitive(o, "servers") : NULL;
    if (cJSON_IsObject(srv) && cJSON_GetObjectItemCaseSensitive(srv, name)) {
        cJSON_DeleteItemFromObjectCaseSensitive(srv, name);
        char *txt = cJSON_Print(o);
        ok = txt && write_whole_file_atomic(path, txt, strlen(txt));
        free(txt);
    }
    cJSON_Delete(o); free(path);
    return ok;
}

/* ---------- processes ---------- */
static void kill_group(pid_t pgid, int sig) { if (pgid > 0) kill(-pgid, sig); }
static void reap(mcp_server *m) {
    if (!m->running) return;
    int st; pid_t r = waitpid(m->pid, &st, WNOHANG);   /* this server's pid only: never -1, the jobs' exits are not ours */
    if (r == 0) return;
    m->running = false; m->dead = true;
    if (m->wfd >= 0) { close(m->wfd); m->wfd = -1; }
    if (m->rfd >= 0) { close(m->rfd); m->rfd = -1; }
    for (int i = 0; i < g_nsrv; i++) if (&g_srv[i] == m) g_mcp_pgid[i] = 0;
}
/* A pipe whose ends do not survive an exec (so no other child of corbienest holds a server's stdin open). */
static bool cloexec_pipe(int p[2]) {
#ifdef __linux__
    return pipe2(p, O_CLOEXEC) == 0;
#else
    if (pipe(p) != 0) return false;
    fcntl(p[0], F_SETFD, FD_CLOEXEC); fcntl(p[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}
/* The program to run: the command as written when it has a /, else the first PATH entry where it is executable. */
static char *resolve_command(const char *cmd) {
    if (strchr(cmd, '/')) return xstrdup(cmd);
    const char *path = getenv("PATH"); if (!path) path = "/usr/bin:/bin";
    char *pc = xstrdup(path);
    for (char *dir = strtok(pc, ":"); dir; dir = strtok(NULL, ":")) {
        char p[PATH_MAX + 64]; snprintf(p, sizeof p, "%s/%s", *dir ? dir : ".", cmd);
        if (!access(p, X_OK)) { free(pc); return xstrdup(p); }
    }
    free(pc);
    return NULL;
}
static bool spawn(mcp_server *m) {
    char *prog = resolve_command(m->command);
    if (!prog) { free(m->status); m->status = xstrdup("command not found on PATH"); m->dead = true; return false; }
    /* everything the child needs is built here: after fork only async-signal-safe calls are made (notcurses has a thread) */
    char **argv = xmalloc(sizeof(char*) * (size_t)(m->nargs + 2));
    argv[0] = m->command;
    for (int i = 0; i < m->nargs; i++) argv[i + 1] = m->args[i];
    argv[m->nargs + 1] = NULL;
    int keep = 0; for (char **e = environ; *e; e++) keep++;
    char **envp = xmalloc(sizeof(char*) * (size_t)(keep + m->nenv + 1)); int ne = 0;
    for (char **e = environ; *e; e++) {   /* the environment, except the keys the server sets itself */
        bool over = false;
        for (int k = 0; k < m->nenv; k++) { size_t kl = strcspn(m->env[k], "="); if (!strncmp(*e, m->env[k], kl) && (*e)[kl] == '=') over = true; }
        if (!over) envp[ne++] = *e;
    }
    for (int k = 0; k < m->nenv; k++) envp[ne++] = m->env[k];
    envp[ne] = NULL;
    int in[2], out[2];
    if (!cloexec_pipe(in) || !cloexec_pipe(out)) { free(prog); free(argv); free(envp); m->status = xstrdup("cannot make pipes"); m->dead = true; return false; }
#ifdef __linux__   /* read only by the PDEATHSIG check below, which is Linux's */
    pid_t parent = getpid();
#endif
    pid_t pid = fork();
    if (pid < 0) { free(prog); free(argv); free(envp); close(in[0]); close(in[1]); close(out[0]); close(out[1]); m->status = xstrdup("fork failed"); m->dead = true; return false; }
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(1);
#endif
        dup2(in[0], STDIN_FILENO); dup2(out[1], STDOUT_FILENO);
        int err = open("/dev/null", O_WRONLY); if (err >= 0) dup2(err, STDERR_FILENO);
        execve(prog, argv, envp);
        _exit(127);
    }
    setpgid(pid, pid);
    close(in[0]); close(out[1]);
    free(prog); free(argv); free(envp);
    m->pid = pid; m->wfd = in[1]; m->rfd = out[0];
    fcntl(m->wfd, F_SETFL, fcntl(m->wfd, F_GETFL) | O_NONBLOCK);
    fcntl(m->rfd, F_SETFL, fcntl(m->rfd, F_GETFL) | O_NONBLOCK);
    m->running = true; m->dead = false;
    for (int i = 0; i < g_nsrv; i++) if (&g_srv[i] == m) g_mcp_pgid[i] = pid;
    return true;
}

/* ---------- JSON-RPC over the pipes ---------- */
static double mono(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9; }
/* One line to the server. False if it is gone (the server is marked dead), or if it would not take the line in time
 * (then it is marked dead too: half a line on its input would corrupt every later message). */
static bool send_line(mcp_server *m, const char *json, int timeout_s) {
    size_t n = strlen(json);
    char *buf = xmalloc(n + 2);
    memcpy(buf, json, n); buf[n] = '\n'; buf[n + 1] = 0;
    size_t len = n + 1, off = 0;
    double until = mono() + timeout_s;
    bool ok = false;
    while (off < len) {
        ssize_t w = write(m->wfd, buf + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (mono() > until) { if (off) m->dead = true; break; }
            struct pollfd pf = { .fd = m->wfd, .events = POLLOUT };
            poll(&pf, 1, 100);
            if (pf.revents & (POLLERR | POLLHUP)) { m->dead = true; break; }
            continue;
        }
        m->dead = true; break;   /* EPIPE and the rest: the server has gone */
    }
    ok = off == len;
    free(buf);
    return ok;
}
/* Reads until the response with this id (its result or error), or until the deadline, an interrupt or the server's end.
 * Lines that are not JSON-RPC (a banner on stdout) are skipped; the server's own requests are answered; notifications
 * are ignored. A cancel is sent when the user interrupts. */
static cJSON *await(mcp_server *m, int id, int timeout_s, bool *interrupted) {
    *interrupted = false;
    double until = mono() + timeout_s;
    while (!m->dead) {
        /* a complete line already read? */
        char *nl;
        while ((nl = memchr(m->rbuf.data ? m->rbuf.data : "", '\n', m->rbuf.len))) {
            size_t ll = (size_t)(nl - m->rbuf.data);
            char *line = xstrndup(m->rbuf.data, ll);
            size_t rest = m->rbuf.len - ll - 1;
            memmove(m->rbuf.data, nl + 1, rest); m->rbuf.len = rest; m->rbuf.data[rest] = 0;
            cJSON *o = cJSON_Parse(line);
            free(line);
            if (!o || !cJSON_IsObject(o)) { cJSON_Delete(o); continue; }
            cJSON *mid = cJSON_GetObjectItemCaseSensitive(o, "id"), *meth = cJSON_GetObjectItemCaseSensitive(o, "method");
            if (cJSON_IsString(meth) && mid) {   /* the server asks us something: ping is answered, the rest is refused */
                cJSON *r = cJSON_CreateObject();
                cJSON_AddItemReferenceToObject(r, "id", mid);
                if (!strcmp(meth->valuestring, "ping")) cJSON_AddObjectToObject(r, "result");
                else { cJSON *e = cJSON_AddObjectToObject(r, "error"); cJSON_AddNumberToObject(e, "code", -32601); cJSON_AddStringToObject(e, "message", "not supported by corbienest"); }
                cJSON_AddStringToObject(r, "jsonrpc", "2.0");
                char *t = cJSON_PrintUnformatted(r); cJSON_Delete(r);
                if (t) send_line(m, t, 5);
                free(t); cJSON_Delete(o);
                continue;
            }
            if (cJSON_IsNumber(mid) && mid->valuedouble == id && (cJSON_GetObjectItemCaseSensitive(o, "result") || cJSON_GetObjectItemCaseSensitive(o, "error")))
                return o;
            cJSON_Delete(o);   /* a notification, or a late answer to a call that was cancelled */
        }
        if (m->rbuf.len > MCP_LINE_MAX) { m->dead = true; break; }   /* a line this long is not a server speaking the protocol */
        double left = until - mono();
        if (left <= 0) break;
        if (term_poll_interrupt()) {
            *interrupted = true;
            char c[160]; snprintf(c, sizeof c, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":%d,\"reason\":\"stopped by the user\"}}", id);
            send_line(m, c, 1);
            break;
        }
        struct pollfd pf = { .fd = m->rfd, .events = POLLIN };
        int pr = poll(&pf, 1, left < 0.1 ? (int)(left * 1000) + 1 : 100);
        if (pr <= 0) continue;
        char buf[8192];
        ssize_t r = read(m->rfd, buf, sizeof buf);
        if (r == 0) { m->dead = true; break; }
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) continue; m->dead = true; break; }
        if (!m->rbuf.data) sb_init(&m->rbuf);
        sb_append(&m->rbuf, buf, (size_t)r);
    }
    return NULL;
}
/* One request and its answer. NULL on failure: *interrupted tells whether the user stopped it. */
static cJSON *request(mcp_server *m, const char *method, cJSON *params, int timeout_s, bool *interrupted) {
    int id = ++m->next_id;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "method", method);
    if (params) cJSON_AddItemToObject(o, "params", params);
    char *t = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    *interrupted = false;
    if (!t || !send_line(m, t, 5)) { free(t); return NULL; }
    free(t);
    cJSON *ans = await(m, id, timeout_s, interrupted);
    return ans;
}
static void notify(mcp_server *m, const char *method) {
    char t[160]; snprintf(t, sizeof t, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}", method);
    send_line(m, t, 2);
}

/* Starts a server and reads its tools. Its status says how it went. */
static void connect_one(mcp_server *m) {
    bool intr = false;
    cJSON *params = cJSON_CreateObject(), *ci = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTOCOL);
    cJSON_AddObjectToObject(params, "capabilities");
    cJSON_AddStringToObject(ci, "name", "corbienest"); cJSON_AddStringToObject(ci, "version", CORBIE_VERSION);
    cJSON *ans = request(m, "initialize", params, MCP_CONNECT_TIMEOUT, &intr);
    if (!ans || !cJSON_GetObjectItemCaseSensitive(ans, "result")) {
        cJSON_Delete(ans);
        free(m->status);
        m->status = xstrdup(m->dead ? "exited before it answered" : intr ? "stopped" : "did not answer initialize");
        reap(m);
        return;
    }
    cJSON_Delete(ans);
    notify(m, "notifications/initialized");
    int added = 0;
    bool failed = false;
    char *cursor = NULL;
    for (int page = 0; page < 20; page++) {
        cJSON *p = cJSON_CreateObject();
        if (cursor) cJSON_AddStringToObject(p, "cursor", cursor);
        ans = request(m, "tools/list", p, MCP_CONNECT_TIMEOUT, &intr);
        if (!ans) { free(m->status); m->status = xstrdup(m->dead ? "exited while listing its tools" : "did not list its tools"); failed = true; reap(m); break; }
        cJSON *res = cJSON_GetObjectItemCaseSensitive(ans, "result");
        cJSON *tools = res ? cJSON_GetObjectItemCaseSensitive(res, "tools") : NULL, *t;
        cJSON_ArrayForEach(t, tools) {
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
            if (!cJSON_IsString(nm) || !nm->valuestring[0] || g_ntool >= MCP_MAX_TOOLS) continue;
            mcp_tool *mt = &g_tool[g_ntool];
            mt->exposed = mcp_tool_name(m->name, nm->valuestring);
            for (int i = 0; i < g_ntool; i++) if (!strcmp(g_tool[i].exposed, mt->exposed)) { free(mt->exposed); mt->exposed = NULL; break; }
            if (!mt->exposed) continue;
            cJSON *d = cJSON_GetObjectItemCaseSensitive(t, "description"), *sch = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
            mt->srv = m; mt->tool = xstrdup(nm->valuestring);
            sbuf db; sb_init(&db); sb_printf(&db, "MCP server %s: %s", m->name, cJSON_IsString(d) ? d->valuestring : nm->valuestring);
            mt->desc = sb_detach(&db);
            if (cJSON_IsObject(sch) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(sch, "type"))) mt->schema = cJSON_Duplicate(sch, 1);
            else { mt->schema = cJSON_CreateObject(); cJSON_AddStringToObject(mt->schema, "type", "object"); cJSON_AddObjectToObject(mt->schema, "properties"); }
            g_ntool++; added++;
        }
        free(cursor); cursor = NULL;
        cJSON *nc = res ? cJSON_GetObjectItemCaseSensitive(res, "nextCursor") : NULL;
        if (cJSON_IsString(nc) && nc->valuestring[0]) cursor = xstrdup(nc->valuestring);
        cJSON_Delete(ans);
        if (!cursor) break;
    }
    free(cursor);
    m->ntools = added;
    if (failed || m->dead || (m->status && !strncmp(m->status, "exited", 6))) return;
    free(m->status);
    char s[80]; snprintf(s, sizeof s, "connected: %d tool%s", added, added == 1 ? "" : "s");
    m->status = xstrdup(s);
}

/* Starts every configured server: all spawned first, then each one's handshake. */
static void mcp_start_all(void) {
    for (int i = 0; i < g_nsrv; i++) if (!g_srv[i].running && !g_srv[i].dead) spawn(&g_srv[i]);
    for (int i = 0; i < g_nsrv; i++) if (g_srv[i].running) connect_one(&g_srv[i]);
}
static void mcp_stop_all(void) {
    for (int i = 0; i < g_nsrv; i++) if (g_srv[i].running) kill_group(g_srv[i].pid, SIGTERM);
    usleep(100000);
    for (int i = 0; i < g_nsrv; i++) if (g_srv[i].running) { kill_group(g_srv[i].pid, SIGKILL); int st; waitpid(g_srv[i].pid, &st, 0); g_srv[i].running = false; }
    for (int i = 0; i < g_nsrv; i++) { mcp_server *m = &g_srv[i]; if (m->wfd >= 0) close(m->wfd); if (m->rfd >= 0) close(m->rfd); m->wfd = m->rfd = -1; g_mcp_pgid[i] = 0; }
    mcp_forget_tools();
}

/* ---------- the public side ---------- */
/* Reads the servers (the project's and the user's) and starts them. Called once at start-up, and again by /mcp. */
void mcp_load(void) {
    mcp_stop_all();
    mcp_forget_servers();
    load_file(mcp_project_path(), "project");
    load_file(mcp_global_path(), "global");
    mcp_start_all();
}
/* Stops every server's process group: at exit, and before a re-exec. Also run from the signal handler (see below). */
void mcp_shutdown(void) { mcp_stop_all(); }
void mcp_kill_signal(void) {   /* async-signal-safe: the groups only */
    for (int i = 0; i < MCP_MAX_SERVERS; i++) { pid_t g = g_mcp_pgid[i]; if (g > 0) kill(-g, SIGKILL); }
}
void mcp_init(void) { if (!g_mcp_atexit) { atexit(mcp_shutdown); g_mcp_atexit = true; } }

/* The tool definitions of the connected servers, added to a tools array (the shape tools_definitions() gives). */
void mcp_add_tools(cJSON *arr) {
    for (int i = 0; i < g_ntool; i++) {
        mcp_tool *t = &g_tool[i];
        if (!t->srv || !t->srv->running) continue;
        cJSON *tool = cJSON_CreateObject(), *fn = cJSON_AddObjectToObject(tool, "function");
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(fn, "name", t->exposed);
        cJSON_AddStringToObject(fn, "description", t->desc);
        cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(t->schema, 1));
        cJSON_AddItemToArray(arr, tool);
    }
}
/* One call of a server's tool, with its arguments. The result's text goes to out; the status says whether it failed. */
tool_status mcp_call(const char *exposed, cJSON *args, sbuf *out) {
    mcp_tool *t = NULL;
    for (int i = 0; i < g_ntool; i++) if (!strcmp(g_tool[i].exposed, exposed)) t = &g_tool[i];
    if (!t) { sb_printf(out, "error: no MCP tool %s is connected", exposed); return TOOL_ERROR; }
    mcp_server *m = t->srv;
    reap(m);
    if (!m->running) { sb_printf(out, "error: MCP server %s has exited (/mcp shows it; /mcp reconnect %s starts it again)", m->name, m->name); return TOOL_ERROR; }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", t->tool);
    cJSON_AddItemToObject(p, "arguments", args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject());
    bool intr = false;
    cJSON *ans = request(m, "tools/call", p, MCP_CALL_TIMEOUT, &intr);
    if (!ans) {
        if (intr) { sb_puts(out, "(stopped by the user)"); return TOOL_ERROR; }
        if (m->dead) { reap(m); sb_printf(out, "error: MCP server %s exited during the call", m->name); return TOOL_ERROR; }
        sb_printf(out, "error: MCP server %s did not answer within %d s", m->name, MCP_CALL_TIMEOUT);
        return TOOL_ERROR;
    }
    cJSON *err = cJSON_GetObjectItemCaseSensitive(ans, "error");
    if (err) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        sb_printf(out, "error: %s", cJSON_IsString(msg) ? msg->valuestring : "the server refused the call");
        cJSON_Delete(ans);
        return TOOL_ERROR;
    }
    cJSON *res = cJSON_GetObjectItemCaseSensitive(ans, "result");
    char *text = mcp_content_text(res);
    sb_puts(out, text ? text : "");
    bool failed = res && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "isError"));
    free(text);
    cJSON_Delete(ans);
    return failed ? TOOL_ERROR : TOOL_OK;
}

/* /mcp: the servers, where each is kept, and what it came to. */
void mcp_list(void) {
    if (!g_nsrv) { printf(C_DIM "  no MCP servers. /mcp add NAME COMMAND [ARGS…] (or add -g for every project)" C_RESET "\n"); return; }
    for (int i = 0; i < g_nsrv; i++) {
        mcp_server *m = &g_srv[i];
        reap(m);
        const char *st = m->running ? (m->status ? m->status : "running") : (m->status ? m->status : "not running");
        printf("  %d. " C_BOLD "%s" C_RESET " %s  " C_DIM "%s · %s" C_RESET "\n", i + 1, m->name, m->command, m->origin, st);
    }
    printf(C_DIM "  /mcp remove NAME · /mcp reconnect NAME" C_RESET "\n");
}
int mcp_count(void) { return g_nsrv; }
const char *mcp_server_name(int i) { return i >= 0 && i < g_nsrv ? g_srv[i].name : NULL; }
