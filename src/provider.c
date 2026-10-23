/* Hosted model APIs, named PROVIDER:MODEL — "xai:grok-4.7", "openai:gpt-5.2",
 * "anthropic:claude-opus-5". xAI, OpenAI, DeepSeek and Kimi speak Chat Completions: advisors, and the workers of /goal.
 * Anthropic speaks the Messages API: a consultation (provider_chat) is one toolless question,
 * and a turn (provider_turn) is the model that does the work — tools, streaming, the same
 * conversation ollama_chat() speaks, translated at the wire.
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
    { "xai",       "grok",   "xAI",       "XAI_API_KEY",       "XAI_BASE_URL",       "https://api.x.ai/v1",       PROVIDER_CHAT_COMPLETIONS, "grok-4.7", "Grok", false },
    { "openai",    "chatgpt","OpenAI",    "OPENAI_API_KEY",    "OPENAI_BASE_URL",    "https://api.openai.com/v1", PROVIDER_CHAT_COMPLETIONS, "gpt-5.2", "ChatGPT", false },
    /* no /v1 in the base: ANTHROPIC_BASE_URL is written that way for the SDKs (and Claude Code) */
    { "anthropic", "claude", "Anthropic", "ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "https://api.anthropic.com", PROVIDER_MESSAGES,         "claude-opus-5", "Claude", false },
    /* verified against the vendors' docs (2026-10): DeepSeek's API is OpenAI-compatible at the root, Kimi's at /v1 */
    { "deepseek",  "deepseek","DeepSeek","DEEPSEEK_API_KEY",  "DEEPSEEK_BASE_URL",  "https://api.deepseek.com",  PROVIDER_CHAT_COMPLETIONS, "deepseek-flash", "DeepSeek", false },
    { "kimi",      "moonshot","Kimi",    "MOONSHOT_API_KEY",  "MOONSHOT_BASE_URL",  "https://api.moonshot.ai/v1", PROVIDER_CHAT_COMPLETIONS, "kimi-k2.6", "Kimi", false },
};

/* The hosts the user added (/model add), after the built-in table. A fixed array: a pointer from provider_at
 * or provider_find stays valid until that host is removed or replaced, and nothing else moves. */
#define N_BUILTIN ((int)(sizeof PROVIDERS / sizeof *PROVIDERS))
static provider_def g_hosts[PROVIDER_HOSTS_MAX];
static int g_nhosts = 0;

int provider_host_count(void) { return g_nhosts; }

const provider_def *provider_at(int i) {
    if (i < 0) return NULL;
    if (i < N_BUILTIN) return &PROVIDERS[i];
    if (i - N_BUILTIN < g_nhosts) return &g_hosts[i - N_BUILTIN];
    return NULL;
}

const provider_def *provider_find(const char *advisor, const char **model) {
    const char *c = advisor ? strchr(advisor, ':') : NULL;
    if (!c || !c[1]) return NULL;
    size_t n = (size_t)(c - advisor);
    for (int i = 0; provider_at(i); i++) {
        const provider_def *p = provider_at(i);
        if ((strlen(p->name) == n && !strncasecmp(advisor, p->name, n)) || (p->alias && strlen(p->alias) == n && !strncasecmp(advisor, p->alias, n))) {
            if (model) *model = c + 1;
            return p;
        }
    }
    return NULL;
}

const char *provider_base_url(const provider_def *p) {
    const char *u = p->url_env && *p->url_env ? getenv(p->url_env) : NULL;   /* a host has no environment variable for its URL */
    return u && *u ? u : p->url;
}

static void set_err(char *err, size_t n, const char *msg) { if (err && n) snprintf(err, n, "%s", msg); }

bool provider_name_ok(const char *name, char *err, size_t n) {
    size_t len = name ? strlen(name) : 0;
    if (len < 2 || len > 24) { set_err(err, n, "a host name is 2 to 24 characters"); return false; }
    if (name[0] < 'a' || name[0] > 'z') { set_err(err, n, "a host name starts with a letter"); return false; }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) { set_err(err, n, "a host name is lowercase letters, digits and '-' only"); return false; }
    }
    for (int i = 0; i < N_BUILTIN; i++) {
        const provider_def *p = &PROVIDERS[i];
        if (!strcasecmp(name, p->name) || (p->alias && !strcasecmp(name, p->alias))) {
            if (err && n) snprintf(err, n, "%s is a built-in provider's name", name);
            return false;
        }
    }
    return true;
}

bool provider_host_url_ok(const char *url, char *err, size_t n) {
    if (!url || !*url) { set_err(err, n, "the URL is empty"); return false; }
    if (strchr(url, ' ') || strlen(url) >= 512) { set_err(err, n, "the URL has a space in it or is too long"); return false; }
    if (strncasecmp(url, "https://", 8) && strncasecmp(url, "http://", 7)) { set_err(err, n, "the URL starts with https:// (or http:// for this machine)"); return false; }
    char h[256] = "";
    if (!url_hostname(url, h, sizeof h) || !*h) { set_err(err, n, "the URL has no host in it"); return false; }
    if (url_cleartext(url) && !url_is_local(url)) {
        if (err && n) snprintf(err, n, "%s is plain http to another machine: the key would cross the network unencrypted — use https://", h);
        return false;
    }
    return true;
}

/* an environment variable name: letters, digits and '_', not starting with a digit */
static bool env_name_ok(const char *s) {
    size_t len = s ? strlen(s) : 0;
    if (!len || len >= 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!letter && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

static void free_host(provider_def *h) {
    free((void *)h->name); free((void *)h->label); free((void *)h->key_env); free((void *)h->url);
    memset(h, 0, sizeof *h);
}

int provider_host_add(const char *name, const char *url, const char *key_env, char *err, size_t n) {
    if (!provider_name_ok(name, err, n) || !provider_host_url_ok(url, err, n)) return -1;
    char key[64];
    if (key_env && *key_env) {
        if (!env_name_ok(key_env)) { set_err(err, n, "the key variable is a name like OPENROUTER_API_KEY"); return -1; }
        snprintf(key, sizeof key, "%s", key_env);
    } else {   /* NAME_API_KEY: "my-host" is MY_HOST_API_KEY */
        size_t k = 0;
        for (size_t i = 0; name[i] && k < sizeof key - 9; i++) key[k++] = name[i] == '-' ? '_' : (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
        snprintf(key + k, sizeof key - k, "_API_KEY");
    }
    int slot = -1;
    for (int i = 0; i < g_nhosts; i++) if (!strcasecmp(g_hosts[i].name, name)) slot = i;
    if (slot >= 0) free_host(&g_hosts[slot]);   /* the same name again: its URL and key are replaced */
    else {
        if (g_nhosts >= PROVIDER_HOSTS_MAX) { if (err && n) snprintf(err, n, "at most %d hosts can be added", PROVIDER_HOSTS_MAX); return -1; }
        slot = g_nhosts++;
    }
    provider_def *h = &g_hosts[slot];
    h->name = xstrdup(name); h->label = xstrdup(name); h->key_env = xstrdup(key);
    h->url_env = ""; h->url = xstrdup(url);
    h->style = PROVIDER_CHAT_COMPLETIONS; h->example = NULL; h->known_as = NULL; h->host = true;
    return 0;
}

bool provider_host_remove(const char *name) {
    for (int i = 0; i < g_nhosts; i++) {
        if (strcasecmp(g_hosts[i].name, name)) continue;
        free_host(&g_hosts[i]);
        for (int j = i; j + 1 < g_nhosts; j++) g_hosts[j] = g_hosts[j + 1];
        memset(&g_hosts[--g_nhosts], 0, sizeof g_hosts[0]);
        return true;
    }
    return false;
}

const char *provider_chat_path(const provider_def *p) {
    return p && p->style == PROVIDER_MESSAGES ? "/v1/messages" : "/chat/completions";
}

/* Anthropic's models with server-side refusal fallbacks (the refusals page, which names them: Fable
 * 5.1 and 5, Opus 5.5 and 5, Sonnet 5.5; Haiku 5.5 has none): a request their safety classifiers
 * decline (security tooling can look like that) is answered by another model instead of not at all */
bool provider_fallbacks(const char *advisor) {
    const char *model; const provider_def *p = provider_find(advisor, &model);
    if (!p || p->style != PROVIDER_MESSAGES) return false;
    return !strncmp(model, "claude-opus-5", 13) || !strncmp(model, "claude-fable-5", 14) || !strncmp(model, "claude-mythos-5", 15)
        || !strncmp(model, "claude-sonnet-5-5", 17);
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
    mi->tools = true;   /* a Messages model does the work with the same tools; the endpoint does not say so */
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "max_input_tokens");
    cJSON *mo = cJSON_GetObjectItemCaseSensitive(j, "max_tokens");
    if (cJSON_IsNumber(mo)) mi->max_output = (int)mo->valuedouble;
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
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "cache_creation_input_tokens"))) st->cache_write += (int)v->valuedouble;
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "cache_read_input_tokens"))) st->cache_read += (int)v->valuedouble;
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
            /* the cached part of the prompt (OpenAI and xAI: prompt_tokens_details.cached_tokens, a part of prompt_tokens) */
            cJSON *pd = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens_details");
            if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(pd, "cached_tokens"))) st->cache_read = (int)v->valuedouble;
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

