/* Hosted model APIs the advisor can consult instead of a model on the Ollama server: xAI (Grok),
 * OpenAI and Anthropic (Claude), named PROVIDER:MODEL — "xai:grok-4.7", "openai:gpt-5.2",
 * "anthropic:claude-opus-5". xAI and OpenAI speak Chat Completions, Anthropic the Messages API.
 *
 * A consultation is one request, not streamed: the advice is not shown while it is being written,
 * so there is nothing to stream it for, and one JSON answer is one thing to parse where streaming
 * would be two event formats. The keys come from the environment (XAI_API_KEY, OPENAI_API_KEY,
 * ANTHROPIC_API_KEY) and are never written anywhere; the base URLs can be pointed elsewhere
 * (XAI_BASE_URL, OPENAI_BASE_URL, ANTHROPIC_BASE_URL) — a proxy, a compatible server, a test's
 * fake. */
#define _GNU_SOURCE
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

static const provider_def PROVIDERS[] = {
    { "xai",       "grok",   "xAI",       "XAI_API_KEY",       "XAI_BASE_URL",       "https://api.x.ai/v1",       PROVIDER_CHAT_COMPLETIONS },
    { "openai",    NULL,     "OpenAI",    "OPENAI_API_KEY",    "OPENAI_BASE_URL",    "https://api.openai.com/v1", PROVIDER_CHAT_COMPLETIONS },
    /* no /v1 in the base: ANTHROPIC_BASE_URL is written that way for the SDKs (and Claude Code) */
    { "anthropic", "claude", "Anthropic", "ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "https://api.anthropic.com", PROVIDER_MESSAGES },
};

const provider_def *provider_find(const char *advisor, const char **model) {
    const char *c = advisor ? strchr(advisor, ':') : NULL;
    if (!c || !c[1]) return NULL;
    size_t n = (size_t)(c - advisor);
    for (size_t i = 0; i < sizeof PROVIDERS / sizeof *PROVIDERS; i++) {
        const provider_def *p = &PROVIDERS[i];
        if ((strlen(p->name) == n && !strncasecmp(advisor, p->name, n)) || (p->alias && strlen(p->alias) == n && !strncasecmp(advisor, p->alias, n))) {
            if (model) *model = c + 1;
            return p;
        }
    }
    return NULL;
}

const char *provider_base_url(const provider_def *p) {
    const char *u = getenv(p->url_env);
    return u && *u ? u : p->url;
}

/* Anthropic's models with server-side refusal fallbacks: a request their safety classifiers
 * decline (security tooling can look like that) is answered by another model instead of not at all */
bool provider_fallbacks(const char *advisor) {
    const char *model; const provider_def *p = provider_find(advisor, &model);
    if (!p || p->style != PROVIDER_MESSAGES) return false;
    return !strncmp(model, "claude-opus-5", 13) || !strncmp(model, "claude-fable-5", 14) || !strncmp(model, "claude-mythos-5", 15);
}

/* What a Chat Completions model may be set to. The API does not say per model, so these are the
 * names the provider's models take between them; one a model lacks is refused by the server,
 * which says so (and /advisor effort default takes it back). "off" goes out as "none". */
static void chat_levels(const provider_def *p, model_info *mi) {
    static const char *XAI[] = { "low", "medium", "high", "xhigh", NULL };
    static const char *OPENAI[] = { "minimal", "low", "medium", "high", "xhigh", "max", NULL };
    const char **lv = !strcmp(p->name, "xai") ? XAI : OPENAI;
    mi->thinking = mi->think_declared = mi->think_off = true;
    for (int i = 0; lv[i] && mi->n_think_levels < EFFORT_LEVELS_MAX; i++) snprintf(mi->think_levels[mi->n_think_levels++], EFFORT_NAME_MAX, "%s", lv[i]);
}

static void info_reset(model_info *mi) { memset(mi, 0, sizeof *mi); mi->draft = -1; }

static bool supported(cJSON *node) {
    cJSON *s = node ? cJSON_GetObjectItemCaseSensitive(node, "supported") : NULL;
    return cJSON_IsTrue(s);
}

/* The model endpoint's answer. Anthropic's says what the model takes (capabilities: its thinking
 * types and effort levels, and its window); the Chat Completions ones only that it exists. */
void provider_parse_model(const char *advisor, const char *json, model_info *mi) {
    info_reset(mi);
    const provider_def *p = provider_find(advisor, NULL);
    if (!p) return;
    if (p->style == PROVIDER_CHAT_COMPLETIONS) { chat_levels(p, mi); return; }
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "max_input_tokens");
    if (cJSON_IsNumber(v)) mi->context_length = (int)v->valuedouble;
    cJSON *caps = cJSON_GetObjectItemCaseSensitive(j, "capabilities");
    cJSON *th = caps ? cJSON_GetObjectItemCaseSensitive(caps, "thinking") : NULL;
    cJSON *types = th ? cJSON_GetObjectItemCaseSensitive(th, "types") : NULL;
    mi->think_adaptive = supported(types ? cJSON_GetObjectItemCaseSensitive(types, "adaptive") : NULL);
    mi->think_budget = supported(types ? cJSON_GetObjectItemCaseSensitive(types, "enabled") : NULL);
    cJSON *ef = caps ? cJSON_GetObjectItemCaseSensitive(caps, "effort") : NULL;
    static const char *LEVELS[] = { "low", "medium", "high", "xhigh", "max" };
    if (supported(ef))
        for (int i = 0; i < 5; i++)
            if (supported(cJSON_GetObjectItemCaseSensitive(ef, LEVELS[i]))) snprintf(mi->think_levels[mi->n_think_levels++], EFFORT_NAME_MAX, "%s", LEVELS[i]);
    if (mi->n_think_levels) {
        /* no "off": turning thinking off is refused by some (Fable) and only half-allowed by others
         * (Opus 5, up to high) — the lowest level is the cheap end */
        mi->thinking = mi->think_declared = true;
        snprintf(mi->think_default, EFFORT_NAME_MAX, "high");   /* what the API does with no effort given */
    } else if (mi->think_budget) {
        mi->thinking = mi->think_declared = mi->think_off = mi->think_on = true;   /* a thinking budget, or none */
        snprintf(mi->think_default, EFFORT_NAME_MAX, "off");
    }
    mi->caps_known = cJSON_IsObject(caps);
    cJSON_Delete(j);
}

