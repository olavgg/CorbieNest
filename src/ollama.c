/* Ollama /api/chat streaming client. */
#define _GNU_SOURCE
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    md_state md;
    sbuf content;
    sbuf thinking;
    cJSON *tool_calls;      /* array or NULL */
    chat_stats *stats;
    bool got_content;       /* printed any visible content yet */
    bool thinking_header;   /* printed thinking header */
    bool spinner_shown;
    int  think_tokens;
    int  out_chunks;        /* streamed chunks so far (≈ tokens), for the live status bar */
    char error[512];
    struct timeval t0;
    int spin_i;
} chat_ctx;

static double elapsed(struct timeval *t0) {
    struct timeval t; gettimeofday(&t, NULL);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_usec - t0->tv_usec) / 1e6;
}

static const char *SPIN[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };

bool ollama_quiet = false;

static void spinner_clear(chat_ctx *c) {
    if (c->spinner_shown) { fputs("\r\x1b[2K", stdout); fflush(stdout); c->spinner_shown = false; }
}

static void on_idle(void *ud) {
    chat_ctx *c = ud;
    term_busy_tick();
    if (!g_cfg.interactive || c->got_content || ollama_quiet) return;
    if (c->thinking_header && g_cfg.show_thinking) return;   /* thinking is being streamed visibly */
    c->spin_i = (c->spin_i + 1) % 10;
    if (c->think_tokens > 0)
        printf("\r\x1b[2K" C_MAGENTA "%s" C_RESET C_DIM " thinking… (%d tokens, %.0fs)  " C_GRAY "esc to interrupt" C_RESET, SPIN[c->spin_i], c->think_tokens, elapsed(&c->t0));
    else
        printf("\r\x1b[2K" C_ORANGE "%s" C_RESET C_DIM " %s… (%.0fs)  " C_GRAY "esc to interrupt" C_RESET, SPIN[c->spin_i],
               elapsed(&c->t0) > 8 ? "loading model / waiting" : "thinking", elapsed(&c->t0));
    fflush(stdout);
    c->spinner_shown = true;
}

static int on_line(const char *line, size_t len, void *ud) {
    chat_ctx *c = ud;
    if (len == 0) return 0;
    cJSON *j = cJSON_ParseWithLength(line, len);
    if (!j) return 0;
    cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (cJSON_IsString(err)) { snprintf(c->error, sizeof c->error, "%s", err->valuestring); cJSON_Delete(j); return 0; }
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(j, "message");
    if (msg) {
        term_status_live(++c->out_chunks);
        term_busy_tick();
        cJSON *th = cJSON_GetObjectItemCaseSensitive(msg, "thinking");
        if (cJSON_IsString(th) && th->valuestring[0]) {
            if (c->think_tokens == 0) term_busy("thinking");
            c->think_tokens++;
            sb_puts(&c->thinking, th->valuestring);
            if (g_cfg.show_thinking && !ollama_quiet) {
                spinner_clear(c);
                if (!c->thinking_header) { printf(C_MAGENTA "✻ Thinking…" C_RESET "\n" C_GRAY); c->thinking_header = true; }
                fputs(th->valuestring, stdout); fflush(stdout);
            } else if (!c->thinking_header) c->thinking_header = true;
        }
        cJSON *ct = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (cJSON_IsString(ct) && ct->valuestring[0]) {
            if (ollama_quiet) { sb_puts(&c->content, ct->valuestring); term_busy("generating"); }
            else if (!c->got_content) {
                spinner_clear(c);
                term_busy("generating");
                if (c->thinking_header && g_cfg.show_thinking) printf(C_RESET "\n\n");
                /* skip leading whitespace-only starts */
                fputs(C_ORANGE "●" C_RESET " ", stdout);
                c->got_content = true;
                const char *s = ct->valuestring; while (*s == '\n' || *s == ' ') s++;
                sb_puts(&c->content, ct->valuestring);
                md_feed(&c->md, s, strlen(s));
            } else {
                sb_puts(&c->content, ct->valuestring);
                md_feed(&c->md, ct->valuestring, strlen(ct->valuestring));
            }
        }
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        if (cJSON_IsArray(tc)) {
            if (!c->tool_calls) { c->tool_calls = cJSON_CreateArray(); term_busy("calling tools"); }
            cJSON *it;
            cJSON_ArrayForEach(it, tc) cJSON_AddItemToArray(c->tool_calls, cJSON_Duplicate(it, 1));
        }
    }
    cJSON *done = cJSON_GetObjectItemCaseSensitive(j, "done");
    if (cJSON_IsTrue(done) && c->stats) {
        cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(j, "prompt_eval_count"))) c->stats->prompt_tokens = (int)v->valuedouble;
        if ((v = cJSON_GetObjectItemCaseSensitive(j, "eval_count"))) c->stats->eval_tokens = (int)v->valuedouble;
        if ((v = cJSON_GetObjectItemCaseSensitive(j, "eval_duration"))) c->stats->eval_seconds = v->valuedouble / 1e9;
        if ((v = cJSON_GetObjectItemCaseSensitive(j, "total_duration"))) c->stats->total_seconds = v->valuedouble / 1e9;
    }
    cJSON_Delete(j);
    return 0;
}