/* ---------- a Messages turn: the conversation ollama_chat() speaks, on the wire ----------
 * The stored messages stay as they are (sessions, the advisor, compaction). This is the only
 * place that knows a tool call is a tool_use block and a tool result has to sit in the user
 * message after it. provider_content, when the assistant message has it, is that reply's blocks
 * verbatim — thinking and its signature included, which the next call is refused without. */

static const char *jrole(cJSON *m) {
    cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
    return cJSON_IsString(r) ? r->valuestring : "";
}
static const char *jcontent(cJSON *m) {
    cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
    return cJSON_IsString(c) ? c->valuestring : "";
}
static bool tool_offered(cJSON *tools, const char *name) {
    cJSON *t;
    if (!tools || !name) return false;
    cJSON_ArrayForEach(t, tools) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
        cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        if (cJSON_IsString(nm) && !strcmp(nm->valuestring, name)) return true;
    }
    return false;
}
static cJSON *args_object(cJSON *args) {
    if (cJSON_IsObject(args)) return cJSON_Duplicate(args, 1);
    if (cJSON_IsString(args) && args->valuestring[0]) {
        cJSON *p = cJSON_Parse(args->valuestring);
        if (cJSON_IsObject(p)) return p;
        cJSON_Delete(p);
    }
    return cJSON_CreateObject();
}
static bool block_is_thinking(cJSON *b) {
    cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type");
    return cJSON_IsString(ty) && (!strcmp(ty->valuestring, "thinking") || !strcmp(ty->valuestring, "redacted_thinking"));
}
static void cache_break(cJSON *block) {
    if (!block || block_is_thinking(block) || cJSON_GetObjectItemCaseSensitive(block, "cache_control")) return;
    cJSON *cc = cJSON_AddObjectToObject(block, "cache_control");
    cJSON_AddStringToObject(cc, "type", "ephemeral");
}
/* cache_control on a thinking block is a modified block, which the API refuses. The break goes on the
 * last block that is not one. */
static void cache_break_last(cJSON *content) {
    if (!cJSON_IsArray(content)) return;
    for (int k = cJSON_GetArraySize(content) - 1; k >= 0; k--) {
        cJSON *b = cJSON_GetArrayItem(content, k);
        if (block_is_thinking(b)) continue;
        cache_break(b);
        return;
    }
}
static void blocks_strip_thinking(cJSON *blocks) {
    if (!cJSON_IsArray(blocks)) return;
    for (int k = cJSON_GetArraySize(blocks) - 1; k >= 0; k--)
        if (block_is_thinking(cJSON_GetArrayItem(blocks, k))) cJSON_DeleteItemFromArray(blocks, k);
}
static cJSON *text_block(const char *s) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "text");
    cJSON_AddStringToObject(b, "text", s ? s : "");
    return b;
}
static void user_add(cJSON **acc, cJSON *block) {
    if (!*acc) *acc = cJSON_CreateArray();
    cJSON_AddItemToArray(*acc, block);
}
static void user_flush(cJSON *out, cJSON **acc) {
    if (!*acc || cJSON_GetArraySize(*acc) == 0) { cJSON_Delete(*acc); *acc = NULL; return; }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddItemToObject(m, "content", *acc);
    *acc = NULL;
    cJSON_AddItemToArray(out, m);
}

/* tool calls of one assistant message, in order, and whether each one may go out as a tool_use
 * (a name the request is not offering — the advisor, after it was turned off — would be a 400) */
#define TURN_CALLS 64
typedef struct { char id[40]; bool demote; } turn_call;

static bool blocks_sendable(cJSON *blocks, cJSON *tools) {
    cJSON *b;
    int uses = 0;
    if (!cJSON_IsArray(blocks) || cJSON_GetArraySize(blocks) == 0) return false;
    cJSON_ArrayForEach(b, blocks) {
        cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type");
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(b, "name");
        if (!cJSON_IsString(ty) || strcmp(ty->valuestring, "tool_use")) continue;
        /* pend[] holds TURN_CALLS: a result with no entry would go out as text and leave its tool_use unanswered */
        if (++uses > TURN_CALLS || !tool_offered(tools, cJSON_IsString(nm) ? nm->valuestring : "")) return false;
    }
    return true;
}

static cJSON *assistant_blocks(cJSON *m, cJSON *tools, turn_call *pend, int *pn) {
    *pn = 0;
    cJSON *pc = cJSON_GetObjectItemCaseSensitive(m, "provider_content");
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
    if (blocks_sendable(pc, tools)) {
        cJSON *b, *dup = cJSON_Duplicate(pc, 1);
        cJSON_ArrayForEach(b, dup) {
            cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type");
            if (!cJSON_IsString(ty) || strcmp(ty->valuestring, "tool_use") || *pn >= TURN_CALLS) continue;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "id");
            snprintf(pend[*pn].id, sizeof pend[*pn].id, "%s", cJSON_IsString(id) ? id->valuestring : "");
            pend[*pn].demote = false;
            (*pn)++;
        }
        return dup;
    }
    cJSON *blocks = cJSON_CreateArray();
    const char *text = jcontent(m);
    if (text[0]) cJSON_AddItemToArray(blocks, text_block(text));
    cJSON *call;
    cJSON_ArrayForEach(call, calls) {
        if (*pn >= TURN_CALLS) break;
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
        const char *name = cJSON_IsString(nm) ? nm->valuestring : "tool";
        if (cJSON_IsString(id) && id->valuestring[0]) snprintf(pend[*pn].id, sizeof pend[*pn].id, "%s", id->valuestring);
        else snprintf(pend[*pn].id, sizeof pend[*pn].id, "call_%d", *pn + 1);
        cJSON *input = args_object(fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL);
        pend[*pn].demote = !tool_offered(tools, name);
        if (pend[*pn].demote) {
            char *a = cJSON_PrintUnformatted(input);
            sbuf t; sb_init(&t); sb_printf(&t, "[called %s]\n%s", name, a ? a : "{}");
            cJSON_AddItemToArray(blocks, text_block(t.data));
            sb_free(&t); free(a); cJSON_Delete(input);
        } else {
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "tool_use");
            cJSON_AddStringToObject(b, "id", pend[*pn].id);
            cJSON_AddStringToObject(b, "name", name);
            cJSON_AddItemToObject(b, "input", input);
            cJSON_AddItemToArray(blocks, b);
        }
        (*pn)++;
    }
    return blocks;
}

