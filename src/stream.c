/* stream-json: the conversation as JSON lines (see DOCUMENTATION.md, "Scripts: stream-json"). Pure: the caller decides
 * where the lines go (main.c writes them to the real stdout). */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a tool call the API gave no id gets one, and its result takes the oldest call not yet answered */
void stream_reset(stream_st *st) {
    for (int i = 0; i < st->npend; i++) free(st->pend[i]);
    st->npend = 0;
}
static const char *call_id(cJSON *call) {
    cJSON *id = call ? cJSON_GetObjectItemCaseSensitive(call, "id") : NULL;
    return cJSON_IsString(id) && id->valuestring[0] ? id->valuestring : NULL;
}
static const char *call_fn_name(cJSON *call) {
    cJSON *fn = call ? cJSON_GetObjectItemCaseSensitive(call, "function") : NULL;
    cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
    return cJSON_IsString(nm) ? nm->valuestring : "?";
}

/* One line per message of msgs from index `from` on: the user's text, a reply (its text and its tool calls as tool_use),
 * a tool result (as tool_result). A system message is not part of the stream. Appends to out, each line ending in \n. */
void stream_messages(stream_st *st, cJSON *msgs, int from, const char *session_id, sbuf *out) {
    int n = cJSON_GetArraySize(msgs);
    for (int idx = from; idx < n; idx++) {
        cJSON *m = cJSON_GetArrayItem(msgs, idx);
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        const char *role = cJSON_IsString(r) ? r->valuestring : "";
        cJSON *o = cJSON_CreateObject(), *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "session_id", session_id ? session_id : "");
        cJSON_AddStringToObject(msg, "role", "user");
        if (!strcmp(role, "user") && cJSON_IsString(c)) {
            cJSON_AddStringToObject(o, "type", "user"); cJSON_AddStringToObject(msg, "content", c->valuestring);
        } else if (!strcmp(role, "assistant")) {
            cJSON_AddStringToObject(o, "type", "assistant"); cJSON_AddStringToObject(msg, "role", "assistant");
            cJSON *blocks = cJSON_AddArrayToObject(msg, "content");
            if (cJSON_IsString(c) && c->valuestring[0]) {
                cJSON *t = cJSON_CreateObject();
                cJSON_AddStringToObject(t, "type", "text"); cJSON_AddStringToObject(t, "text", c->valuestring);
                cJSON_AddItemToArray(blocks, t);
            }
            cJSON *call, *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
            cJSON_ArrayForEach(call, calls) {
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function"), *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                cJSON *input = cJSON_IsString(args) ? cJSON_Parse(args->valuestring) : cJSON_Duplicate(args, 1);
                if (!input) input = cJSON_CreateObject();
                char gen[48]; const char *cid = call_id(call);
                if (!cid || !*cid) { snprintf(gen, sizeof gen, "toolu_%d_%d", idx, st->npend); cid = gen; }
                if (st->npend < STREAM_PEND_MAX) st->pend[st->npend++] = xstrdup(cid);
                cJSON *t = cJSON_CreateObject();
                cJSON_AddStringToObject(t, "type", "tool_use"); cJSON_AddStringToObject(t, "id", cid);
                cJSON_AddStringToObject(t, "name", call_fn_name(call)); cJSON_AddItemToObject(t, "input", input);
                cJSON_AddItemToArray(blocks, t);
            }
        } else if (!strcmp(role, "tool")) {
            cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "content"), *tid = cJSON_GetObjectItemCaseSensitive(m, "tool_call_id");
            cJSON *block = cJSON_CreateObject(), *blocks = cJSON_AddArrayToObject(msg, "content");
            cJSON_AddStringToObject(o, "type", "user");
            cJSON_AddStringToObject(block, "type", "tool_result");
            char *own = NULL;   /* the oldest call not yet answered, when the result names none */
            if (!(cJSON_IsString(tid) && tid->valuestring[0]) && st->npend > 0) {
                own = st->pend[0];
                memmove(st->pend, st->pend + 1, sizeof(char*) * (size_t)(--st->npend));
            }
            cJSON_AddStringToObject(block, "tool_use_id", own ? own : cJSON_IsString(tid) ? tid->valuestring : "");
            free(own);
            cJSON_AddStringToObject(block, "content", cJSON_IsString(tc) ? tc->valuestring : "");
            cJSON_AddItemToArray(blocks, block);
        } else { cJSON_Delete(o); cJSON_Delete(msg); continue; }
        cJSON_AddItemToObject(o, "message", msg);
        char *t = cJSON_PrintUnformatted(o);
        if (t) { sb_puts(out, t); sb_putc(out, '\n'); }
        free(t); cJSON_Delete(o);
    }
}

/* The text of one input line of --input-format stream-json: {"type":"user","message":{"role":"user","content":...}}, with
 * content a string or a list of text blocks. NULL (and a message on stderr) for anything else. Malloc'd. */
char *stream_input_text(const char *line) {
    cJSON *o = cJSON_Parse(line);
    if (!o) { fprintf(stderr, "corbienest: a stream-json input line is not JSON: %.80s\n", line); return NULL; }
    cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "message");
    cJSON *c = cJSON_GetObjectItemCaseSensitive(m ? m : o, "content");
    char *text = NULL;
    if (cJSON_IsString(c)) text = xstrdup(c->valuestring);
    else if (cJSON_IsArray(c)) {
        sbuf b; sb_init(&b);
        cJSON *blk;
        cJSON_ArrayForEach(blk, c) { cJSON *t = cJSON_GetObjectItemCaseSensitive(blk, "text"); if (cJSON_IsString(t)) sb_printf(&b, "%s%s", b.len ? "\n" : "", t->valuestring); }
        text = sb_detach(&b);
    }
    cJSON_Delete(o);
    return text;
}