/* ---------- fallback: tool calls leaked as text ----------
 * Some local models emit their native tool syntax as plain content when the
 * server-side parser fails. We recognise two shapes:
 *   Qwen3-Coder XML:  <function=NAME><parameter=K>V</parameter>...</function>
 *   Hermes/Qwen2.5:   <tool_call>{"name": "...", "arguments": {...}}</tool_call>
 * Returns a cJSON array of {function:{name,arguments}} or NULL. */
static char *trim_nl(const char *s, size_t n) {
    while (n && (s[0] == '\n' || s[0] == '\r')) { s++; n--; }
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) n--;
    return xstrndup(s, n);
}
cJSON *parse_text_tool_calls(const char *content) {
    cJSON *arr = NULL;
    const char *p = content;
    while ((p = strstr(p, "<function="))) {
        const char *ns = p + 10, *ne = strchr(ns, '>');
        if (!ne) break;
        char *name = trim_nl(ns, (size_t)(ne - ns));
        const char *fend = strstr(ne, "</function>");
        const char *q = ne + 1;
        cJSON *args = cJSON_CreateObject();
        while ((q = strstr(q, "<parameter=")) && (!fend || q < fend)) {
            const char *ks = q + 11, *ke = strchr(ks, '>');
            if (!ke) break;
            const char *ve = strstr(ke + 1, "</parameter>");
            if (!ve) break;
            char *k = trim_nl(ks, (size_t)(ke - ks));
            char *v = trim_nl(ke + 1, (size_t)(ve - ke - 1));
            cJSON_AddStringToObject(args, k, v);
            free(k); free(v);
            q = ve + 12;
        }
        if (name[0]) {
            if (!arr) arr = cJSON_CreateArray();
            cJSON *call = cJSON_CreateObject(); cJSON *fn = cJSON_AddObjectToObject(call, "function");
            cJSON_AddStringToObject(fn, "name", name); cJSON_AddItemToObject(fn, "arguments", args);
            cJSON_AddItemToArray(arr, call);
        } else cJSON_Delete(args);
        free(name);
        p = fend ? fend + 11 : ne + 1;
    }
    if (arr) return arr;
    p = content;
    while ((p = strstr(p, "<tool_call>"))) {
        const char *js = p + 11, *je = strstr(js, "</tool_call>");
        size_t n = je ? (size_t)(je - js) : strlen(js);
        cJSON *j = cJSON_ParseWithLength(js, n);
        if (j) {
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(j, "name");
            cJSON *ag = cJSON_GetObjectItemCaseSensitive(j, "arguments");
            if (!ag) ag = cJSON_GetObjectItemCaseSensitive(j, "parameters");
            if (cJSON_IsString(nm)) {
                if (!arr) arr = cJSON_CreateArray();
                cJSON *call = cJSON_CreateObject(); cJSON *fn = cJSON_AddObjectToObject(call, "function");
                cJSON_AddStringToObject(fn, "name", nm->valuestring);
                cJSON_AddItemToObject(fn, "arguments", ag ? cJSON_Duplicate(ag, 1) : cJSON_CreateObject());
                cJSON_AddItemToArray(arr, call);
            }
            cJSON_Delete(j);
        }
        p = je ? je + 12 : js + n;
    }
    return arr;
}