char *provider_turn_body(const char *model_name, const model_info *mi, cJSON *messages, cJSON *tools, int max_tokens, const char *effort, bool thinking, double temperature, bool stream) {
    const char *model = ""; provider_find(model_name, &model);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddNumberToObject(req, "max_tokens", max_tokens > 0 ? max_tokens : 32000);
    cJSON_AddBoolToObject(req, "stream", stream);
    /* extended thinking refuses a temperature other than the default: with a thinking object, none goes out */
    bool think_on = thinking && mi && (mi->think_adaptive || (mi->think_budget && max_tokens > 2048));
    if (temperature >= 0 && !think_on) cJSON_AddNumberToObject(req, "temperature", temperature);
    bool named = effort && strcmp(effort, "off") && strcmp(effort, "on");
    if (thinking && mi && mi->think_adaptive) {
        cJSON *t = cJSON_AddObjectToObject(req, "thinking"); cJSON_AddStringToObject(t, "type", "adaptive");
    } else if (thinking && mi && mi->think_budget && max_tokens > 2048) {
        cJSON *t = cJSON_AddObjectToObject(req, "thinking"); cJSON_AddStringToObject(t, "type", "enabled");
        cJSON_AddNumberToObject(t, "budget_tokens", max_tokens / 2);
    }
    if (thinking && named) { cJSON *oc = cJSON_AddObjectToObject(req, "output_config"); cJSON_AddStringToObject(oc, "effort", effort); }
    if (provider_fallbacks(model_name)) cJSON_AddStringToObject(req, "fallbacks", "default");
    if (tools && cJSON_GetArraySize(tools) > 0) {
        cJSON *arr = cJSON_AddArrayToObject(req, "tools"), *t;
        cJSON_ArrayForEach(t, tools) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
            if (!fn) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(fn, "name"), *ds = cJSON_GetObjectItemCaseSensitive(fn, "description");
            cJSON *ps = cJSON_GetObjectItemCaseSensitive(fn, "parameters");
            cJSON_AddStringToObject(o, "name", cJSON_IsString(nm) ? nm->valuestring : "");
            cJSON_AddStringToObject(o, "description", cJSON_IsString(ds) ? ds->valuestring : "");
            cJSON_AddItemToObject(o, "input_schema", ps ? cJSON_Duplicate(ps, 1) : cJSON_CreateObject());
            cJSON_AddItemToArray(arr, o);
        }
    }
    sbuf sys; sb_init(&sys);
    cJSON *out = cJSON_CreateArray(), *acc = NULL, *m;
    turn_call pend[TURN_CALLS]; int pn = 0, pi = 0;
    bool strip = false;   /* a rewritten assistant message changes the prefix: no later thinking block may follow it */
    cJSON_ArrayForEach(m, messages) {
        const char *role = jrole(m);
        if (!strcmp(role, "system")) {
            user_flush(out, &acc);
            if (sys.len && jcontent(m)[0]) sb_puts(&sys, "\n\n");
            sb_puts(&sys, jcontent(m));
        } else if (!strcmp(role, "assistant")) {
            user_flush(out, &acc); pi = 0;
            cJSON *pc = cJSON_GetObjectItemCaseSensitive(m, "provider_content");
            bool verbatim = blocks_sendable(pc, tools);
            cJSON *blocks = assistant_blocks(m, tools, pend, &pn);
            if (!verbatim || strip) {          /* omitting some and keeping later ones is a gap, and that is refused */
                if (verbatim) blocks_strip_thinking(blocks);
                strip = true;
            }
            if (cJSON_GetArraySize(blocks) == 0) { cJSON_Delete(blocks); continue; }
            cJSON *am = cJSON_CreateObject();
            cJSON_AddStringToObject(am, "role", "assistant");
            cJSON_AddItemToObject(am, "content", blocks);
            cJSON_AddItemToArray(out, am);
        } else if (!strcmp(role, "tool")) {
            const char *got = NULL;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
            if (cJSON_IsString(id) && id->valuestring[0]) got = id->valuestring;
            bool demote = true; const char *use = got;
            if (pi < pn) {
                if (!use || !*use) use = pend[pi].id;
                demote = pend[pi].demote;
                pi++;
            }
            cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name");
            const char *name = cJSON_IsString(tn) ? tn->valuestring : "tool";
            if (demote || !use || !*use) {
                sbuf t; sb_init(&t); sb_printf(&t, "[%s result]\n%s", name, jcontent(m));
                user_add(&acc, text_block(t.data)); sb_free(&t);
            } else {
                cJSON *b = cJSON_CreateObject();
                cJSON_AddStringToObject(b, "type", "tool_result");
                cJSON_AddStringToObject(b, "tool_use_id", use);
                cJSON_AddStringToObject(b, "content", jcontent(m));
                user_add(&acc, b);
            }
        } else {   /* user, and anything else: said by the person */
            if (jcontent(m)[0]) user_add(&acc, text_block(jcontent(m)));
        }
    }
    user_flush(out, &acc);
    if (sys.len) {
        cJSON *arr = cJSON_CreateArray(), *b = text_block(sys.data);
        cache_break(b);
        cJSON_AddItemToArray(arr, b);
        cJSON_AddItemToObject(req, "system", arr);
    }
    sb_free(&sys);
    if (cJSON_GetArraySize(out) > 0) {
        cJSON *last = cJSON_GetArrayItem(out, cJSON_GetArraySize(out) - 1);
        cJSON *c = cJSON_GetObjectItemCaseSensitive(last, "content");
        cache_break_last(c);
    }
    cJSON_AddItemToObject(req, "messages", out);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* one assistant message, the shape run_turn already reads. `refusal` fills an empty answer. */
static cJSON *assistant_from_blocks(cJSON *content, const char *stop, const char *refusal, chat_stats *st) {
    sbuf text; sb_init(&text);
    cJSON *calls = cJSON_CreateArray(), *keep = cJSON_CreateArray(), *b;
    cJSON_ArrayForEach(b, content) {
        cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type");
        const char *t = cJSON_IsString(ty) ? ty->valuestring : "";
        if (!strcmp(t, "text")) {
            cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
            if (!cJSON_IsString(tx) || !tx->valuestring[0]) continue;
            sb_puts(&text, tx->valuestring);
            cJSON_AddItemToArray(keep, cJSON_Duplicate(b, 1));
        } else if (!strcmp(t, "redacted_thinking")) {
            cJSON_AddItemToArray(keep, cJSON_Duplicate(b, 1));
        } else if (!strcmp(t, "thinking")) {
            cJSON *th = cJSON_GetObjectItemCaseSensitive(b, "thinking"), *sig = cJSON_GetObjectItemCaseSensitive(b, "signature");
            if ((cJSON_IsString(th) && th->valuestring[0]) || (cJSON_IsString(sig) && sig->valuestring[0]))
                cJSON_AddItemToArray(keep, cJSON_Duplicate(b, 1));
        } else if (!strcmp(t, "tool_use")) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "id"), *nm = cJSON_GetObjectItemCaseSensitive(b, "name");
            cJSON *part = cJSON_GetObjectItemCaseSensitive(b, "_partial");
            cJSON *parsed = NULL;
            if (cJSON_IsString(part) && part->valuestring[0]) {
                cJSON *p = cJSON_Parse(part->valuestring);
                if (cJSON_IsObject(p)) parsed = p; else cJSON_Delete(p);
            }
            cJSON *in = parsed ? parsed : cJSON_GetObjectItemCaseSensitive(b, "input");
            cJSON *call = cJSON_CreateObject();
            if (cJSON_IsString(id) && id->valuestring[0]) cJSON_AddStringToObject(call, "id", id->valuestring);
            cJSON *fn = cJSON_AddObjectToObject(call, "function");
            cJSON_AddStringToObject(fn, "name", cJSON_IsString(nm) ? nm->valuestring : "");
            char *args = cJSON_IsObject(in) ? cJSON_PrintUnformatted(in) : xstrdup("{}");
            cJSON_AddStringToObject(fn, "arguments", args ? args : "{}");
            free(args);
            cJSON_AddItemToArray(calls, call);
            cJSON *copy = cJSON_Duplicate(b, 1);
            cJSON_DeleteItemFromObject(copy, "_partial");
            if (parsed) {
                cJSON_DeleteItemFromObject(copy, "input");
                cJSON_AddItemToObject(copy, "input", parsed);   /* the copy keeps the object the stream assembled */
            }
            cJSON_AddItemToArray(keep, copy);
        }
    }
    if (refusal && refusal[0] && !text.len) sb_puts(&text, refusal);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", text.data ? text.data : "");
    if (cJSON_GetArraySize(calls) > 0) cJSON_AddItemToObject(msg, "tool_calls", calls); else cJSON_Delete(calls);
    if (cJSON_GetArraySize(keep) > 0) cJSON_AddItemToObject(msg, "provider_content", keep); else cJSON_Delete(keep);
    sb_free(&text);
    if (st) snprintf(st->done_reason, sizeof st->done_reason, "%s", stop && !strcmp(stop, "max_tokens") ? "length" : "stop");
    return msg;
}

static void take_usage(cJSON *u, chat_stats *st, bool inputs) {
    if (!cJSON_IsObject(u) || !st) return;
    cJSON *v;
    const char *in_keys[] = { "input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens" };
    if (inputs) for (int i = 0; i < 3; i++) if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, in_keys[i]))) st->prompt_tokens += (int)v->valuedouble;
    if (inputs && cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "cache_creation_input_tokens"))) st->cache_write += (int)v->valuedouble;
    if (inputs && cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "cache_read_input_tokens"))) st->cache_read += (int)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(u, "output_tokens"))) st->eval_tokens = (int)v->valuedouble;
}