char *provider_request_body(const char *advisor, const model_info *mi, const provider_request *rq) {
    const char *model = ""; const provider_def *p = provider_find(advisor, &model);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    const char *level = rq->effort ? effort_resolve(mi, rq->effort, NULL) : NULL;
    if (!p || p->style == PROVIDER_CHAT_COMPLETIONS) {
        cJSON *msgs = cJSON_AddArrayToObject(req, "messages"), *m;
        m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "system"); cJSON_AddStringToObject(m, "content", rq->system ? rq->system : ""); cJSON_AddItemToArray(msgs, m);
        m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", rq->user ? rq->user : ""); cJSON_AddItemToArray(msgs, m);
        /* max_tokens is the deprecated name, and reasoning models refuse it */
        if (rq->max_tokens > 0) cJSON_AddNumberToObject(req, "max_completion_tokens", rq->max_tokens);
        if (level && strcmp(level, "on")) cJSON_AddStringToObject(req, "reasoning_effort", !strcmp(level, "off") ? "none" : level);
    } else {
        cJSON_AddNumberToObject(req, "max_tokens", rq->max_tokens > 0 ? rq->max_tokens : 16000);
        cJSON_AddStringToObject(req, "system", rq->system ? rq->system : "");
        cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
        cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", rq->user ? rq->user : ""); cJSON_AddItemToArray(msgs, m);
        bool named = level && strcmp(level, "off") && strcmp(level, "on");
        if (mi->think_adaptive) {
            /* the advisor is there to think: on the models where leaving it out means no thinking
             * (Opus 4.6-4.8) as well as on those where it is the default */
            cJSON *t = cJSON_AddObjectToObject(req, "thinking"); cJSON_AddStringToObject(t, "type", "adaptive");
        } else if (mi->think_budget && level && !strcmp(level, "on") && rq->max_tokens > 2048) {
            cJSON *t = cJSON_AddObjectToObject(req, "thinking"); cJSON_AddStringToObject(t, "type", "enabled");
            cJSON_AddNumberToObject(t, "budget_tokens", rq->max_tokens / 2);   /* at least 1024, and below max_tokens */
        }
        if (named) { cJSON *oc = cJSON_AddObjectToObject(req, "output_config"); cJSON_AddStringToObject(oc, "effort", level); }
        if (provider_fallbacks(advisor)) cJSON_AddStringToObject(req, "fallbacks", "default");
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* what a failed request says about itself, on one line */
static void error_detail(const char *json, char *out, size_t n) {
    cJSON *j = cJSON_Parse(json ? json : "");
    cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
    cJSON *m = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
    if (cJSON_IsString(m)) snprintf(out, n, "%s", m->valuestring);
    else if (cJSON_IsString(e)) snprintf(out, n, "%s", e->valuestring);   /* xAI: {"code": …, "error": "…"} */
    else if (j && cJSON_IsString(m = cJSON_GetObjectItemCaseSensitive(j, "message"))) snprintf(out, n, "%s", m->valuestring);
    else snprintf(out, n, "%.200s", json && *json ? json : "no details");
    for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    /* "Incorrect API key provided: sk-proj-********abcd." — even masked, a piece of the key has no
     * business in a tool result (which is saved with the session, and shown to the next advisor) */
    char *k = strcasestr(out, "key provided:");
    if (k) {   /* "…key provided: XXXX. You can…" → "…key provided. You can…" */
        char *e = k + 12, *t = e + 1; while (*t == ' ') t++;
        while (*t && *t != ' ') t++;
        if (t > e && t[-1] == '.') t--;
        memmove(e, t, strlen(t) + 1);
    }
    cJSON_Delete(j);
}

/* a key the provider does not take: 401/403 — and xAI's 400 "Incorrect API key provided" */
static bool key_refused(int status, const char *detail) {
    return status == 401 || status == 403 || (status == 400 && strcasestr(detail, "api key"));
}

static void status_error(const provider_def *p, const char *model, int status, const char *json, char *err, size_t n) {
    char d[320]; error_detail(json, d, sizeof d);
    if (key_refused(status, d)) snprintf(err, n, "%s refused the key in %s (%d: %s)", p->label, p->key_env, status, d);
    else if (status == 404) snprintf(err, n, "%s does not know a model '%s' (404: %s)", p->label, model, d);
    else if (status == 429) snprintf(err, n, "%s is rate-limiting the key, or its credit is used up (429: %s)", p->label, d);
    else snprintf(err, n, "%s answered %d: %s", p->label, status, d);
}

char *provider_parse_reply(const char *advisor, const char *json, int status, chat_stats *st, char *err, size_t n) {
    const char *model = ""; const provider_def *p = provider_find(advisor, &model);
    err[0] = 0;
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", advisor ? advisor : ""); return NULL; }
    if (status >= 400 || status == 0) { status_error(p, model, status, json, err, n); return NULL; }
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { snprintf(err, n, "%s sent an answer that is not JSON: %.120s", p->label, json ? json : ""); return NULL; }
    sbuf text; sb_init(&text); sb_append(&text, "", 0);
    char *out = NULL;
    cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "usage"), *v;
    if (p->style == PROVIDER_MESSAGES) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "content"), *b;
        cJSON_ArrayForEach(b, c) {   /* the text blocks: thinking blocks are the way there, not the advice */
            cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "type"), *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
            if (cJSON_IsString(t) && !strcmp(t->valuestring, "text") && cJSON_IsString(tx)) sb_puts(&text, tx->valuestring);
        }
        cJSON *sr = cJSON_GetObjectItemCaseSensitive(j, "stop_reason");
        const char *why = cJSON_IsString(sr) ? sr->valuestring : "";
        if (!strcmp(why, "max_tokens")) snprintf(st->done_reason, sizeof st->done_reason, "length");
        else snprintf(st->done_reason, sizeof st->done_reason, "stop");
        if (u) {
            const char *in_keys[] = { "input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens" };
            for (int i = 0; i < 3; i++) if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, in_keys[i]))) st->prompt_tokens += (int)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "output_tokens"))) st->eval_tokens = (int)v->valuedouble;
        }
        if (!strcmp(why, "refusal")) {
            cJSON *sd = cJSON_GetObjectItemCaseSensitive(j, "stop_details");
            cJSON *cat = sd ? cJSON_GetObjectItemCaseSensitive(sd, "category") : NULL;
            snprintf(err, n, "%s declined to answer (refusal%s%s)", model, cJSON_IsString(cat) ? ": " : "", cJSON_IsString(cat) ? cat->valuestring : "");
            goto done;
        }
    } else {
        cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
        cJSON *msg = ch ? cJSON_GetObjectItemCaseSensitive(ch, "message") : NULL;
        cJSON *c = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
        if (cJSON_IsString(c)) sb_puts(&text, c->valuestring);
        cJSON *fr = ch ? cJSON_GetObjectItemCaseSensitive(ch, "finish_reason") : NULL;
        const char *why = cJSON_IsString(fr) ? fr->valuestring : "";
        snprintf(st->done_reason, sizeof st->done_reason, "%s", !strcmp(why, "length") ? "length" : "stop");
        if (u) {
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens"))) st->prompt_tokens = (int)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "completion_tokens"))) st->eval_tokens = (int)v->valuedouble;
        }
        cJSON *rf = msg ? cJSON_GetObjectItemCaseSensitive(msg, "refusal") : NULL;
        if ((cJSON_IsString(rf) && rf->valuestring[0]) || !strcmp(why, "content_filter")) {
            snprintf(err, n, "%s declined to answer%s%.200s", model, cJSON_IsString(rf) && rf->valuestring[0] ? ": " : " (content filter)", cJSON_IsString(rf) ? rf->valuestring : "");
            goto done;
        }
    }
    out = sb_detach(&text);