cJSON *ollama_chat(cJSON *messages, cJSON *tools, chat_stats *stats, bool *aborted) {
    *aborted = false;
    if (stats) memset(stats, 0, sizeof *stats);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_cfg.model);
    cJSON_AddItemReferenceToObject(req, "messages", messages);
    cJSON_AddBoolToObject(req, "stream", true);
    if (tools && cJSON_GetArraySize(tools) > 0) cJSON_AddItemReferenceToObject(req, "tools", tools);
    if (g_cfg.think >= 0) cJSON_AddBoolToObject(req, "think", g_cfg.think == 1);
    cJSON *opts = cJSON_AddObjectToObject(req, "options");
    if (g_cfg.num_ctx > 0) cJSON_AddNumberToObject(opts, "num_ctx", g_cfg.num_ctx);
    if (g_cfg.temperature >= 0) cJSON_AddNumberToObject(opts, "temperature", g_cfg.temperature);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    chat_ctx c; memset(&c, 0, sizeof c);
    md_init(&c.md); sb_init(&c.content); sb_init(&c.thinking);
    c.stats = stats;
    gettimeofday(&c.t0, NULL);

    term_raw(true);
    http_interrupt_fd = g_cfg.interactive ? STDIN_FILENO : -1;
    http_interrupt_check = term_poll_interrupt;
    http_idle = on_idle; http_idle_ud = &c;
    term_busy("waiting for model");
    on_idle(&c);
    http_result res;
    int rc = http_request(g_cfg.host, "POST", "/api/chat", body, NULL, on_line, &c, &res);
    http_idle = NULL; http_interrupt_fd = -1;
    term_busy(NULL);
    free(body);
    term_status_live(0);   /* the caller adds the final counts and refreshes the bar */
    spinner_clear(&c);
    if (c.got_content) { md_finish(&c.md); }
    else if (c.thinking_header && g_cfg.show_thinking) fputs(C_RESET, stdout);

    cJSON *msg = NULL;
    if (res.aborted) {
        *aborted = true;
        printf("\n" C_YELLOW "⏹ interrupted" C_RESET "\n");
    } else if (rc != 0) {
        printf(C_RED "✗ request failed: %s" C_RESET "\n", res.err);
        if (strstr(res.err, "connect")) printf(C_DIM "  is ollama running? try: ollama serve   (host: %s)" C_RESET "\n", g_cfg.host);
    } else if (c.error[0] || res.status >= 400) {
        printf(C_RED "✗ ollama error (%d): %s" C_RESET "\n", res.status, c.error[0] ? c.error : "unknown");
        if (strstr(c.error, "not found")) printf(C_DIM "  try /models to list, or: ollama pull %s" C_RESET "\n", g_cfg.model);
        if (strstr(c.error, "does not support tools")) printf(C_DIM "  this model has no tool support; use /tools off or pick another model" C_RESET "\n");
    }
    /* Build the assistant message even on abort (partial content keeps context coherent) */
    if (res.aborted || (rc == 0 && !c.error[0] && res.status < 400)) {
        if (c.got_content) printf("\n");
        msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "assistant");
        cJSON_AddStringToObject(msg, "content", c.content.data ? c.content.data : "");
        if (!res.aborted && (!c.tool_calls || cJSON_GetArraySize(c.tool_calls) == 0) && c.content.data && tools) {
            cJSON *fromtext = parse_text_tool_calls(c.content.data);
            if (fromtext) {
                if (c.tool_calls) cJSON_Delete(c.tool_calls);
                c.tool_calls = fromtext;
                printf(C_DIM "  (recovered %d tool call%s from text output)" C_RESET "\n", cJSON_GetArraySize(fromtext), cJSON_GetArraySize(fromtext) == 1 ? "" : "s");
            }
        }
        if (c.tool_calls && !res.aborted && cJSON_GetArraySize(c.tool_calls) > 0) { cJSON_AddItemToObject(msg, "tool_calls", c.tool_calls); c.tool_calls = NULL; }
        if (res.aborted && !c.content.len) { cJSON_Delete(msg); msg = NULL; }
    }
    if (c.tool_calls) cJSON_Delete(c.tool_calls);
    sb_free(&c.content); sb_free(&c.thinking);
    fflush(stdout);
    return msg;
}