/* ---------- Chat Completions turns (the workers of /goal on xAI, OpenAI, DeepSeek, Kimi) ----------
 * The conversation is the one ollama_chat() speaks, and so is the reply: an assistant message with its
 * tool_calls (arguments as an object) and its content. Chat Completions wants the arguments as a JSON
 * string, every result after the id of its call, and max_completion_tokens from OpenAI; that is
 * translated here, at the wire. Calls that arrive without an id get one, and the results that follow
 * them without one are matched to those ids in order. */
static bool cc_openai(const provider_def *p) { return !strcmp(p->name, "openai"); }

static cJSON *cc_messages(cJSON *messages) {
    cJSON *out = cJSON_CreateArray();
    cJSON *pending = cJSON_CreateArray();   /* the ids of the last assistant message's calls */
    int seq = 0, next = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, messages) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
        if (!cJSON_IsString(role)) continue;
        cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
        const char *text = cJSON_IsString(content) ? content->valuestring : "";
        cJSON *o = cJSON_CreateObject();
        if (!strcmp(role->valuestring, "tool")) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
            const char *cid = cJSON_IsString(id) && id->valuestring[0] ? id->valuestring : NULL;
            if (!cid && next < cJSON_GetArraySize(pending)) cid = cJSON_GetArrayItem(pending, next++)->valuestring;
            cJSON_AddStringToObject(o, "role", "tool");
            if (cid) cJSON_AddStringToObject(o, "tool_call_id", cid);
            cJSON_AddStringToObject(o, "content", text);
        } else if (!strcmp(role->valuestring, "assistant")) {
            cJSON_AddStringToObject(o, "role", "assistant");
            cJSON_AddStringToObject(o, "content", text);
            cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
            if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls)) {
                cJSON *tc = cJSON_AddArrayToObject(o, "tool_calls");
                cJSON_Delete(pending); pending = cJSON_CreateArray(); next = 0;
                cJSON *call;
                cJSON_ArrayForEach(call, calls) {
                    cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
                    cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
                    cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                    cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
                    char idbuf[48];
                    if (cJSON_IsString(id) && id->valuestring[0]) snprintf(idbuf, sizeof idbuf, "%s", id->valuestring);
                    else snprintf(idbuf, sizeof idbuf, "call_%d", ++seq);
                    cJSON *t = cJSON_CreateObject();
                    cJSON_AddStringToObject(t, "id", idbuf);
                    cJSON_AddStringToObject(t, "type", "function");
                    cJSON *f = cJSON_AddObjectToObject(t, "function");
                    cJSON_AddStringToObject(f, "name", cJSON_IsString(nm) ? nm->valuestring : "");
                    char *as = cJSON_IsString(args) ? xstrdup(args->valuestring) : args ? cJSON_PrintUnformatted(args) : xstrdup("{}");
                    cJSON_AddStringToObject(f, "arguments", as);
                    free(as);
                    cJSON_AddItemToArray(tc, t);
                    cJSON_AddItemToArray(pending, cJSON_CreateString(idbuf));
                }
            }
        } else {
            cJSON_AddStringToObject(o, "role", role->valuestring);
            cJSON_AddStringToObject(o, "content", text);
            cJSON_Delete(pending); pending = cJSON_CreateArray(); next = 0;
        }
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(pending);
    return out;
}

/* the body of one turn for a Chat Completions provider; mid is the model's own name (no prefix) */
static char *cc_turn_body(const provider_def *p, const char *mid, cJSON *messages, cJSON *tools, int max_tokens, double temperature) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "model", mid);
    cJSON_AddItemToObject(b, "messages", cc_messages(messages));
    if (tools && cJSON_GetArraySize(tools)) { cJSON_AddItemToObject(b, "tools", cJSON_Duplicate(tools, 1)); cJSON_AddStringToObject(b, "tool_choice", "auto"); }
    cJSON_AddBoolToObject(b, "stream", false);
    if (max_tokens > 0) cJSON_AddNumberToObject(b, cc_openai(p) ? "max_completion_tokens" : "max_tokens", max_tokens);
    if (temperature >= 0) cJSON_AddNumberToObject(b, "temperature", temperature);
    /* Kimi's K2 thinking models keep their reasoning between turns only when asked to; corbienest does not
     * carry it, so thinking is off for them. kimi-k3 always reasons (reasoning_effort) and is left alone. */
    if (!strcmp(p->name, "kimi") && !strncmp(mid, "kimi-k2", 7)) {
        cJSON *th = cJSON_AddObjectToObject(b, "thinking");
        cJSON_AddStringToObject(th, "type", "disabled");
    }
    char *s = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    return s;
}

/* the reply, in the shape ollama_chat() returns: role, content and tool_calls (id, function.name, function.arguments as an object) */
static cJSON *cc_turn_reply(const provider_def *p, const char *mid, const char *json, int status, chat_stats *st, char *err, size_t n) {
    (void)mid;
    if (status >= 400 || status == 0) { status_error(p, mid, status, json, err, n); return NULL; }
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { snprintf(err, n, "%s sent an answer that is not JSON: %.120s", p->label, json ? json : ""); return NULL; }
    cJSON *ch = cJSON_GetObjectItemCaseSensitive(j, "choices");
    cJSON *c0 = cJSON_IsArray(ch) ? cJSON_GetArrayItem(ch, 0) : NULL;
    cJSON *msg = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "message") : NULL;
    if (!msg) { snprintf(err, n, "%s sent an answer with no message", p->label); cJSON_Delete(j); return NULL; }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "role", "assistant");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    cJSON_AddStringToObject(reply, "content", cJSON_IsString(content) ? content->valuestring : "");
    cJSON *tcs = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
    if (cJSON_IsArray(tcs) && cJSON_GetArraySize(tcs)) {
        cJSON *calls = cJSON_AddArrayToObject(reply, "tool_calls");
        cJSON *t; int k = 0;
        cJSON_ArrayForEach(t, tcs) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
            char idbuf[48];
            if (cJSON_IsString(id) && id->valuestring[0]) snprintf(idbuf, sizeof idbuf, "%s", id->valuestring);
            else snprintf(idbuf, sizeof idbuf, "call_%d", ++k);
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "id", idbuf);
            cJSON *f = cJSON_AddObjectToObject(c, "function");
            cJSON_AddStringToObject(f, "name", cJSON_IsString(nm) ? nm->valuestring : "");
            cJSON *parsed = cJSON_IsString(args) ? cJSON_Parse(args->valuestring) : NULL;
            if (parsed) cJSON_AddItemToObject(f, "arguments", parsed);
            else cJSON_AddStringToObject(f, "arguments", cJSON_IsString(args) ? args->valuestring : "{}");
            cJSON_AddItemToArray(calls, c);
        }
    }
    if (st) {
        memset(st, 0, sizeof *st);
        cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "usage");
        if (u) {
            cJSON *pt = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens"), *ct = cJSON_GetObjectItemCaseSensitive(u, "completion_tokens");
            cJSON *pd = cJSON_GetObjectItemCaseSensitive(u, "prompt_tokens_details");
            cJSON *cached = pd ? cJSON_GetObjectItemCaseSensitive(pd, "cached_tokens") : NULL;
            cJSON *cw = pd ? cJSON_GetObjectItemCaseSensitive(pd, "cache_write_tokens") : NULL;
            if (cJSON_IsNumber(pt)) st->prompt_tokens = pt->valueint;
            if (cJSON_IsNumber(ct)) st->eval_tokens = ct->valueint;
            if (cJSON_IsNumber(cached)) st->cache_read = cached->valueint;
            if (cJSON_IsNumber(cw)) st->cache_write = cw->valueint;
        }
        cJSON *fr = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "finish_reason") : NULL;
        if (cJSON_IsString(fr)) snprintf(st->done_reason, sizeof st->done_reason, "%s", !strcmp(fr->valuestring, "length") ? "length" : "stop");
    }
    cJSON_Delete(j);
    err[0] = 0;
    return reply;
}