done:
    sb_free(&text);
    cJSON_Delete(j);
    return out;
}

/* the headers a request to p carries; the strings live in *keep (free with sb_free) */
static void auth_headers(const provider_def *p, const char *advisor, const char *key, const char *hdrs[4], sbuf *keep) {
    sb_init(keep);
    int k = 0;
    if (p->style == PROVIDER_MESSAGES) {
        sb_printf(keep, "x-api-key: %s", key); sb_putc(keep, 0);
        size_t v = keep->len; sb_puts(keep, "anthropic-version: 2023-06-01"); sb_putc(keep, 0);
        size_t b = keep->len; sb_puts(keep, "anthropic-beta: server-side-fallback-2026-07-01"); sb_putc(keep, 0);
        hdrs[k++] = keep->data; hdrs[k++] = keep->data + v;
        if (provider_fallbacks(advisor)) hdrs[k++] = keep->data + b;
    } else {
        sb_printf(keep, "Authorization: Bearer %s", key); sb_putc(keep, 0);
        hdrs[k++] = keep->data;
    }
    hdrs[k] = NULL;
}

static void idle_tick(void *ud) { (void)ud; term_busy_tick(); }

/* A key goes over TLS, or stays on this machine. The defaults are all https; a base URL set to
 * plain http for another host — or to one without a scheme, which libcurl sends as http — would
 * hand the key to anyone on the way (a proxy on the LAN, a typo), so it is refused, not used. */