/* ---------- plain (non-streaming) requests: spinner while we wait ---------- */
typedef struct { struct timeval t0; int frame; bool shown; const char *what; } wait_ctx;
static void wait_idle(void *ud) {
    wait_ctx *w = ud;
    term_busy_tick();
    if (!g_cfg.interactive) return;
    w->frame = (w->frame + 1) % 10;
    printf("\r\x1b[2K" C_ORANGE "%s" C_RESET C_DIM " %s… (%.0fs)" C_RESET, SPIN[w->frame], w->what, elapsed(&w->t0));
    fflush(stdout);
    w->shown = true;
}
static int plain_request(const char *method, const char *path, const char *body, const char *what, sbuf *out, http_result *res) {
    wait_ctx w = { .frame = 0, .shown = false, .what = what };
    gettimeofday(&w.t0, NULL);
    http_idle = wait_idle; http_idle_ud = &w;
    term_busy(what);
    int rc = http_request(g_cfg.host, method, path, body, out, NULL, NULL, res);
    http_idle = NULL; http_idle_ud = NULL;
    term_busy(NULL);
    if (w.shown) { fputs("\r\x1b[2K", stdout); fflush(stdout); }
    return rc;
}

cJSON *ollama_list_models(void) {
    sbuf out; sb_init(&out);
    http_result res;
    if (plain_request("GET", "/api/tags", NULL, "listing models", &out, &res) != 0 || res.status != 200) {
        printf(C_RED "✗ cannot list models: %s" C_RESET "\n", res.err[0] ? res.err : "bad status");
        sb_free(&out); return NULL;
    }
    cJSON *j = cJSON_Parse(out.data ? out.data : "");
    sb_free(&out);
    if (!j) return NULL;
    cJSON *arr = cJSON_CreateArray();
    cJSON *models = cJSON_GetObjectItemCaseSensitive(j, "models"), *m;
    cJSON_ArrayForEach(m, models) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(m, "name");
        if (cJSON_IsString(n)) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", n->valuestring);
            cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "details");
            cJSON *ps = d ? cJSON_GetObjectItemCaseSensitive(d, "parameter_size") : NULL;
            cJSON_AddStringToObject(o, "size", cJSON_IsString(ps) ? ps->valuestring : "");
            cJSON *caps = cJSON_GetObjectItemCaseSensitive(m, "capabilities");
            bool tools = false, think = false;
            cJSON *cp; cJSON_ArrayForEach(cp, caps) { if (cJSON_IsString(cp)) { if (!strcmp(cp->valuestring, "tools")) tools = true; if (!strcmp(cp->valuestring, "thinking")) think = true; } }
            cJSON_AddBoolToObject(o, "tools", tools);
            cJSON_AddBoolToObject(o, "thinking", think);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cJSON_Delete(j);
    return arr;
}

int ollama_ping(char *ver, size_t verlen) {
    sbuf out; sb_init(&out); http_result res;
    int rc = plain_request("GET", "/api/version", NULL, "contacting ollama", &out, &res);
    if (rc == 0 && res.status == 200 && out.data) {
        cJSON *j = cJSON_Parse(out.data);
        cJSON *v = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
        snprintf(ver, verlen, "%s", cJSON_IsString(v) ? v->valuestring : "?");
        cJSON_Delete(j);
    } else { snprintf(ver, verlen, "%s", res.err[0] ? res.err : "unreachable"); rc = -1; }
    sb_free(&out);
    return rc;
}

/* Maximum context length the model was trained for, from /api/show
 * (model_info."<arch>.context_length"). 0 if unknown. */
int ollama_model_context_length(const char *model) {
    if (!model || !*model) return 0;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    char *body = cJSON_PrintUnformatted(req); cJSON_Delete(req);
    sbuf out; sb_init(&out); http_result res;
    int rc = plain_request("POST", "/api/show", body, "reading model info", &out, &res);
    free(body);
    int ctx = 0;
    if (rc == 0 && res.status == 200 && out.data) {
        cJSON *j = cJSON_Parse(out.data);
        cJSON *info = j ? cJSON_GetObjectItemCaseSensitive(j, "model_info") : NULL;
        cJSON *k; cJSON_ArrayForEach(k, info) {
            size_t l = k->string ? strlen(k->string) : 0;
            if (l >= 15 && !strcmp(k->string + l - 15, ".context_length") && cJSON_IsNumber(k)) { ctx = (int)k->valuedouble; break; }
        }
        cJSON_Delete(j);
    }
    sb_free(&out);
    return ctx;
}