cJSON *provider_turn_reply(const char *model, const char *json, int status, chat_stats *st, char *err, size_t n) {
    const char *mid = ""; const provider_def *p = provider_find(model, &mid);
    err[0] = 0;
    if (st) memset(st, 0, sizeof *st);
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", model ? model : ""); return NULL; }
    if (p->style == PROVIDER_CHAT_COMPLETIONS) return cc_turn_reply(p, mid, json, status, st, err, n);
    if (status >= 400 || status == 0) { status_error(p, mid, status, json, err, n); return NULL; }
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { snprintf(err, n, "%s sent an answer that is not JSON: %.120s", p->label, json ? json : ""); return NULL; }
    cJSON *sr = cJSON_GetObjectItemCaseSensitive(j, "stop_reason");
    const char *why = cJSON_IsString(sr) ? sr->valuestring : "";
    char whybuf[160] = "";
    if (!strcmp(why, "refusal")) {
        cJSON *sd = cJSON_GetObjectItemCaseSensitive(j, "stop_details");
        cJSON *cat = sd ? cJSON_GetObjectItemCaseSensitive(sd, "category") : NULL;
        snprintf(whybuf, sizeof whybuf, "%s declined to answer (refusal%s%s)", mid, cJSON_IsString(cat) ? ": " : "", cJSON_IsString(cat) ? cat->valuestring : "");
    }
    cJSON *msg = assistant_from_blocks(cJSON_GetObjectItemCaseSensitive(j, "content"), why, whybuf, st);
    take_usage(cJSON_GetObjectItemCaseSensitive(j, "usage"), st, true);
    cJSON_Delete(j);
    return msg;
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
    snprintf(err, n, "%s is plain http to another machine (%s): the key in %s would cross the network unencrypted — use https://", p->url_env && *p->url_env ? p->url_env : "the base URL", host, p->key_env);
    return true;
}

int provider_model_info(const char *advisor, model_info *mi, char *err, size_t n) {
    err[0] = 0;
    const char *model; const provider_def *p = provider_find(advisor, &model);
    provider_parse_model(advisor, NULL, mi);   /* what it generally takes, until the API says more */
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", advisor ? advisor : ""); return -1; }
    const char *key = getenv(p->key_env);
    if (!key || !*key) { snprintf(err, n, "%s is not set — export it before starting corbienest (a NAME=value line without `export` is not passed on)", p->key_env); return -1; }
    if (base_insecure(p, err, n)) return -1;
    const char *hdrs[4]; sbuf keep; auth_headers(p, advisor, key, hdrs, &keep);
    char *id = url_encode(model);
    sbuf path; sb_init(&path); sb_printf(&path, "%s/models/%s", p->style == PROVIDER_MESSAGES ? "/v1" : "", id); free(id);
    sbuf out; sb_init(&out); http_result res;
    http_headers = hdrs; http_idle = idle_tick; http_idle_ud = NULL;
    int idle_was = http_idle_timeout_ms;   /* a model list answers at once, or not at all: 30 s, or less when the caller asks (the start-up check) */
    http_idle_timeout_ms = idle_was < 30 * 1000 ? idle_was : 30 * 1000;
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

/* A provider's model list — {"data":[{"id":…}]} at all three, Anthropic's with a display_name —
 * as an array of {"id","name"}, in the order given. NULL when the answer is not a list. */
cJSON *provider_parse_models(const char *json) {
    cJSON *j = cJSON_Parse(json ? json : ""), *data = cJSON_GetObjectItemCaseSensitive(j, "data"), *m;
    if (!cJSON_IsArray(data)) { cJSON_Delete(j); return NULL; }
    cJSON *arr = cJSON_CreateArray();
    cJSON_ArrayForEach(m, data) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id"), *dn = cJSON_GetObjectItemCaseSensitive(m, "display_name");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", id->valuestring);
        if (cJSON_IsString(dn)) cJSON_AddStringToObject(o, "name", dn->valuestring);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_Delete(j);
    return arr;
}

/* The models the key in the environment can use, for the suggestions under the input field:
 * asked silently, given up on after wait_ms, NULL without a key or an answer — err then says
 * which, in the few words a row of that list has room for. */
cJSON *provider_list_models(const provider_def *p, int wait_ms, char *err, size_t n) {
    err[0] = 0;
    const char *key = getenv(p->key_env);
    if (!key || !*key) { snprintf(err, n, "%s is not set — export it before starting corbienest", p->key_env); return NULL; }
    if (base_insecure(p, err, n)) return NULL;
    const char *hdrs[4]; sbuf keep; auth_headers(p, "", key, hdrs, &keep);
    sbuf out; sb_init(&out); http_result res;
    int idle_was = http_idle_timeout_ms, fd_was = http_interrupt_fd;
    http_headers = hdrs; http_idle_timeout_ms = wait_ms; http_interrupt_fd = -1;
    int rc = http_request(provider_base_url(p), "GET", p->style == PROVIDER_MESSAGES ? "/v1/models?limit=1000" : "/models", NULL, &out, NULL, NULL, &res);
    http_headers = NULL; http_idle_timeout_ms = idle_was; http_interrupt_fd = fd_was;
    cJSON *arr = rc == 0 && res.status == 200 ? provider_parse_models(out.data) : NULL;
    if (rc != 0) snprintf(err, n, "no model list: %s", res.err);
    else if (res.status == 404 || (res.status == 200 && !arr)) snprintf(err, n, "%s has no model list at %s", p->label, provider_base_url(p));
    else if (res.status != 200) status_error(p, "", res.status, out.data, err, n);
    sb_free(&out); sb_free(&keep);
    return arr;
}

/* ---------- gzip request bodies ----------
 * A body goes gzipped to a provider that has been seen to take it (gzip.NAME=1, set by /gzip probe), and only
 * when it is big enough to gain from it. A refusal is answered with the plain body; a plain call that then
 * succeeds turns gzip off for that provider for the rest of the session. The config is not written by a
 * request: only /gzip probe and /gzip NAME on|off save it. */
#define GZIP_MIN_BYTES 4096
#define GZIP_SESSION_MAX 24
static char g_gz_refused[GZIP_SESSION_MAX][64];
static int  g_n_gz_refused = 0;

static bool gzip_refused_here(const char *name) {
    for (int i = 0; i < g_n_gz_refused; i++) if (!strcmp(g_gz_refused[i], name)) return true;
    return false;
}

bool provider_gzip_wanted(const provider_def *p) {
    bool on = false;
    return p && gzip_available() && gzip_get(p->name, &on) && on && !gzip_refused_here(p->name);
}

void provider_gzip_refused(const provider_def *p) {
    if (!gzip_refused_here(p->name) && g_n_gz_refused < GZIP_SESSION_MAX) snprintf(g_gz_refused[g_n_gz_refused++], sizeof g_gz_refused[0], "%s", p->name);
    printf(C_DIM "  %s does not take gzipped request bodies: plain from now on, this session (/gzip probe %s saves that)" C_RESET "\n", p->label, p->name);
}

/* the bytes that go on the wire for a body: gzipped when the provider takes it and the body is big enough, or
 * when forced (the probe). False = send the plain body, and *z is NULL. */
static bool wire_gzip(const provider_def *p, const char *plain, bool force, char **z, size_t *zlen) {
    *z = NULL; *zlen = 0;
    size_t n = strlen(plain);
    if (!force && (n < GZIP_MIN_BYTES || !provider_gzip_wanted(p))) return false;
    *z = gzip_body(plain, n, zlen);
    return *z != NULL;
}

/* /gzip probe: one small call with the plain body and one gzipped (forced, so the probe is the same whatever
 * the size), each asking for one token. Returns gzip_probe_verdict(): 1 accepted, 0 refused, -1 no answer to go by;
 * `said` is one line for the user. */