static bool base_insecure(const provider_def *p, char *err, size_t n) {
    const char *u = provider_base_url(p);
    if (!url_cleartext(u) || url_is_local(u)) return false;
    char host[256] = ""; url_hostname(u, host, sizeof host);
    snprintf(err, n, "%s is plain http to another machine (%s): the key in %s would cross the network unencrypted — use https://", p->url_env, host, p->key_env);
    return true;
}

int provider_model_info(const char *advisor, model_info *mi, char *err, size_t n) {
    err[0] = 0;
    const char *model; const provider_def *p = provider_find(advisor, &model);
    provider_parse_model(advisor, NULL, mi);   /* what it generally takes, until the API says more */
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", advisor ? advisor : ""); return -1; }
    const char *key = getenv(p->key_env);
    if (!key || !*key) { snprintf(err, n, "%s is not set — export it before starting corbienest", p->key_env); return -1; }
    if (base_insecure(p, err, n)) return -1;
    const char *hdrs[4]; sbuf keep; auth_headers(p, advisor, key, hdrs, &keep);
    char *id = url_encode(model);
    sbuf path; sb_init(&path); sb_printf(&path, "%s/models/%s", p->style == PROVIDER_MESSAGES ? "/v1" : "", id); free(id);
    sbuf out; sb_init(&out); http_result res;
    http_headers = hdrs; http_idle = idle_tick; http_idle_ud = NULL;
    int idle_was = http_idle_timeout_ms; http_idle_timeout_ms = 30 * 1000;   /* a model list answers at once, or not at all */
    term_busy("checking the model");
    int rc = http_request(provider_base_url(p), "GET", path.data, NULL, &out, NULL, NULL, &res);
    term_busy(NULL);
    http_idle_timeout_ms = idle_was;
    http_headers = NULL; http_idle = NULL;
    int ret;
    if (rc != 0) { snprintf(err, n, "%s", res.err); ret = 1; }
    else if (res.status == 200) { provider_parse_model(advisor, out.data, mi); ret = 0; }
    else {
        char d[320]; error_detail(out.data, d, sizeof d);
        status_error(p, model, res.status, out.data, err, n);
        ret = key_refused(res.status, d) || res.status == 404 ? -1 : 1;
    }
    sb_free(&out); sb_free(&path); sb_free(&keep);
    return ret;
}