int provider_gzip_probe(const char *advisor, char *said, size_t n) {
    const char *model = ""; const provider_def *p = provider_find(advisor, &model);
    if (!p) { snprintf(said, n, "'%s' is not a hosted model", advisor ? advisor : ""); return -1; }
    if (!gzip_available()) { snprintf(said, n, "this build has no zlib (ZLIB=0): nothing to compress with"); return -1; }
    const char *hdrs[4]; sbuf keep; char err[512] = "";
    if (!provider_call_headers(advisor, hdrs, &keep, err, sizeof err)) { snprintf(said, n, "%s", err); return -1; }
    provider_request rq; memset(&rq, 0, sizeof rq);
    rq.system = ""; rq.user = "hi"; rq.max_tokens = 1;
    model_info mi; memset(&mi, 0, sizeof mi);
    char *body = provider_request_body(advisor, &mi, &rq);
    size_t blen = strlen(body), zlen = 0;
    char *zbody = gzip_body(body, blen, &zlen);
    if (!zbody) { snprintf(said, n, "could not compress the probe"); free(body); sb_free(&keep); return -1; }
    const char *path = provider_chat_path(p);
    sbuf plain_out, gz_out; sb_init(&plain_out); sb_init(&gz_out);
    http_result pr, gr; memset(&pr, 0, sizeof pr); memset(&gr, 0, sizeof gr);
    http_headers = hdrs; http_interrupt_fd = -1; http_idle = NULL;
    int rc1 = http_request_bytes(provider_base_url(p), "POST", path, body, blen, false, &plain_out, NULL, NULL, &pr);
    int rc2 = rc1 == 0 ? http_request_bytes(provider_base_url(p), "POST", path, zbody, zlen, true, &gz_out, NULL, NULL, &gr) : -1;
    http_headers = NULL;
    int v = -1;
    if (rc1 != 0) snprintf(said, n, "the plain call failed: %s", pr.err);
    else if (rc2 != 0) snprintf(said, n, "the gzipped call failed: %s", gr.err);
    else {
        v = gzip_probe_verdict(pr.status, gr.status, gz_out.data);
        if (v == 1) snprintf(said, n, "accepted (plain %d, gzipped %d)", pr.status, gr.status);
        else if (v == 0) {
            char detail[200] = "";
            error_detail(gz_out.data ? gz_out.data : "", detail, sizeof detail);
            snprintf(said, n, "refused the gzipped body (%d)%s%s", gr.status, detail[0] ? ": " : "", detail);
        } else {
            char detail[200] = "";
            error_detail(gz_out.data ? gz_out.data : "", detail, sizeof detail);
            snprintf(said, n, "no conclusion: plain %d, gzipped %d%s%s", pr.status, gr.status, detail[0] ? ": " : "", detail);
        }
    }
    free(zbody); free(body); sb_free(&keep); sb_free(&plain_out); sb_free(&gz_out);
    return v;
}

/* a worker's call, started on a thread: its body gzipped when the provider takes it and allow_gzip is set.
 * *gzipped says whether it went gzipped (the caller restarts it plain if the server refuses that). */
http_job *provider_turn_start(const provider_def *p, const char *plain, const char *const *hdrs, bool allow_gzip, bool *gzipped) {
    char *z = NULL; size_t zl = 0;
    bool gz = allow_gzip && wire_gzip(p, plain, false, &z, &zl);
    http_job *j = http_job_start_bytes(provider_base_url(p), provider_chat_path(p), gz ? z : plain, gz ? zl : strlen(plain), gz, hdrs);
    free(z);
    *gzipped = gz && j;
    return j;
}

char *provider_chat(const char *advisor, const model_info *mi, const provider_request *rq, chat_stats *st, bool *aborted, char *err, size_t n) {
    *aborted = false; err[0] = 0;
    memset(st, 0, sizeof *st);
    const char *model; const provider_def *p = provider_find(advisor, &model);
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", advisor ? advisor : ""); return NULL; }
    const char *key = getenv(p->key_env);
    if (!key || !*key) { snprintf(err, n, "%s is not set — export it before starting corbienest (a NAME=value line without `export` is not passed on)", p->key_env); return NULL; }
    if (base_insecure(p, err, n)) return NULL;
    char *body = provider_request_body(advisor, mi, rq);
    const char *hdrs[4]; sbuf keep; auth_headers(p, advisor, key, hdrs, &keep);
    struct timeval t0, t1; gettimeofday(&t0, NULL);
    term_raw(true);
    http_interrupt_fd = g_cfg.interactive ? term_interrupt_fd() : -1;
    http_interrupt_check = term_poll_interrupt;
    http_idle = idle_tick; http_idle_ud = NULL;
    http_headers = hdrs;
    int idle_was = http_idle_timeout_ms;
    if (rq->idle_ms > 0) http_idle_timeout_ms = rq->idle_ms;
    term_busy(rq->busy ? rq->busy : "consulting");
    sbuf out; sb_init(&out); http_result res;
    const char *path = p->style == PROVIDER_MESSAGES ? "/v1/messages" : "/chat/completions";
    char *zbody; size_t zlen; bool gz = wire_gzip(p, body, false, &zbody, &zlen);
    int rc = http_request_bytes(provider_base_url(p), "POST", path, gz ? zbody : body, gz ? zlen : strlen(body), gz, &out, NULL, NULL, &res);
    if (gz && rc == 0 && !res.aborted && gzip_refused(res.status, out.data)) {   /* refused: the same call plain, once */
        sb_free(&out); sb_init(&out);
        rc = http_request_bytes(provider_base_url(p), "POST", path, body, strlen(body), false, &out, NULL, NULL, &res);
        if (rc == 0 && !res.aborted && res.status >= 200 && res.status <= 299) provider_gzip_refused(p);
    }
    free(zbody);
    term_busy(NULL);
    http_idle_timeout_ms = idle_was;
    http_headers = NULL; http_idle = NULL; http_interrupt_fd = -1;
    gettimeofday(&t1, NULL);
    st->total_seconds = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_usec - t0.tv_usec) / 1e6;
    char *text = NULL;
    ratelimit_set(advisor, &res.rl);
    if (res.aborted) *aborted = true;
    else if (rc != 0) snprintf(err, n, "%s", res.err);
    else text = provider_parse_reply(advisor, out.data, res.status, st, err, n);
    free(body); sb_free(&out); sb_free(&keep);
    return text;
}

/* The headers of one call. Built here, on the main thread: the key is in the environment, and a
 * worker's thread reads neither. The strings live in *keep until the caller has handed them to
 * http_job_start (which copies them) or http_request has returned. */
bool provider_call_headers(const char *model, const char *hdrs[4], sbuf *keep, char *err, size_t n) {
    err[0] = 0;
    const provider_def *p = provider_find(model, NULL);
    if (!p) { snprintf(err, n, "'%s' is not a hosted model", model ? model : ""); return false; }
    const char *key = getenv(p->key_env);
    if (!key || !*key) { snprintf(err, n, "%s is not set — export it before starting corbienest (a NAME=value line without `export` is not passed on)", p->key_env); return false; }
    if (base_insecure(p, err, n)) return false;
    auth_headers(p, model, key, hdrs, keep);
    return true;
}

/* What this call asks the working model for, from ollama_call and the configuration — the same
 * places ollama_chat() reads. A quiet call (think == 0: the memory update, a compaction) sends
 * neither thinking nor an effort, even when a level would otherwise stay for the prompt cache. */
static void turn_ask(const char *model, const model_info *mi, int *max_tokens, const char **effort, bool *thinking, double *temperature) {
    *max_tokens = ollama_call.num_predict > 0 ? ollama_call.num_predict : (mi && mi->max_output > 0 ? mi->max_output : 32000);
    *effort = NULL; *thinking = false;
    bool other = ollama_call.model && !model_same(ollama_call.model, g_cfg.model);
    *temperature = g_cfg.temperature >= 0 && !other ? g_cfg.temperature : -1;
    if (ollama_call.think == 0 || !mi) return;
    const char *level = NULL;
    think_kind k = think_decide(ollama_call.model ? -1 : g_cfg.think, false, ollama_call.followup, effort_get(model), mi, &level);
    if ((k == THINK_TRUE || k == THINK_LEVEL) && (mi->think_adaptive || mi->think_budget)) *thinking = true;
    if (*thinking && k == THINK_LEVEL && level && strcmp(level, "off") && strcmp(level, "on")) *effort = level;
}

char *provider_turn_request(cJSON *messages, cJSON *tools) {
    const char *model = ollama_call.model ? ollama_call.model : g_cfg.model;
    static const model_info no_info;
    const model_info *mi = ollama_call.model ? (ollama_call.info ? ollama_call.info : &no_info) : &g_model_info;
    int max_tokens; const char *effort; bool thinking; double temperature;
    turn_ask(model, mi, &max_tokens, &effort, &thinking, &temperature);
    const char *mid = ""; const provider_def *p = provider_find(model, &mid);
    if (p && p->style == PROVIDER_CHAT_COMPLETIONS) return cc_turn_body(p, mid, messages, tools, max_tokens, temperature);
    return provider_turn_body(model, mi, messages, tools, max_tokens, effort, thinking, temperature, false);
}

/* ---------- a streamed turn ----------
 * Anthropic's stream is Server-Sent Events: "event: …", "data: {…}", a blank line. Text is
 * printed as it arrives, the way Ollama's chunks are; a tool's arguments are accumulated and
 * not shown. At the end the blocks are the same assistant message provider_turn_reply() builds. */
static const char *TURN_SPIN[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };

typedef struct {
    md_state md;
    chat_stats *stats;
    cJSON *blocks;
    sbuf data;
    char event[40];
    char error[512];
    char stop_reason[32];
    char refusal[160];
    bool got_content, thinking_header, spinner_shown;
    int think_tokens, out_chunks, spin_i, index;
    struct timeval t0, t_think;
    double think_secs;
} turn_ctx;

static double turn_elapsed(struct timeval *t0) {
    struct timeval t; gettimeofday(&t, NULL);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_usec - t0->tv_usec) / 1e6;
}
static const char *turn_busy(const char *def) { return ollama_call.busy ? ollama_call.busy : def; }
static void turn_spin_clear(turn_ctx *c) {
    if (c->spinner_shown) { fputs("\r\x1b[2K", stdout); fflush(stdout); c->spinner_shown = false; }
}
static void turn_idle(void *ud) {
    turn_ctx *c = ud;
    term_busy_tick();
    if (!g_cfg.interactive || c->got_content || ollama_quiet) return;
    if (c->thinking_header && g_cfg.show_thinking) return;
    c->spin_i = (c->spin_i + 1) % 10;
    if (c->think_tokens > 0)
        printf("\r\x1b[2K" C_MAGENTA "%s" C_RESET C_DIM " thinking… (%d tokens, %.0fs)  " C_RESET "esc to interrupt", TURN_SPIN[c->spin_i], c->think_tokens, turn_elapsed(&c->t0));
    else
        printf("\r\x1b[2K" C_ORANGE "%s" C_RESET C_DIM " %s… (%.0fs)  " C_RESET "esc to interrupt", TURN_SPIN[c->spin_i],
               turn_elapsed(&c->t0) > 8 ? "waiting" : "thinking", turn_elapsed(&c->t0));
    fflush(stdout);
    c->spinner_shown = true;
}
static void jstr_append(cJSON *obj, const char *key, const char *add) {
    if (!obj || !add || !*add) return;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *cur = cJSON_IsString(v) ? v->valuestring : "";
    size_t a = strlen(cur), b = strlen(add);
    char *s = xmalloc(a + b + 1);
    memcpy(s, cur, a); memcpy(s + a, add, b + 1);
    if (v) cJSON_ReplaceItemInObject(obj, key, cJSON_CreateString(s));
    else cJSON_AddStringToObject(obj, key, s);
    free(s);
}
static cJSON *turn_block(turn_ctx *c, int index) {
    if (index < 0 || index >= TURN_CALLS) return NULL;
    while (cJSON_GetArraySize(c->blocks) <= index) cJSON_AddItemToArray(c->blocks, cJSON_CreateObject());
    return cJSON_GetArrayItem(c->blocks, index);
}
static void turn_close_thinking(turn_ctx *c) {
    if (c->think_tokens && c->think_secs == 0) c->think_secs = turn_elapsed(&c->t_think);
    if (c->thinking_header && g_cfg.show_thinking && !ollama_quiet) { printf(C_RESET "\n\n"); c->thinking_header = false; }
}
static void turn_text(turn_ctx *c, const char *s) {
    if (!s || !*s) return;
    if (ollama_quiet) { term_busy(turn_busy("generating")); return; }
    turn_spin_clear(c);
    turn_close_thinking(c);
    term_busy(turn_busy("generating"));
    if (!c->got_content) {
        fputs(C_ORANGE "●" C_RESET " ", stdout);
        c->got_content = true;
        while (*s == '\n' || *s == ' ') s++;
        if (*s) md_feed(&c->md, s, strlen(s));
    } else md_feed(&c->md, s, strlen(s));
}
static void turn_event(turn_ctx *c, cJSON *j, const char *type) {
    if (!strcmp(type, "error")) {
        cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
        cJSON *m = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "message") : e;
        if (!cJSON_IsString(m)) m = cJSON_GetObjectItemCaseSensitive(j, "message");
        snprintf(c->error, sizeof c->error, "%s", cJSON_IsString(m) ? m->valuestring : "the API refused the call");
        return;
    }
    if (!strcmp(type, "message_start")) {
        cJSON *msg = cJSON_GetObjectItemCaseSensitive(j, "message");
        take_usage(msg ? cJSON_GetObjectItemCaseSensitive(msg, "usage") : NULL, c->stats, true);
        return;
    }
    if (!strcmp(type, "message_delta")) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "delta");
        cJSON *sr = d ? cJSON_GetObjectItemCaseSensitive(d, "stop_reason") : NULL;
        if (cJSON_IsString(sr) && sr->valuestring[0]) snprintf(c->stop_reason, sizeof c->stop_reason, "%s", sr->valuestring);
        cJSON *sd = d ? cJSON_GetObjectItemCaseSensitive(d, "stop_details") : NULL;
        cJSON *cat = sd ? cJSON_GetObjectItemCaseSensitive(sd, "category") : NULL;
        if (!strcmp(c->stop_reason, "refusal"))
            snprintf(c->refusal, sizeof c->refusal, "declined to answer (refusal%s%s)", cJSON_IsString(cat) ? ": " : "", cJSON_IsString(cat) ? cat->valuestring : "");
        take_usage(cJSON_GetObjectItemCaseSensitive(j, "usage"), c->stats, false);
        return;
    }
    if (!strcmp(type, "content_block_start")) {
        cJSON *ix = cJSON_GetObjectItemCaseSensitive(j, "index");
        int index = cJSON_IsNumber(ix) ? (int)ix->valuedouble : c->index + 1;
        cJSON *slot = turn_block(c, index);
        if (!slot) return;
        cJSON *src = cJSON_GetObjectItemCaseSensitive(j, "content_block");
        cJSON *copy = cJSON_IsObject(src) ? cJSON_Duplicate(src, 1) : cJSON_CreateObject();
        cJSON_ReplaceItemInArray(c->blocks, index, copy);
        c->index = index;
        cJSON *ty = cJSON_GetObjectItemCaseSensitive(copy, "type");
        const char *bt = cJSON_IsString(ty) ? ty->valuestring : "";
        if (!strcmp(bt, "thinking")) {
            if (c->think_tokens == 0) { term_busy(turn_busy("thinking")); gettimeofday(&c->t_think, NULL); }
        } else if (!strcmp(bt, "tool_use")) {
            turn_close_thinking(c);
            term_busy(turn_busy("calling tools"));
        }
        return;
    }
    if (!strcmp(type, "content_block_delta")) {
        cJSON *ix = cJSON_GetObjectItemCaseSensitive(j, "index");
        int index = cJSON_IsNumber(ix) ? (int)ix->valuedouble : c->index;
        cJSON *blk = turn_block(c, index);
        cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "delta");
        cJSON *dt = d ? cJSON_GetObjectItemCaseSensitive(d, "type") : NULL;
        const char *k = cJSON_IsString(dt) ? dt->valuestring : "";
        if (!strcmp(k, "text_delta")) {
            cJSON *tx = cJSON_GetObjectItemCaseSensitive(d, "text");
            if (!cJSON_IsString(tx) || !tx->valuestring[0]) return;
            term_status_live(++c->out_chunks); term_busy_tick();
            jstr_append(blk, "text", tx->valuestring);
            turn_text(c, tx->valuestring);
        } else if (!strcmp(k, "thinking_delta")) {
            cJSON *th = cJSON_GetObjectItemCaseSensitive(d, "thinking");
            if (!cJSON_IsString(th) || !th->valuestring[0]) return;
            if (c->think_tokens == 0) { term_busy(turn_busy("thinking")); gettimeofday(&c->t_think, NULL); }
            c->think_tokens++;
            term_status_live(++c->out_chunks); term_busy_tick();
            jstr_append(blk, "thinking", th->valuestring);
            if (g_cfg.show_thinking && !ollama_quiet) {
                turn_spin_clear(c);
                if (!c->thinking_header) { printf(C_MAGENTA "✻ Thinking…" C_RESET "\n" C_GRAY); c->thinking_header = true; }
                fputs(th->valuestring, stdout); fflush(stdout);
            } else if (!c->thinking_header) c->thinking_header = true;
        } else if (!strcmp(k, "signature_delta")) {
            cJSON *sg = cJSON_GetObjectItemCaseSensitive(d, "signature");
            if (cJSON_IsString(sg)) jstr_append(blk, "signature", sg->valuestring);
        } else if (!strcmp(k, "input_json_delta")) {
            cJSON *pj = cJSON_GetObjectItemCaseSensitive(d, "partial_json");
            if (cJSON_IsString(pj)) jstr_append(blk, "_partial", pj->valuestring);
        }
    }
}
static void turn_dispatch(turn_ctx *c) {
    if (!c->data.len) { c->event[0] = 0; return; }
    cJSON *j = cJSON_Parse(c->data.data);
    sb_clear(&c->data);
    char ev[sizeof c->event]; snprintf(ev, sizeof ev, "%s", c->event); c->event[0] = 0;
    if (!j) return;
    cJSON *ty = cJSON_GetObjectItemCaseSensitive(j, "type");
    turn_event(c, j, cJSON_IsString(ty) ? ty->valuestring : ev);
    cJSON_Delete(j);
}
static int turn_line(const char *line, size_t len, void *ud) {
    turn_ctx *c = ud;
    while (len && (*line == ' ' || *line == '\r')) { line++; len--; }
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if (len == 0) { turn_dispatch(c); return 0; }
    if (len >= 6 && !strncmp(line, "event:", 6)) {
        if (c->data.len) turn_dispatch(c);
        const char *v = line + 6; size_t vl = len - 6; while (vl && *v == ' ') { v++; vl--; }
        if (vl >= sizeof c->event) vl = sizeof c->event - 1;
        memcpy(c->event, v, vl); c->event[vl] = 0;
        return 0;
    }
    if (len >= 5 && !strncmp(line, "data:", 5)) {
        const char *v = line + 5; size_t vl = len - 5; while (vl && *v == ' ') { v++; vl--; }
        if (c->data.len) sb_putc(&c->data, '\n');
        sb_append(&c->data, v, vl);
        return 0;
    }
    if (line[0] == '{') {   /* an error body is one JSON line, not an event */
        if (c->data.len) turn_dispatch(c);
        sb_append(&c->data, line, len);
        turn_dispatch(c);
    }
    return 0;
}