char *provider_chat(const char *advisor, const model_info *mi, const provider_request *rq, chat_stats *st, bool *aborted, char *err, size_t n) {
    *aborted = false; err[0] = 0;
    memset(st, 0, sizeof *st);
    const char *model; const provider_def *p = provider_find(advisor, &model);
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", advisor ? advisor : ""); return NULL; }
    const char *key = getenv(p->key_env);
    if (!key || !*key) { snprintf(err, n, "%s is not set — export it before starting corbienest", p->key_env); return NULL; }
    if (base_insecure(p, err, n)) return NULL;
    char *body = provider_request_body(advisor, mi, rq);
    const char *hdrs[4]; sbuf keep; auth_headers(p, advisor, key, hdrs, &keep);
    struct timeval t0, t1; gettimeofday(&t0, NULL);
    term_raw(true);
    ollama_stopped_for_message = false;
    http_interrupt_fd = g_cfg.interactive ? STDIN_FILENO : -1;
    http_interrupt_check = ollama_poll_or_message;
    http_idle = idle_tick; http_idle_ud = NULL;
    http_headers = hdrs;
    int idle_was = http_idle_timeout_ms;
    if (rq->idle_ms > 0) http_idle_timeout_ms = rq->idle_ms;
    term_busy(rq->busy ? rq->busy : "consulting");
    sbuf out; sb_init(&out); http_result res;
    int rc = http_request(provider_base_url(p), "POST", p->style == PROVIDER_MESSAGES ? "/v1/messages" : "/chat/completions", body, &out, NULL, NULL, &res);
    term_busy(NULL);
    http_idle_timeout_ms = idle_was;
    http_headers = NULL; http_idle = NULL; http_interrupt_fd = -1;
    gettimeofday(&t1, NULL);
    st->total_seconds = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_usec - t0.tv_usec) / 1e6;
    char *text = NULL;
    if (res.aborted) *aborted = true;
    else if (rc != 0) snprintf(err, n, "%s", res.err);
    else text = provider_parse_reply(advisor, out.data, res.status, st, err, n);
    free(body); sb_free(&out); sb_free(&keep);
    return text;
}