/* A streamed turn the server refused as a gzipped body, before anything was shown: it starts again from nothing.
 * The stats and the start time stay; the rest of the context is as a new turn's. */
static void turn_restart(turn_ctx *c) {
    turn_spin_clear(c);
    cJSON_Delete(c->blocks); sb_free(&c->data);
    chat_stats *st = c->stats; struct timeval t0 = c->t0;
    memset(c, 0, sizeof *c);
    c->stats = st; c->t0 = t0; c->index = -1;
    md_init(&c->md); sb_init(&c->data); c->blocks = cJSON_CreateArray();
}

cJSON *provider_turn(cJSON *messages, cJSON *tools, chat_stats *stats, bool *aborted) {
    *aborted = false;
    ollama_error[0] = 0;
    if (stats) memset(stats, 0, sizeof *stats);
    const char *model = ollama_call.model ? ollama_call.model : g_cfg.model;
    const char *mid = ""; const provider_def *p = provider_find(model, &mid);
    if (!p || p->style != PROVIDER_MESSAGES) {
        snprintf(ollama_error, sizeof ollama_error, "'%s' is not an Anthropic model", model ? model : "");
        printf(C_RED "✗ %s" C_RESET "\n", ollama_error);
        return NULL;
    }
    static const model_info no_info;
    const model_info *mi = ollama_call.model ? (ollama_call.info ? ollama_call.info : &no_info) : &g_model_info;
    int max_tokens; const char *effort; bool thinking; double temperature;
    turn_ask(model, mi, &max_tokens, &effort, &thinking, &temperature);
    char *body = provider_turn_body(model, mi, messages, tools, max_tokens, effort, thinking, temperature, true);
    char err[512]; const char *hdrs[4]; sbuf keep;
    if (!provider_call_headers(model, hdrs, &keep, err, sizeof err)) {
        snprintf(ollama_error, sizeof ollama_error, "%s", err);
        printf(C_RED "✗ %s" C_RESET "\n", ollama_error);
        free(body);
        return NULL;
    }
    turn_ctx c; memset(&c, 0, sizeof c);
    md_init(&c.md); sb_init(&c.data); c.blocks = cJSON_CreateArray(); c.stats = stats; c.index = -1;
    gettimeofday(&c.t0, NULL);
    term_raw(true);
    http_interrupt_fd = g_cfg.interactive ? term_interrupt_fd() : -1;
    http_interrupt_check = term_poll_interrupt;
    http_idle = turn_idle; http_idle_ud = &c;
    http_headers = hdrs;
    term_busy(turn_busy("waiting for model"));
    turn_idle(&c);
    http_result res;
    int idle_was = http_idle_timeout_ms;
    if (ollama_call.idle_ms > 0) http_idle_timeout_ms = ollama_call.idle_ms;
    char *zbody; size_t zlen; bool gz = wire_gzip(p, body, false, &zbody, &zlen);
    int rc = http_request_bytes(provider_base_url(p), "POST", "/v1/messages", gz ? zbody : body, gz ? zlen : strlen(body), gz, NULL, turn_line, &c, &res);
    if (gz && rc == 0 && !res.aborted && gzip_refused(res.status, c.error) && !c.got_content && !c.think_tokens && !c.out_chunks) {
        turn_restart(&c);   /* nothing was shown: the same call plain, once */
        rc = http_request_bytes(provider_base_url(p), "POST", "/v1/messages", body, strlen(body), false, NULL, turn_line, &c, &res);
        if (rc == 0 && !res.aborted && res.status >= 200 && res.status <= 299) provider_gzip_refused(p);
    }
    free(zbody);
    http_idle_timeout_ms = idle_was;
    http_headers = NULL; http_idle = NULL; http_interrupt_fd = -1;
    term_busy(NULL);
    ratelimit_set(model, &res.rl);
    free(body); sb_free(&keep);
    if (c.data.len) turn_dispatch(&c);
    term_status_live(0);
    turn_spin_clear(&c);
    if (c.got_content) md_finish(&c.md);
    else if (c.thinking_header && g_cfg.show_thinking && !ollama_quiet) fputs(C_RESET, stdout);
    struct timeval t1; gettimeofday(&t1, NULL);
    if (stats) {
        stats->total_seconds = (double)(t1.tv_sec - c.t0.tv_sec) + (double)(t1.tv_usec - c.t0.tv_usec) / 1e6;
        if (c.think_tokens) { stats->think_chunks = c.think_tokens; stats->think_seconds = c.think_secs > 0 ? c.think_secs : turn_elapsed(&c.t_think); }
    }
    cJSON *msg = NULL;
    bool failed = false;
    if (res.aborted) {
        *aborted = true;
        if (!ollama_quiet) printf("\n" C_YELLOW "⏹ interrupted" C_RESET "\n");
    } else if (rc != 0) {
        failed = true;
        snprintf(ollama_error, sizeof ollama_error, "%s", res.err);
        printf(C_RED "✗ request failed: %s" C_RESET "\n", res.err);
        if (strstr(res.err, "connect")) printf(C_DIM "  is %s reachable? (%s)" C_RESET "\n", p->label, provider_base_url(p));
    } else if (c.error[0] || res.status >= 400 || res.status == 0) {
        failed = true;
        if (!c.error[0]) status_error(p, mid, res.status, "", ollama_error, sizeof ollama_error);
        else snprintf(ollama_error, sizeof ollama_error, "%s", c.error);
        printf(C_RED "✗ %s" C_RESET "\n", ollama_error);
    }
    if (res.aborted || !failed) {
        if (c.got_content && !ollama_quiet) printf("\n");
        msg = assistant_from_blocks(c.blocks, c.stop_reason, c.refusal, stats);
        if (res.aborted) {
            cJSON *ct = cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (!cJSON_IsString(ct) || !ct->valuestring[0]) { cJSON_Delete(msg); msg = NULL; }
            else { cJSON_DeleteItemFromObject(msg, "tool_calls"); cJSON_DeleteItemFromObject(msg, "provider_content"); }
        }
    }
    cJSON_Delete(c.blocks); sb_free(&c.data);
    fflush(stdout);
    return msg;
}
