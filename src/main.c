/* corbienest — a Claude-Code-style terminal coding agent for local Ollama models. */
#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static cJSON *g_messages = NULL;    /* conversation, without system prompt */
static cJSON *g_tools = NULL;       /* tool definitions */
static bool   g_model_tools = true; /* current model supports tools */
static int    g_model_max_ctx = 0;  /* context length the model was trained for (0 = unknown) */
static char   g_cwd[PATH_MAX];
static char  *g_project_instructions = NULL;
static char  *g_memory = NULL;                    /* contents of MEMORY_PATH (see memory_*) */

static char   g_session_id[64];                  /* current session (file stem under config_dir()/sessions) */

static const char *SLASH_CMDS[] = {
    "/help", "/model", "/models", "/clear", "/compact", "/status", "/system", "/think",
    "/mode", "/yolo", "/tools", "/ctx", "/temp", "/host", "/save", "/history", "/cd", "/pwd", "/skills", "/memory", "/resume", "/permissions", "/init", "/quit", "/exit"
};

/* slash completion list = built-in commands + /skill names (rebuilt when skills reload) */
static const char **g_slash_all = NULL;
static void refresh_slash_completion(void) {
    int nb = (int)(sizeof SLASH_CMDS / sizeof *SLASH_CMDS), ns = skills_count();
    static char **owned = NULL; static int nowned = 0;
    for (int i = 0; i < nowned; i++) free(owned[i]);
    free(owned); free(g_slash_all);
    owned = xmalloc(sizeof(char*) * (size_t)(ns ? ns : 1)); nowned = ns;
    g_slash_all = xmalloc(sizeof(char*) * (size_t)(nb + ns));
    for (int i = 0; i < nb; i++) g_slash_all[i] = SLASH_CMDS[i];
    for (int i = 0; i < ns; i++) {
        sbuf b; sb_init(&b); sb_printf(&b, "/%s", skill_get(i)->name);
        owned[i] = sb_detach(&b); g_slash_all[nb + i] = owned[i];
    }
    term_set_slash_commands(g_slash_all, nb + ns);
}

/* ---------- project memory ----------
 * Like Claude Code's auto-memory: a per-project markdown file (MEMORY_PATH, in the working
 * directory) that the *model* curates. It is loaded into the system prompt of every request,
 * the model may edit it directly with edit_file, and at the end of every request a quiet
 * extraction call asks the model whether this exchange taught it anything durable — who the
 * user is, feedback on how to work, project goals/decisions/constraints, references — and,
 * if so, to return the updated file (or NO_CHANGE). Off with /memory off (saved in config). */
#define MEMORY_PATH   ".corbienest/memory.md"
#define MEMORY_MAX    (24 * 1024)     /* larger files are cut when loaded; the model is told to keep it short */

static void load_memory(void) {
    free(g_memory); g_memory = NULL;
    if (!is_file(MEMORY_PATH)) return;
    size_t n; char *d = read_whole_file(MEMORY_PATH, &n, MEMORY_MAX);
    if (!d) return;
    if (n >= MEMORY_MAX) { char *nl = strrchr(d, '\n'); if (nl) *nl = 0; }
    g_memory = d;
}

static const char *MEMORY_TEMPLATE =
    "# Project memory\n\n"
    "<!-- Kept by corbienest (" MEMORY_PATH "). Loaded into every request; updated by the model after each request\n"
    "     and editable by hand. One durable fact per bullet; keep it short. -->\n\n"
    "## User\n\n## Feedback\n\n## Project\n\n## Reference\n";

static const char *MEMORY_RULES =
    "The memory file has four sections. Save only what will still matter in a future session:\n"
    "- User: who the user is (role, expertise, preferences, tools they like).\n"
    "- Feedback: guidance the user gave on how to work — corrections and confirmed approaches — with the why.\n"
    "- Project: goals, decisions, constraints and ongoing work that cannot be derived from the code or git history; write dates as absolute dates.\n"
    "- Reference: pointers to external things (URLs, tickets, dashboards, hosts).\n"
    "Do NOT save what the repository already records (code structure, what a file contains, fixes just made), transient task state, "
    "or anything only relevant to the current conversation. Update or delete facts that turned out wrong; merge duplicates. "
    "Keep the whole file under ~80 lines, one bullet per fact.";

/* Strip an optional ```markdown fence around the model's reply. Returns a malloc'd string. */
static char *unfence(const char *t) {
    while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t') t++;
    size_t n = strlen(t);
    while (n && (t[n-1] == ' ' || t[n-1] == '\n' || t[n-1] == '\r' || t[n-1] == '\t')) n--;
    if (n >= 6 && !strncmp(t, "```", 3) && !strncmp(t + n - 3, "```", 3)) {
        const char *b = strchr(t, '\n'); if (!b || b >= t + n - 3) return xstrndup(t, n);
        b++; size_t e = n - 3; while (e > (size_t)(b - t) && (t[e-1] == '\n' || t[e-1] == ' ')) e--;
        return xstrndup(b, e - (size_t)(b - t));
    }
    return xstrndup(t, n);
}

/* End of a request: ask the model (quietly, without tools) to fold anything worth
 * remembering from this exchange into the memory file. `first` is the index in
 * g_messages where the request started. In one-shot (-p) runs the file is only touched
 * if the project directory already exists, so `corbienest -p` leaves no litter. */
static void memory_update(int first, bool aborted) {
    if (!g_cfg.memory || aborted) return;
    if (!g_cfg.interactive && !is_dir(".corbienest")) return;
    int total = cJSON_GetArraySize(g_messages);
    if (first < 0 || first >= total) first = 0;   /* e.g. auto-compact replaced the conversation */
    if (total - first < 2) return;                 /* no reply: nothing to learn */
    cJSON *msgs = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    sbuf sp; sb_init(&sp);
    sb_printf(&sp, "You maintain the persistent memory file %s of a coding assistant. You are shown the exchange that just finished. %s\n\n"
                   "Current memory file:\n\n%s", MEMORY_PATH, MEMORY_RULES, g_memory ? g_memory : MEMORY_TEMPLATE);
    cJSON_AddStringToObject(sys, "content", sp.data); sb_free(&sp);
    cJSON_AddItemToArray(msgs, sys);
    for (int i = first; i < total; i++) {
        cJSON *m = cJSON_GetArrayItem(g_messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        const char *r = cJSON_IsString(role) ? role->valuestring : "user";
        cJSON *n = cJSON_CreateObject();
        /* keep it plain: tool results become user text (no tools are offered on this call) */
        cJSON_AddStringToObject(n, "role", !strcmp(r, "assistant") ? "assistant" : "user");
        sbuf b; sb_init(&b);
        if (!strcmp(r, "tool")) { cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name"); sb_printf(&b, "[result of %s]\n", cJSON_IsString(tn) ? tn->valuestring : "tool"); }
        if (cJSON_IsString(c) && c->valuestring[0]) {
            size_t len = strlen(c->valuestring), cap = 4000;
            sb_append(&b, c->valuestring, len < cap ? len : cap);
            if (len > cap) sb_puts(&b, "\n[…truncated]");
        }
        if (cJSON_IsArray(tc)) { char *t = cJSON_PrintUnformatted(tc); sb_printf(&b, "%s[tool calls] %.600s", b.len ? "\n" : "", t); free(t); }
        if (!b.len) sb_puts(&b, "(empty)");
        cJSON_AddStringToObject(n, "content", b.data); sb_free(&b);
        cJSON_AddItemToArray(msgs, n);
    }
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content",
        "Update the memory file with anything durable this exchange revealed, following the rules. "
        "If nothing is worth saving, reply with exactly NO_CHANGE. Otherwise reply with the complete updated memory file "
        "in markdown, keeping the existing headings, and nothing else.");
    cJSON_AddItemToArray(msgs, u);

    if (g_cfg.interactive) { printf(C_DIM "✎ updating memory…" C_RESET); fflush(stdout); }
    chat_stats st; bool ab = false;
    ollama_quiet = true;
    cJSON *reply = ollama_chat(msgs, NULL, &st, &ab);
    ollama_quiet = false;
    cJSON_Delete(msgs);
    g_session.prompt_tokens += st.prompt_tokens; g_session.eval_tokens += st.eval_tokens;
    if (g_cfg.interactive) { fputs("\r\x1b[2K", stdout); term_status_refresh(); }
    if (!reply) return;
    cJSON *c = cJSON_GetObjectItemCaseSensitive(reply, "content");
    char *text = unfence(cJSON_IsString(c) ? c->valuestring : "");
    cJSON_Delete(reply);
    bool nochange = ab || !*text || strstr(text, "NO_CHANGE") || *text != '#' || strlen(text) > 2 * MEMORY_MAX;
    if (!nochange && (!g_memory || strcmp(text, g_memory))) {
        size_t n = strlen(text);
        if (n && text[n-1] != '\n') { text = xrealloc(text, n + 2); text[n++] = '\n'; text[n] = 0; }
        if (mkdir_p(".corbienest") == 0 && write_whole_file(MEMORY_PATH, text, n) == 0) {
            load_memory();
            printf(C_DIM "✎ memory updated (%s)" C_RESET "\n", MEMORY_PATH);
        }
    }
    free(text);
}

static void cmd_memory(const char *arg) {
    if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
        g_cfg.memory = !strcmp(arg, "on"); config_save();
        printf(C_GREEN "✓ memory %s" C_RESET "\n", g_cfg.memory ? "on: " MEMORY_PATH " is updated after each request" : "off");
        return;
    }
    if (arg && !strcmp(arg, "clear")) {
        if (is_file(MEMORY_PATH) && unlink(MEMORY_PATH) == 0) { load_memory(); printf(C_GREEN "✓ removed %s" C_RESET "\n", MEMORY_PATH); }
        else printf(C_DIM "no memory file" C_RESET "\n");
        return;
    }
    if (arg && *arg) { printf("usage: /memory [on|off|clear]\n"); return; }
    printf(C_DIM "%s — %s; loaded into the system prompt · /memory on|off|clear" C_RESET "\n", MEMORY_PATH, g_cfg.memory ? "updated after each request" : "auto-update is off");
    if (!g_memory) { printf(C_DIM "(no memory yet — nothing durable has been recorded)" C_RESET "\n"); return; }
    fputs(g_memory, stdout);
    if (g_memory[strlen(g_memory) - 1] != '\n') printf("\n");
}

/* ---------- sessions (--continue / --resume / /resume) ----------
 * Every conversation is persisted as JSON under config_dir()/sessions/<id>.json after each
 * request (id = start time + pid). --continue reloads the latest session started in this
 * working directory, --resume [ID] / /resume [ID] pick one from a menu (or by id). The
 * latest SESSIONS_KEEP files are kept. */
#define SESSIONS_KEEP 100
static void print_result_preview(const char *text, int lines);

static const char *sessions_dir(void) {
    static char d[1200];
    if (!d[0]) { snprintf(d, sizeof d, "%s/sessions", config_dir()); mkdir_p(d); }
    return d;
}

static void session_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/%s.json", sessions_dir(), id); }

/* first line of the first user message, for menus */
static char *session_title_of(cJSON *messages) {
    cJSON *m;
    cJSON_ArrayForEach(m, messages) {
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(r) && !strcmp(r->valuestring, "user") && cJSON_IsString(c) && c->valuestring[0]) {
            const char *t = c->valuestring; while (*t == ' ' || *t == '\n') t++;
            size_t n = strcspn(t, "\n"); if (n > 70) n = 70;
            while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
            char *out = xmalloc(n + 4); memcpy(out, t, n); out[n] = 0;
            if (t[n]) strcat(out, "…");
            return out;
        }
    }
    return xstrdup("(empty)");
}

static void sessions_prune(void) {
    DIR *d = opendir(sessions_dir()); if (!d) return;
    struct { char name[128]; time_t mt; } *ents = NULL; int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", sessions_dir(), e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; ents = xrealloc(ents, sizeof *ents * (size_t)cap); }
        snprintf(ents[n].name, sizeof ents[n].name, "%s", e->d_name); ents[n].mt = st.st_mtime; n++;
    }
    closedir(d);
    while (n > SESSIONS_KEEP) {   /* drop the oldest one at a time (n is small) */
        int oldest = 0; for (int i = 1; i < n; i++) if (ents[i].mt < ents[oldest].mt) oldest = i;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", sessions_dir(), ents[oldest].name); unlink(p);
        ents[oldest] = ents[--n];
    }
    free(ents);
}

static void session_new_id(void) {
    time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(g_session_id, sizeof g_session_id, "%s-%d", ts, (int)getpid());
}

static void session_save(void) {
    if (!g_messages || cJSON_GetArraySize(g_messages) == 0) return;
    if (!g_session_id[0]) session_new_id();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", g_session_id);
    cJSON_AddStringToObject(o, "cwd", g_cwd);
    cJSON_AddStringToObject(o, "model", g_cfg.model ? g_cfg.model : "");
    cJSON_AddNumberToObject(o, "updated", (double)time(NULL));
    char *title = session_title_of(g_messages); cJSON_AddStringToObject(o, "title", title); free(title);
    cJSON_AddNumberToObject(o, "prompt_tokens", (double)g_session.prompt_tokens);
    cJSON_AddNumberToObject(o, "eval_tokens", (double)g_session.eval_tokens);
    cJSON_AddItemReferenceToObject(o, "messages", g_messages);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char path[1400]; session_path(g_session_id, path, sizeof path);
    write_whole_file(path, txt, strlen(txt));
    free(txt);
    sessions_prune();
}

typedef struct { char id[128]; char *title; char *cwd; char *model; int nmsg; time_t updated; } session_info;

static int session_cmp(const void *a, const void *b) {
    const session_info *x = a, *y = b;
    return x->updated < y->updated ? 1 : x->updated > y->updated ? -1 : 0;
}

/* newest first; if only_cwd, just the sessions started in the current directory */
static session_info *sessions_list(bool only_cwd, int *count) {
    session_info *v = NULL; int n = 0, cap = 0;
    DIR *d = opendir(sessions_dir());
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || l >= 128 || strcmp(e->d_name + l - 5, ".json")) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", sessions_dir(), e->d_name);
        size_t sz; char *txt = read_whole_file(p, &sz, 64 * 1024 * 1024);
        if (!txt) continue;
        cJSON *o = cJSON_Parse(txt); free(txt);
        if (!o) continue;
        cJSON *cwd = cJSON_GetObjectItemCaseSensitive(o, "cwd");
        if (only_cwd && !(cJSON_IsString(cwd) && !strcmp(cwd->valuestring, g_cwd))) { cJSON_Delete(o); continue; }
        if (n == cap) { cap = cap ? cap * 2 : 32; v = xrealloc(v, sizeof *v * (size_t)cap); }
        session_info *si = &v[n++]; memset(si, 0, sizeof *si);
        snprintf(si->id, sizeof si->id, "%.*s", (int)(l - 5), e->d_name);
        cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "title"), *m = cJSON_GetObjectItemCaseSensitive(o, "model"), *u = cJSON_GetObjectItemCaseSensitive(o, "updated");
        si->title = xstrdup(cJSON_IsString(t) ? t->valuestring : "?");
        si->cwd = xstrdup(cJSON_IsString(cwd) ? cwd->valuestring : "?");
        si->model = xstrdup(cJSON_IsString(m) ? m->valuestring : "?");
        si->updated = cJSON_IsNumber(u) ? (time_t)u->valuedouble : 0;
        si->nmsg = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "messages"));
        cJSON_Delete(o);
    }
    if (d) closedir(d);
    if (n) qsort(v, (size_t)n, sizeof *v, session_cmp);
    *count = n;
    return v;
}
static void sessions_free(session_info *v, int n) { for (int i = 0; i < n; i++) { free(v[i].title); free(v[i].cwd); free(v[i].model); } free(v); }

/* Load a session into g_messages; returns false if not found / unreadable. */
static bool session_load(const char *id) {
    char path[1400]; session_path(id, path, sizeof path);
    size_t sz; char *txt = read_whole_file(path, &sz, 64 * 1024 * 1024);
    if (!txt) return false;
    cJSON *o = cJSON_Parse(txt); free(txt);
    if (!o) return false;
    cJSON *msgs = cJSON_DetachItemFromObjectCaseSensitive(o, "messages");
    if (!cJSON_IsArray(msgs)) { if (msgs) cJSON_Delete(msgs); cJSON_Delete(o); return false; }
    cJSON_Delete(g_messages); g_messages = msgs;
    tools_reset_permissions();
    snprintf(g_session_id, sizeof g_session_id, "%s", id);
    cJSON *pt = cJSON_GetObjectItemCaseSensitive(o, "prompt_tokens"), *et = cJSON_GetObjectItemCaseSensitive(o, "eval_tokens");
    g_session.prompt_tokens = cJSON_IsNumber(pt) ? (long)pt->valuedouble : 0;
    g_session.eval_tokens = cJSON_IsNumber(et) ? (long)et->valuedouble : 0;
    g_session.last_prompt_tokens = 0;
    cJSON *cwd = cJSON_GetObjectItemCaseSensitive(o, "cwd");
    /* recap: title, size, and how the last exchange ended */
    char *title = session_title_of(g_messages);
    int n = cJSON_GetArraySize(g_messages);
    printf(C_GREEN "✓ resumed session %s" C_RESET C_DIM " · %d message%s · %s%s%s" C_RESET "\n", id, n, n == 1 ? "" : "s", title,
           cJSON_IsString(cwd) && strcmp(cwd->valuestring, g_cwd) ? " · started in " : "", cJSON_IsString(cwd) && strcmp(cwd->valuestring, g_cwd) ? cwd->valuestring : "");
    free(title);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *m = cJSON_GetArrayItem(g_messages, i);
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role"), *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(r) && !strcmp(r->valuestring, "assistant") && cJSON_IsString(c) && c->valuestring[0]) {
            printf(C_DIM "  last reply:" C_RESET "\n");
            print_result_preview(c->valuestring, 6);
            break;
        }
    }
    cJSON_Delete(o);
    term_status_refresh();
    return true;
}

/* /resume [ID|all]: menu of sessions (this directory first; "all" for every directory) */
static void cmd_resume(const char *arg) {
    if (arg && *arg && strcmp(arg, "all")) { if (!session_load(arg)) printf(C_RED "no session %s" C_RESET " — /resume lists them\n", arg); return; }
    bool all = arg && !strcmp(arg, "all");
    int n; session_info *v = sessions_list(!all, &n);
    if (!n && !all) { sessions_free(v, n); v = sessions_list(false, &n); all = true; }
    if (!n) { printf(C_DIM "no saved sessions yet" C_RESET "\n"); sessions_free(v, n); return; }
    if (!g_cfg.interactive) { for (int i = 0; i < n; i++) printf("%s  %s\n", v[i].id, v[i].title); sessions_free(v, n); return; }
    const char **items = xmalloc(sizeof(char*) * (size_t)n), **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **bufs = xmalloc(sizeof(char*) * (size_t)n * 2);
    for (int i = 0; i < n; i++) {
        char when[32]; strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&v[i].updated));
        sbuf a; sb_init(&a); sb_printf(&a, "%s  %s", when, v[i].title); bufs[2*i] = sb_detach(&a); items[i] = bufs[2*i];
        sbuf b; sb_init(&b); sb_printf(&b, "%d messages · %s", v[i].nmsg, v[i].model);
        if (strcmp(v[i].cwd, g_cwd)) sb_printf(&b, " · %s", v[i].cwd);
        if (!strcmp(v[i].id, g_session_id)) sb_puts(&b, " · current");
        bufs[2*i+1] = sb_detach(&b); descs[i] = bufs[2*i+1];
    }
    int r = term_select(all ? "Resume a session (all directories)" : "Resume a session (this directory; /resume all for every directory)", items, descs, n, 0);
    if (r >= 0) session_load(v[r].id);
    else printf(C_DIM "cancelled" C_RESET "\n");
    for (int i = 0; i < 2 * n; i++) free(bufs[i]);
    free(bufs); free(items); free(descs); sessions_free(v, n);
}

/* ---------- /permissions ---------- */
static void cmd_permissions(const char *arg) {
    if (arg && !strncmp(arg, "add ", 4)) {
        const char *r = arg + 4; while (*r == ' ') r++;
        if (strcmp(r, "edit") && strncmp(r, "bash ", 5)) { printf("usage: /permissions add edit | add bash <leading words>\n"); return; }
        printf(tools_permissions_add(r) ? C_GREEN "✓ added: %s" C_RESET "\n" : C_DIM "already there: %s" C_RESET "\n", r);
        return;
    }
    if (arg && !strncmp(arg, "remove ", 7)) {
        int i = atoi(arg + 7);
        if (tools_permissions_remove(i - 1)) printf(C_GREEN "✓ removed rule %d" C_RESET "\n", i); else printf(C_RED "no rule %d" C_RESET "\n", i);
        return;
    }
    if (arg && !strcmp(arg, "clear")) { tools_permissions_clear(); printf(C_GREEN "✓ project permissions cleared" C_RESET "\n"); return; }
    if (arg && *arg) { printf("usage: /permissions [add edit|add bash <words>|remove N|clear]\n"); return; }
    int n = tools_permissions_count();
    printf(C_BOLD "project permissions" C_RESET C_DIM " (.corbienest/permissions — answers of \"always allow … in this project\")" C_RESET "\n");
    if (!n) printf(C_DIM "  none — pick \"always allow … in this project\" (p) at a confirmation, or /permissions add bash git status" C_RESET "\n");
    for (int i = 0; i < n; i++) {
        const char *r = tools_permissions_get(i);
        if (!strcmp(r, "edit")) printf("  %2d. file writes and edits\n", i + 1);
        else printf("  %2d. shell commands starting with " C_BOLD "%s" C_RESET "\n", i + 1, r + 5);
    }
    if (n) printf(C_DIM "  /permissions remove N · /permissions clear" C_RESET "\n");
}

/* ---------- system prompt ---------- */
static void load_project_instructions(void) {
    const char *names[] = { "CORBIENEST.md", "CLAUDE.md", "AGENTS.md", NULL };
    free(g_project_instructions); g_project_instructions = NULL;
    for (int i = 0; names[i]; i++) {
        if (is_file(names[i])) {
            size_t n; char *d = read_whole_file(names[i], &n, 32 * 1024);
            if (d) {
                sbuf b; sb_init(&b);
                sb_printf(&b, "\n\n# Project instructions (from %s)\n%s", names[i], d);
                g_project_instructions = sb_detach(&b);
                free(d);
            }
            break;
        }
    }
}

static char *build_system_prompt(void) {
    sbuf b; sb_init(&b);
    struct utsname u; uname(&u);
    time_t now = time(NULL); char date[64]; strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
    bool git = is_dir(".git");
    sb_puts(&b,
        "You are Corbie Nest (corbienest), an expert software engineering assistant running as a coding agent inside the user's terminal. "
        "You help with programming tasks: exploring and understanding codebases, writing new code, editing existing code, "
        "running shell and git commands, building, testing and debugging.\n\n");
    sb_printf(&b, "# Environment\n- Working directory: %s\n- OS: %s %s\n- Shell: /bin/sh\n- Date: %s\n- Git repository: %s\n",
              g_cwd, u.sysname, u.machine, date, git ? "yes" : "no");
    if (g_model_tools && !g_cfg.no_tools) {
        sb_puts(&b,
            "\n# Tools\nYou have these tools: read_file, write_file, edit_file, list_dir, grep, bash. Use them proactively "
            "instead of asking the user to paste files or run commands for you.\n"
            "- Explore first: use list_dir / grep / read_file to understand relevant code before changing it.\n"
            "- Always read a file before editing it. Use edit_file for targeted changes (old_string must match exactly); "
            "use write_file only for new files or full rewrites.\n"
            "- Use bash for shell work: git (status, diff, log, add, commit, branch...), building, running tests, "
            "package managers, find, etc. Commands run non-interactively with /bin/sh in the working directory. "
            "Never run interactive programs (editors, pagers, prompts).\n"
            "- The user must approve each write, edit and shell command, so proceed and call the tool rather than asking permission in text.\n"
            "- After making code changes, verify them when practical (compile, run tests, or run the program).\n"
            "- Do not fabricate tool results. If a tool errors, read the error and adjust.\n"
            "- When a task is done, briefly summarise what you changed and how you verified it.\n");
    } else {
        sb_puts(&b, "\n(No tools are available in this session; if you need file contents or command output, ask the user to provide them.)\n");
    }
    sb_puts(&b,
        "\n# Style\n- Be concise and direct. Do not pad answers with pleasantries.\n"
        "- Use markdown lightly (code blocks for code, short lists). Reference code as path:line.\n"
        "- When writing code, follow the existing conventions of the project and write complete, working code.\n");
    if (g_cfg.mode == MODE_PLAN)
        sb_puts(&b, "\n# Plan mode (read-only)\n"
            "The user has put you in plan mode. Explore the codebase with read_file, list_dir, grep and read-only shell commands, "
            "then present a concrete implementation plan as text: which files to change and how, the order of steps, and any risks or open questions. "
            "Do NOT modify files in this mode — write_file and edit_file are unavailable. When the plan is approved the user will switch modes and ask you to implement it.\n");
    if (g_model_tools && !g_cfg.no_tools) { char *sk = skills_prompt_section(); if (sk) { sb_puts(&b, sk); free(sk); } }
    if (g_cfg.system_prompt && *g_cfg.system_prompt) sb_printf(&b, "\n# Additional instructions from the user\n%s\n", g_cfg.system_prompt);
    if (g_project_instructions) sb_puts(&b, g_project_instructions);
    if (g_memory) sb_printf(&b, "\n\n# Project memory (from " MEMORY_PATH ")\n"
        "Facts remembered from earlier sessions. Verify anything that names a file, function or flag before relying on it. "
        "You may edit this file with edit_file when the user asks you to remember or forget something; it is also refreshed automatically after each request.\n\n%s", g_memory);
    return sb_detach(&b);
}

/* messages array with system prompt prepended (references, so cheap) */
static cJSON *messages_with_system(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    char *sp = build_system_prompt();
    cJSON_AddStringToObject(sys, "content", sp);
    free(sp);
    cJSON_AddItemToArray(arr, sys);
    cJSON *m; cJSON_ArrayForEach(m, g_messages) cJSON_AddItemReferenceToArray(arr, m);
    return arr;
}

static void add_message(const char *role, const char *content) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(g_messages, m);
}

/* ---------- model info ---------- */
static void refresh_model_caps(bool quiet) {
    cJSON *list = ollama_list_models();
    if (!list) return;
    bool found = false;
    cJSON *m;
    if (!g_cfg.model) {   /* pick a default: first tool-capable model */
        cJSON_ArrayForEach(m, list) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools"))) { g_cfg.model = xstrdup(cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring); break; }
        }
        if (!g_cfg.model && cJSON_GetArraySize(list) > 0)
            g_cfg.model = xstrdup(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, 0), "name")->valuestring);
    }
    if (!g_cfg.model) { cJSON_Delete(list); return; }
    cJSON_ArrayForEach(m, list) {
        const char *n = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring;
        char withtag[512]; snprintf(withtag, sizeof withtag, "%s:latest", g_cfg.model);
        if (!strcmp(n, g_cfg.model) || !strcmp(n, withtag)) {
            found = true;
            g_model_tools = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools"));
            break;
        }
    }
    cJSON_Delete(list);
    g_model_max_ctx = ollama_model_context_length(g_cfg.model);
    if (!found) {
        g_model_tools = true;   /* unknown (maybe remote/cloud); try */
        if (!quiet) printf(C_YELLOW "⚠ model '%s' not found locally — ollama may need to pull it (see /models)" C_RESET "\n", g_cfg.model);
    } else if (!g_model_tools && !quiet) {
        printf(C_YELLOW "⚠ model '%s' does not support tool calling; running in chat-only mode (no file/shell tools)" C_RESET "\n", g_cfg.model);
    }
}

/* ---------- context window ---------- */
/* "32768" -> "32k", "131072" -> "128k", "1000000" -> "1M" (static buffer, 4 rotating) */
static const char *fmt_ctx(int n) {
    static char bufs[4][32]; static int bi = 0;
    char *b = bufs[bi++ & 3];
    if (n <= 0) snprintf(b, 32, "default");
    else if (n % 1024 == 0 && n < 1024 * 1024) snprintf(b, 32, "%dk", n / 1024);
    else if (n % (1024 * 1024) == 0) snprintf(b, 32, "%dM", n / (1024 * 1024));
    else if (n >= 1000000 && n % 1000000 == 0) snprintf(b, 32, "%dM", n / 1000000);
    else snprintf(b, 32, "%d", n);
    return b;
}

/* Parses a context size: "65536", "64k", "128K", "1m", "max" (the model's trained
 * length), "default"/"0" (leave to the server). Returns -1 if unusable. */
static int parse_ctx_arg(const char *a) {
    if (!a) return -1;
    while (*a == ' ') a++;
    if (!strcasecmp(a, "default") || !strcmp(a, "0")) return 0;
    if (!strcasecmp(a, "max")) return g_model_max_ctx > 0 ? g_model_max_ctx : -1;
    char *end; double v = strtod(a, &end);
    if (end == a || v <= 0) return -1;
    while (*end == ' ') end++;
    if (*end == 'k' || *end == 'K') { v *= 1024; end++; }
    else if (*end == 'm' || *end == 'M') { v *= 1024 * 1024; end++; }
    if (*end) return -1;
    if (v < 512 || v > 64.0 * 1024 * 1024) return -1;
    return (int)v;
}

static void set_ctx(int n) {
    g_cfg.num_ctx = n; config_save();
    if (n > 0) {
        printf(C_GREEN "✓ context window: %s (num_ctx %d)" C_RESET, fmt_ctx(n), n);
        if (g_model_max_ctx > 0 && n > g_model_max_ctx) printf(C_YELLOW " — larger than the model's trained length (%s); quality may degrade" C_RESET, fmt_ctx(g_model_max_ctx));
        printf("\n" C_DIM "  takes effect on the next request; larger windows need more RAM/VRAM and reload the model" C_RESET "\n");
    } else printf(C_GREEN "✓ context window: server default" C_RESET "\n");
    term_status_refresh();
}

/* /ctx: no argument opens a picker of sizes up to the model's trained maximum;
 * /ctx N|Nk|max|default sets it directly. */
static void cmd_ctx(const char *arg) {
    if (arg) {
        int n = parse_ctx_arg(arg);
        if (n < 0) {
            if (!strcasecmp(arg, "max")) printf(C_YELLOW "the model's maximum context length is unknown; give a size, e.g. /ctx 64k" C_RESET "\n");
            else printf(C_RED "bad size '%s'" C_RESET " — use e.g. /ctx 32768, /ctx 64k, /ctx max, /ctx default\n", arg);
            return;
        }
        set_ctx(n);
        return;
    }
    if (!g_cfg.interactive) {
        printf("num_ctx: %d", g_cfg.num_ctx);
        if (g_model_max_ctx > 0) printf(" (model max %d)", g_model_max_ctx);
        printf("\n");
        return;
    }
    static const int STD[] = { 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576 };
    int sizes[16], n = 0;
    for (size_t i = 0; i < sizeof STD / sizeof *STD; i++)
        if (!g_model_max_ctx || STD[i] <= g_model_max_ctx) sizes[n++] = STD[i];
    if (g_model_max_ctx > 0) { bool has = false; for (int i = 0; i < n; i++) if (sizes[i] == g_model_max_ctx) has = true; if (!has) sizes[n++] = g_model_max_ctx; }
    if (g_cfg.num_ctx > 0) { bool has = false; for (int i = 0; i < n; i++) if (sizes[i] == g_cfg.num_ctx) has = true; if (!has) sizes[n++] = g_cfg.num_ctx; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && sizes[j-1] > sizes[j]; j--) { int t = sizes[j]; sizes[j] = sizes[j-1]; sizes[j-1] = t; }
    sizes[n++] = 0;   /* server default, last */
    const char **items = xmalloc(sizeof(char*) * (size_t)n), **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **own = xmalloc(sizeof(char*) * (size_t)n * 2);
    int current = -1;
    for (int i = 0; i < n; i++) {
        sbuf a; sb_init(&a); sbuf d; sb_init(&d);
        if (sizes[i] == 0) { sb_puts(&a, "server default"); sb_puts(&d, "let ollama decide (usually 4k)"); }
        else {
            sb_printf(&a, "%s", fmt_ctx(sizes[i]));
            sb_printf(&d, "%d tokens", sizes[i]);
            if (sizes[i] == g_model_max_ctx) sb_puts(&d, " · model maximum");
            else if (g_model_max_ctx > 0 && sizes[i] > g_model_max_ctx) sb_puts(&d, " · beyond the model's trained length");
        }
        if (sizes[i] == g_cfg.num_ctx) { sb_puts(&d, " · current"); current = i; }
        own[2*i] = sb_detach(&a); own[2*i+1] = sb_detach(&d);
        items[i] = own[2*i]; descs[i] = own[2*i+1];
    }
    char title[128];
    if (g_model_max_ctx > 0) snprintf(title, sizeof title, "Context window (%s supports up to %s)", g_cfg.model, fmt_ctx(g_model_max_ctx));
    else snprintf(title, sizeof title, "Context window (num_ctx)");
    int r = term_select(title, items, descs, n, current);
    if (r >= 0 && sizes[r] != g_cfg.num_ctx) set_ctx(sizes[r]);
    else printf(C_DIM "context window unchanged: %s" C_RESET "\n", fmt_ctx(g_cfg.num_ctx));
    for (int i = 0; i < 2 * n; i++) free(own[i]);
    free(own); free(items); free(descs);
}

/* ---------- output helpers ---------- */
static void print_result_preview(const char *text, int max_lines) {
    int lines = 0, total = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') total++;
    if (*text && text[strlen(text)-1] != '\n') total++;
    const char *p = text;
    bool first = true;
    while (*p && lines < max_lines) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int w = term_width() - 6; if (w < 20) w = 20;   /* "  ⎿  " prefix + ellipsis */
        size_t cut = n > (size_t)w ? (size_t)w : n;
        while (cut > 0 && cut < n && ((unsigned char)p[cut] & 0xC0) == 0x80) cut--;   /* don't split UTF-8 */
        printf("  %s" C_DIM "%.*s%s" C_RESET "\n", first ? "⎿  " : "   ", (int)cut, p, cut < n ? "…" : "");
        first = false; lines++;
        if (!e) break;
        p = e + 1;
    }
    if (total > lines) printf("     " C_DIM "… +%d lines" C_RESET "\n", total - lines);
    if (!*text) printf("  ⎿  " C_DIM "(empty)" C_RESET "\n");
}

#define AUTO_COMPACT_PCT 95   /* auto-compact once the last request used this much of num_ctx */
static void print_stats(chat_stats *st) {
    if (!st->eval_tokens && !st->prompt_tokens) return;
    double tps = st->eval_seconds > 0 ? st->eval_tokens / st->eval_seconds : 0;
    printf(C_GRAY "  ↳ %d in · %d out · %.1f tok/s · %.1fs", st->prompt_tokens, st->eval_tokens, tps, st->total_seconds);
    if (g_cfg.num_ctx > 0) {
        int pct = (int)(100.0 * st->prompt_tokens / g_cfg.num_ctx);
        printf(" · ctx %d%%", pct);
        if (pct >= 85 && pct < AUTO_COMPACT_PCT) {   /* at AUTO_COMPACT_PCT it compacts by itself */
            if (g_model_max_ctx > g_cfg.num_ctx) printf(C_YELLOW " (near limit — /compact, or /ctx to enlarge: model supports %s)", fmt_ctx(g_model_max_ctx));
            else printf(C_YELLOW " (near limit — consider /compact)");
        }
    }
    printf(C_RESET "\n");
}

/* short one-line summary of a tool call's main argument */
static void tool_arg_summary(const char *name, cJSON *args, char *out, size_t n) {
    const char *keys[] = { !strcmp(name, "grep") ? "pattern" : "command", "path", "pattern", NULL };
    const char *v = NULL;
    for (int i = 0; keys[i] && !v; i++) {
        cJSON *a = cJSON_GetObjectItemCaseSensitive(args, keys[i]);
        if (cJSON_IsString(a)) v = a->valuestring;
    }
    /* fit "● name(summary)" on one terminal line */
    int room = term_width() - (int)strlen(name) - 6; if (room < 20) room = 20;
    if ((size_t)room >= n) room = (int)n - 1;
    if (v) {
        snprintf(out, n, "%.*s", room, v);
        char *nl = strchr(out, '\n'); if (nl) *nl = 0;
        size_t vl = strcspn(v, "\n");
        if (vl > (size_t)room) {   /* truncated: back off to a UTF-8 boundary and add an ellipsis */
            size_t cut = (size_t)room - 1;
            while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
            snprintf(out + cut, n - cut, "…");
        }
    }
    else out[0] = 0;
    if (!strcmp(name, "grep") && v) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
        if (cJSON_IsString(p)) { size_t l = strlen(out); snprintf(out + l, n - l, " in %s", p->valuestring); }
    }
}

/* tools to offer the model in the current mode (plan mode drops the mutating ones) */
static cJSON *tools_for_mode(void) {
    if (!g_model_tools || g_cfg.no_tools) return NULL;
    if (g_cfg.mode != MODE_PLAN) return g_tools;
    cJSON *arr = cJSON_CreateArray(), *t;
    cJSON_ArrayForEach(t, g_tools) {
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
        cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
        const char *name = cJSON_IsString(nm) ? nm->valuestring : "";
        if (!strcmp(name, "write_file") || !strcmp(name, "edit_file")) continue;
        cJSON_AddItemReferenceToArray(arr, t);
    }
    return arr;
}

static char *expand_mentions(const char *input);

/* ---------- queued messages ---------- */
/* Echo a message the user queued while the model was busy, the way it would have
 * looked had they typed it at the prompt. */
static void echo_queued(const char *text) {
    printf(C_BOLD C_ORANGE "› " C_RESET C_DIM "%s" C_RESET "\n", text);
}

/* Deliver plain messages queued while the model was working (Claude Code style):
 * they are appended as user messages so the model sees them on its next call.
 * Slash / ! lines stay queued and are handled by the REPL after the turn.
 * Returns the number of messages injected. */
static int inject_queued(void) {
    int n = 0;
    const char *q;
    while ((q = term_queue_peek()) && *q != '/' && *q != '!') {
        char *text = term_queue_pop();
        echo_queued(text);
        char *msg = expand_mentions(text);
        add_message("user", msg);
        free(msg); free(text); n++;
    }
    if (n) { term_status_refresh(); printf("\n"); }
    return n;
}

/* ---------- the agent loop ---------- */
static bool maybe_auto_compact(void);

/* Returns true if the user interrupted the turn. */
static bool run_turn(void) {
    int iters = 0;
    for (;;) {
        maybe_auto_compact();   /* context nearly full: summarise before the next model call */
        cJSON *msgs = messages_with_system();
        cJSON *tools = tools_for_mode();
        chat_stats st; bool aborted = false;
        cJSON *reply = ollama_chat(msgs, tools, &st, &aborted);
        if (tools && tools != g_tools) cJSON_Delete(tools);
        cJSON_Delete(msgs);
        if (st.prompt_tokens) g_session.last_prompt_tokens = st.prompt_tokens;
        g_session.prompt_tokens += st.prompt_tokens;
        g_session.eval_tokens += st.eval_tokens;
        term_status_refresh();
        if (!reply) {
            /* On failure, keep the conversation as is; the user can retry. */
            return aborted;
        }
        cJSON_AddItemToArray(g_messages, reply);
        cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
        if (aborted || !calls || cJSON_GetArraySize(calls) == 0) {
            print_stats(&st);
            if (!aborted) maybe_auto_compact();
            return aborted;
        }
        if (++iters > g_cfg.max_iters) {
            printf(C_YELLOW "⚠ stopped after %d tool rounds (loop guard). Send a message to continue." C_RESET "\n", g_cfg.max_iters);
            return false;
        }
        cJSON *call;
        cJSON_ArrayForEach(call, calls) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
            const char *name = cJSON_IsString(nm) ? nm->valuestring : "?";
            cJSON *parsed = NULL;
            if (cJSON_IsString(args)) { parsed = cJSON_Parse(args->valuestring); args = parsed; }
            char summ[160]; tool_arg_summary(name, args, summ, sizeof summ);
            printf(C_GREEN "●" C_RESET " " C_BOLD "%s" C_RESET, name);
            if (summ[0]) printf(C_DIM "(%s)" C_RESET, summ);
            printf("\n");
            sbuf out; sb_init(&out);
            tool_status ts = tools_execute(name, args, &out);
            const char *res = out.data ? out.data : "";
            if (ts == TOOL_DENIED) printf("  ⎿  " C_RED "denied" C_RESET "\n");
            else if (ts == TOOL_ERROR) printf("  ⎿  " C_RED "%s" C_RESET "\n", res);
            else print_result_preview(res, !strcmp(name, "read_file") ? 3 : 8);
            cJSON *tm = cJSON_CreateObject();
            cJSON_AddStringToObject(tm, "role", "tool");
            cJSON_AddStringToObject(tm, "tool_name", name);
            cJSON_AddStringToObject(tm, "content", res);
            cJSON_AddItemToArray(g_messages, tm);
            sb_free(&out);
            if (parsed) cJSON_Delete(parsed);
        }
        printf("\n");
        inject_queued();   /* messages the user queued meanwhile go in before the next model call */
    }
}

/* ---------- @file mentions ---------- */
static char *expand_mentions(const char *input) {
    sbuf extra; sb_init(&extra);
    const char *p = input;
    while (*p) {
        if (*p == '@' && (p == input || isspace((unsigned char)p[-1])) && p[1] && !isspace((unsigned char)p[1])) {
            const char *s = p + 1, *e = s;
            while (*e && !isspace((unsigned char)*e)) e++;
            while (e > s && strchr(",.;:)!?", e[-1])) e--;
            char *path = xstrndup(s, (size_t)(e - s));
            char *fp = expand_home(path);
            if (is_file(fp)) {
                size_t n; char *d = read_whole_file(fp, &n, 128 * 1024);
                if (d) {
                    sb_printf(&extra, "\n\n<file path=\"%s\">\n%s%s\n</file>", path, d, n >= 128 * 1024 ? "\n[truncated]" : "");
                    printf(C_DIM "  (attached %s, %zu bytes)" C_RESET "\n", path, n);
                    free(d);
                }
            } else if (is_dir(fp)) {
                cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "path", fp);
                sbuf o; sb_init(&o); tools_execute("list_dir", a, &o);
                sb_printf(&extra, "\n\n<directory path=\"%s\">\n%s</directory>", path, o.data ? o.data : "");
                printf(C_DIM "  (attached listing of %s)" C_RESET "\n", path);
                sb_free(&o); cJSON_Delete(a);
            }
            free(fp); free(path);
            p = e;
            continue;
        }
        p++;
    }
    if (!extra.len) { sb_free(&extra); return xstrdup(input); }
    sbuf r; sb_init(&r); sb_puts(&r, input); sb_append(&r, extra.data, extra.len);
    sb_free(&extra);
    return sb_detach(&r);
}

/* ---------- /init: have the model write CORBIENEST.md ---------- */
static int cmd_init(void) {
    const char *existing = is_file("CORBIENEST.md") ? "CORBIENEST.md" : is_file("CLAUDE.md") ? "CLAUDE.md" : is_file("AGENTS.md") ? "AGENTS.md" : NULL;
    sbuf b; sb_init(&b);
    sb_puts(&b, "Please analyze this codebase and create a CORBIENEST.md file, which will be given to you (and future instances of you) as project instructions at the start of every session in this directory.\n\n"
                "What to add:\n"
                "1. Commands that will be commonly used, such as how to build, lint, and run tests — including how to run a single test.\n"
                "2. High-level code architecture and structure that requires reading multiple files to understand: the main components, how they fit together, where things live. Do not list every file.\n"
                "3. Project conventions worth knowing (style, naming, patterns, things to avoid).\n\n"
                "Usage notes:\n"
                "- Explore first: list the directory, read the README, build files (Makefile, package.json, Cargo.toml, pyproject.toml, go.mod …), CI config and the main entry points before writing anything.\n"
                "- Keep it concise (aim for well under 100 lines); prefer facts that are not obvious from a glance at the tree.\n"
                "- Do not repeat instructions that are already covered by an existing rules file, and do not make things up: only include commands you have verified exist.\n"
                "- Write the file with write_file, then summarise what you put in it in one short paragraph.\n");
    if (existing) sb_printf(&b, "\nNote: a %s already exists in this directory. Read it first and improve it in place (keep what is right, fix what is wrong, fill the gaps) — write CORBIENEST.md only if you would otherwise clobber a hand-written %s.\n", existing, existing);
    printf(C_DIM "  /init: analysing the project and writing CORBIENEST.md…" C_RESET "\n");
    int first = cJSON_GetArraySize(g_messages);
    add_message("user", b.data); sb_free(&b);
    bool aborted = run_turn();
    session_save();
    memory_update(first, aborted);
    load_project_instructions();   /* pick up the new file right away */
    if (!aborted && g_project_instructions) printf(C_GREEN "✓ project instructions loaded" C_RESET "\n");
    return aborted ? -1 : 0;
}

/* ---------- /skills ---------- */
static void cmd_skills(const char *arg) {
    if (arg && !strncmp(arg, "new", 3) && (arg[3] == ' ' || arg[3] == 0)) {
        const char *name = arg + 3; while (*name == ' ') name++;
        if (!*name) { printf("usage: /skills new NAME\n"); return; }
        char path[1200];
        int rc = skill_scaffold(name, path, sizeof path);
        if (rc == 1) printf(C_YELLOW "%s already exists" C_RESET "\n", path);
        else if (rc < 0) printf(C_RED "✗ cannot create skill '%s': %s" C_RESET "\n", name, strerror(errno));
        else printf(C_GREEN "✓ created %s" C_RESET " — edit it, then run it with /%s [args]\n", path, name);
        skills_load(); refresh_slash_completion();
        return;
    }
    if (arg && !strcmp(arg, "reload")) { skills_load(); refresh_slash_completion(); }
    int n = skills_count();
    if (!n) {
        printf(C_DIM "no skills found." C_RESET "\n"
               "  put a SKILL.md in .corbienest/skills/NAME/ (project) or %s/skills/NAME/ (user), or run " C_BOLD "/skills new NAME" C_RESET "\n", config_dir());
        return;
    }
    int w = term_width();
    printf(C_BOLD "%d skill%s" C_RESET " " C_DIM "(run with /NAME [args] · /skills reload · /skills new NAME)" C_RESET "\n", n, n == 1 ? "" : "s");
    int nw = 8; for (int i = 0; i < n; i++) { int l = (int)strlen(skill_get(i)->name) + 1; if (l > nw && l < 32) nw = l; }
    for (int i = 0; i < n; i++) {
        const skill_t *s = skill_get(i);
        int room = w - nw - 14; if (room < 20) room = 20;
        printf("  " C_ORANGE "/%-*s" C_RESET " %.*s%s " C_DIM "[%s]" C_RESET "\n", nw - 1, s->name, room, s->desc, (int)strlen(s->desc) > room ? "…" : "", s->source);
    }
}

/* ---------- slash commands ---------- */
static void cmd_help(void) {
    printf(C_BOLD "corbienest %s" C_RESET " — chat with local Ollama models, with file & shell tools\n\n", CORBIE_VERSION);
    printf(C_BOLD "Commands\n" C_RESET
           "  /help                 this help\n"
           "  /model [name]         pick a model from a menu, or switch directly (saved as default)\n"
           "  /models               list models available in ollama\n"
           "  /clear                start a new conversation\n"
           "  /compact              summarise the conversation to free context\n"
           "  /memory [on|off|clear] show the project memory (" MEMORY_PATH ", curated by the model after each request), toggle or delete it\n"
           "  /status               show model, context usage, settings\n"
           "  /system [text|clear]  show/set extra system instructions\n"
           "  /think on|off|auto    ask the model to think (thinking-capable models)\n"
           "  /think show|hide      show or hide thinking tokens\n"
           "  /skills [reload|new NAME]  list skills (SKILL.md files); run one with /NAME [args]\n"
           "  /init                 have the model explore the project and write a CORBIENEST.md (project instructions)\n"
           "  /mode [name]          permission mode: manual · accept-edits · plan · auto (or press shift+tab to cycle)\n"
           "  /permissions [...]    list the project's saved \"always allow\" rules (.corbienest/permissions); add/remove/clear\n"
           "  /yolo [on|off]        shortcut for /mode auto / /mode manual (dangerous!)\n"
           "  /tools on|off         enable/disable tool calling\n"
           "  /ctx [N|Nk|max]       context window: no argument opens a size picker; N/64k/max/default set it\n"
           "  /temp X               set temperature (-1 = server default)\n"
           "  /host URL             set ollama host (default http://127.0.0.1:11434)\n"
           "  /save [file]          save transcript as markdown\n"
           "  /resume [ID|all]      pick an earlier session to continue (sessions are saved after every request; also --continue / --resume)\n"
           "  /history [N]          show the last N queries (default 20; the latest 100 are kept across sessions)\n"
           "  /cd DIR, /pwd         change / show working directory\n"
           "  /quit, /exit          leave (also Ctrl-D)\n\n"
           C_BOLD "Input\n" C_RESET
           "  !cmd                  run a shell command yourself; output is added to the conversation\n"
           "  @path                 attach a file (or directory listing) to your message\n"
           "  Enter                 send  ·  Alt+Enter / Ctrl+J / trailing \\ : newline\n"
           "  Enter while busy      queue a message for the model (added between tool rounds or after the turn; Ctrl-C hands it back)\n"
           "  Ctrl-C                cancel generation / clear line (twice: quit)  ·  Ctrl-L clear screen\n"
           "  status bar            bottom row shows the permission mode, model, session tokens and context usage\n"
           "  Tab                   complete slash commands  ·  ↑/↓ history\n\n"
           C_BOLD "Tools the model can call\n" C_RESET "  %s\n"
           "  write/edit/bash ask for confirmation: pick with ↑/↓ + enter, or press y (once), a (always this session), p (always in this project), n (deny, with optional reason)\n"
           "  modes: manual asks for everything · accept-edits auto-approves file edits · plan is read-only (model proposes a plan) · auto approves all\n",
           tools_summary_line());
}

static void cmd_models(void) {
    cJSON *list = ollama_list_models();
    if (!list) return;
    cJSON *m;
    printf(C_BOLD "%-52s %-8s %s" C_RESET "\n", "model", "size", "capabilities");
    cJSON_ArrayForEach(m, list) {
        const char *n = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring;
        bool cur = g_cfg.model && (!strcmp(n, g_cfg.model));
        printf("%s%-52s" C_RESET " %-8s %s%s\n", cur ? C_GREEN "▸ " : "  ", n,
               cJSON_GetObjectItemCaseSensitive(m, "size")->valuestring,
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools")) ? C_CYAN "tools " C_RESET : C_DIM "chat-only " C_RESET,
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "thinking")) ? C_MAGENTA "thinking" C_RESET : "");
    }
    printf(C_DIM "switch with /model <name>; models without 'tools' cannot use files/shell" C_RESET "\n");
    cJSON_Delete(list);
}

static void cmd_model_picker(void) {
    cJSON *list = ollama_list_models();
    if (!list) return;
    int n = cJSON_GetArraySize(list);
    if (n == 0) { printf(C_YELLOW "no models installed — run: ollama pull <model>" C_RESET "\n"); cJSON_Delete(list); return; }
    const char **names = xmalloc(sizeof(char*) * (size_t)n);
    const char **descs = xmalloc(sizeof(char*) * (size_t)n);
    char **descbuf = xmalloc(sizeof(char*) * (size_t)n);
    int current = -1;
    for (int i = 0; i < n; i++) {
        cJSON *m = cJSON_GetArrayItem(list, i);
        names[i] = cJSON_GetObjectItemCaseSensitive(m, "name")->valuestring;
        bool tools = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "tools"));
        bool think = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "thinking"));
        const char *size = cJSON_GetObjectItemCaseSensitive(m, "size")->valuestring;
        sbuf d; sb_init(&d);
        sb_printf(&d, "%-7s %s%s", size, tools ? "tools" : "chat-only", think ? " · thinking" : "");
        descbuf[i] = sb_detach(&d); descs[i] = descbuf[i];
        if (g_cfg.model && !strcmp(names[i], g_cfg.model)) current = i;
    }
    int r = term_select("Select model", names, descs, n, current);
    if (r >= 0) {
        free(g_cfg.model); g_cfg.model = xstrdup(names[r]);
        refresh_model_caps(false); config_save();
        printf(C_GREEN "✓ model set to %s" C_RESET "%s\n", g_cfg.model, g_model_tools ? "" : C_DIM " (chat-only: no tool support)" C_RESET);
    } else printf(C_DIM "model unchanged: %s" C_RESET "\n", g_cfg.model);
    for (int i = 0; i < n; i++) free(descbuf[i]);
    free(descbuf); free(names); free(descs); cJSON_Delete(list);
}

static void cmd_status(void) {
    printf(C_BOLD "model      " C_RESET "%s%s\n", g_cfg.model, g_model_tools ? "" : C_DIM " (no tool support)" C_RESET);
    printf(C_BOLD "host       " C_RESET "%s\n", g_cfg.host);
    printf(C_BOLD "cwd        " C_RESET "%s\n", g_cwd);
    printf(C_BOLD "messages   " C_RESET "%d\n", cJSON_GetArraySize(g_messages));
    printf(C_BOLD "context    " C_RESET "last prompt %d tokens", g_session.last_prompt_tokens);
    if (g_cfg.num_ctx > 0) printf(" of %d (%d%%)", g_cfg.num_ctx, (int)(100.0 * g_session.last_prompt_tokens / g_cfg.num_ctx)); else printf(" (num_ctx: server default)");
    if (g_model_max_ctx > 0) printf(" · model max %d%s", g_model_max_ctx, g_model_max_ctx > g_cfg.num_ctx ? " (/ctx to enlarge)" : "");
    printf("\n" C_BOLD "tokens     " C_RESET "%ld this session (%ld in · %ld generated)\n", g_session.prompt_tokens + g_session.eval_tokens, g_session.prompt_tokens, g_session.eval_tokens);
    printf(C_BOLD "tools      " C_RESET "%s\n", (g_cfg.no_tools || !g_model_tools) ? "off" : "on");
    printf(C_BOLD "mode       " C_RESET "%s%s" C_RESET "\n", g_cfg.mode == MODE_AUTO ? C_RED : g_cfg.mode == MODE_PLAN ? C_CYAN : g_cfg.mode == MODE_ACCEPT_EDITS ? C_ORANGE : "", mode_label(g_cfg.mode));
    printf(C_BOLD "think      " C_RESET "%s, %s\n", g_cfg.think < 0 ? "auto" : g_cfg.think ? "on" : "off", g_cfg.show_thinking ? "shown" : "hidden");
    printf(C_BOLD "temp       " C_RESET "%s", g_cfg.temperature < 0 ? "default\n" : ""); if (g_cfg.temperature >= 0) printf("%g\n", g_cfg.temperature);
    if (g_project_instructions) printf(C_BOLD "project    " C_RESET "instructions file loaded\n");
    printf(C_BOLD "memory     " C_RESET "%s%s\n", g_cfg.memory ? "on" : "off", g_memory ? " · " MEMORY_PATH " loaded" : "");
    if (g_session_id[0]) printf(C_BOLD "session    " C_RESET "%s · resume later with: corbienest --resume %s\n", g_session_id, g_session_id);
    else printf(C_BOLD "session    " C_RESET "(nothing saved yet)\n");
}

/* Returns true when the conversation was compacted. */
static bool cmd_compact(void) {
    if (cJSON_GetArraySize(g_messages) == 0) { printf(C_DIM "nothing to compact" C_RESET "\n"); return false; }
    printf(C_DIM "compacting conversation…" C_RESET "\n");
    cJSON *msgs = messages_with_system();
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content",
        "Write a detailed summary of our conversation so far, to be used as context for continuing the work. "
        "Include: the user's goals and requests, what has been done (files read/changed, commands run, results), key findings, "
        "decisions made, and what remains to be done. Be specific about file paths and code details. Output only the summary.");
    cJSON_AddItemToArray(msgs, u);
    chat_stats st; bool aborted;
    cJSON *reply = ollama_chat(msgs, NULL, &st, &aborted);
    cJSON_Delete(msgs);
    g_session.prompt_tokens += st.prompt_tokens; g_session.eval_tokens += st.eval_tokens;
    term_status_refresh();
    if (!reply || aborted) { if (reply) cJSON_Delete(reply); printf(C_YELLOW "compact cancelled" C_RESET "\n"); return false; }
    const char *summary = cJSON_GetObjectItemCaseSensitive(reply, "content")->valuestring;
    cJSON_Delete(g_messages); g_messages = cJSON_CreateArray();
    sbuf b; sb_init(&b);
    sb_printf(&b, "This conversation was compacted. Summary of the previous conversation:\n\n%s\n\nContinue from here.", summary);
    add_message("user", b.data);
    add_message("assistant", "Understood, I have the context from the summary. How should we continue?");
    sb_free(&b); cJSON_Delete(reply);
    g_session.last_prompt_tokens = 0;   /* unknown until the next request; also stops auto-compact re-firing */
    printf(C_GREEN "✓ conversation compacted" C_RESET "\n");
    return true;
}

/* Auto-compact: once the last request used AUTO_COMPACT_PCT% or more of the context
 * window, summarise the conversation before sending anything else. A failed attempt
 * is not retried until the usage figure changes (a new request came through). */
static bool maybe_auto_compact(void) {
    static int tried_at = -1;
    if (g_cfg.num_ctx <= 0 || g_session.last_prompt_tokens <= 0) return false;
    int pct = (int)(100.0 * g_session.last_prompt_tokens / g_cfg.num_ctx);
    if (pct < AUTO_COMPACT_PCT || g_session.last_prompt_tokens == tried_at) return false;
    tried_at = g_session.last_prompt_tokens;
    printf(C_YELLOW "⚠ context %d%% full — auto-compacting" C_RESET "\n", pct);
    return cmd_compact();
}

static void cmd_save(const char *arg) {
    char path[PATH_MAX];
    if (arg && *arg) snprintf(path, sizeof path, "%s", arg);
    else { time_t t = time(NULL); strftime(path, sizeof path, "corbienest-%Y%m%d-%H%M%S.md", localtime(&t)); }
    sbuf b; sb_init(&b);
    sb_printf(&b, "# corbienest transcript (%s)\n\n", g_cfg.model);
    cJSON *m; cJSON_ArrayForEach(m, g_messages) {
        const char *role = cJSON_GetObjectItemCaseSensitive(m, "role")->valuestring;
        cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!strcmp(role, "tool")) { cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tool_name"); sb_printf(&b, "<details><summary>tool result: %s</summary>\n\n```\n%s\n```\n</details>\n\n", cJSON_IsString(tn) ? tn->valuestring : "?", c && cJSON_IsString(c) ? c->valuestring : ""); continue; }
        sb_printf(&b, "## %s\n\n%s\n\n", role, c && cJSON_IsString(c) ? c->valuestring : "");
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
        if (tc) { char *s = cJSON_PrintUnformatted(tc); sb_printf(&b, "```json\n%s\n```\n\n", s); free(s); }
    }
    if (write_whole_file(path, b.data, b.len) == 0) printf(C_GREEN "✓ saved %s" C_RESET "\n", path);
    else printf(C_RED "✗ cannot write %s: %s" C_RESET "\n", path, strerror(errno));
    sb_free(&b);
}

/* /history [N]: the most recent queries, oldest first (↑/↓ at the prompt recalls them) */
static void cmd_history(const char *arg) {
    int total = hist_count();
    if (!total) { printf(C_DIM "no history yet" C_RESET "\n"); return; }
    int want = arg ? atoi(arg) : 20;
    if (want <= 0) want = 20;
    if (want > total) want = total;
    printf(C_BOLD "last %d of %d quer%s" C_RESET " " C_DIM "(↑/↓ at the prompt recalls them · the latest 100 are kept)" C_RESET "\n", want, total, total == 1 ? "y" : "ies");
    int w = term_width() - 8; if (w < 20) w = 20;
    for (int i = total - want; i < total; i++) {
        const char *q = hist_get(i);
        size_t n = strcspn(q, "\n"), cut = n > (size_t)w ? (size_t)w : n;
        while (cut > 0 && cut < n && ((unsigned char)q[cut] & 0xC0) == 0x80) cut--;
        printf(C_DIM "%4d" C_RESET "  %.*s%s\n", i + 1, (int)cut, q, cut < n || q[n] ? "…" : "");
    }
}

/* returns 1 to quit, -1 if a skill's model turn was interrupted */
static int handle_slash(char *line) {
    char *cmd = line, *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; if (!*arg) arg = NULL; }
    if (!strcmp(cmd, "/help") || !strcmp(cmd, "/?")) cmd_help();
    else if (!strcmp(cmd, "/quit") || !strcmp(cmd, "/exit") || !strcmp(cmd, "/q")) return 1;
    else if (!strcmp(cmd, "/models")) cmd_models();
    else if (!strcmp(cmd, "/model")) {
        if (!arg) cmd_model_picker();
        else { free(g_cfg.model); g_cfg.model = xstrdup(arg); refresh_model_caps(false); config_save(); printf(C_GREEN "✓ model set to %s" C_RESET "\n", g_cfg.model); }
    }
    else if (!strcmp(cmd, "/clear") || !strcmp(cmd, "/new")) { cJSON_Delete(g_messages); g_messages = cJSON_CreateArray(); tools_reset_permissions(); g_session.last_prompt_tokens = 0; g_session_id[0] = 0; term_clear_screen(); printf(C_GREEN "✓ new conversation" C_RESET "\n"); }
    else if (!strcmp(cmd, "/compact")) { if (cmd_compact()) session_save(); }
    else if (!strcmp(cmd, "/resume")) cmd_resume(arg);
    else if (!strcmp(cmd, "/permissions")) cmd_permissions(arg);
    else if (!strcmp(cmd, "/init")) return cmd_init();
    else if (!strcmp(cmd, "/status") || !strcmp(cmd, "/cost")) cmd_status();
    else if (!strcmp(cmd, "/system")) {
        if (!arg) printf("extra system prompt: %s\n", g_cfg.system_prompt ? g_cfg.system_prompt : C_DIM "(none)" C_RESET);
        else if (!strcmp(arg, "clear")) { free(g_cfg.system_prompt); g_cfg.system_prompt = NULL; printf(C_GREEN "✓ cleared" C_RESET "\n"); }
        else { free(g_cfg.system_prompt); g_cfg.system_prompt = xstrdup(arg); printf(C_GREEN "✓ set" C_RESET "\n"); }
    }
    else if (!strcmp(cmd, "/think")) {
        if (!arg) printf("think: %s, %s\n", g_cfg.think < 0 ? "auto" : g_cfg.think ? "on" : "off", g_cfg.show_thinking ? "shown" : "hidden");
        else if (!strcmp(arg, "on")) g_cfg.think = 1;
        else if (!strcmp(arg, "off")) g_cfg.think = 0;
        else if (!strcmp(arg, "auto")) g_cfg.think = -1;
        else if (!strcmp(arg, "show")) g_cfg.show_thinking = true;
        else if (!strcmp(arg, "hide")) g_cfg.show_thinking = false;
        else printf("usage: /think on|off|auto|show|hide\n");
        if (arg) { config_save(); printf(C_GREEN "✓ think: %s, %s" C_RESET "\n", g_cfg.think < 0 ? "auto" : g_cfg.think ? "on" : "off", g_cfg.show_thinking ? "shown" : "hidden"); }
    }
    else if (!strcmp(cmd, "/yolo")) {
        bool on = !arg ? !YOLO() : !strcmp(arg, "on");
        g_cfg.mode = on ? MODE_AUTO : MODE_MANUAL;
        config_save();
        printf(on ? C_RED "⚠ auto (yolo) mode ON — all tool calls auto-approved" C_RESET "\n" : C_GREEN "✓ auto mode off — back to manual confirmations" C_RESET "\n");
    }
    else if (!strcmp(cmd, "/mode")) {
        if (!arg) {
            printf("mode: " C_BOLD "%s" C_RESET "\n" C_DIM "available: manual, accept-edits, plan, auto (shift+tab cycles)" C_RESET "\n", mode_label(g_cfg.mode));
        } else {
            int m = mode_parse(arg);
            if (m < 0) printf(C_RED "unknown mode '%s'" C_RESET " — use manual, accept-edits, plan or auto\n", arg);
            else { g_cfg.mode = m; config_save(); printf("%s✓ mode: %s" C_RESET "\n", m == MODE_AUTO ? C_RED : C_GREEN, mode_label(m)); }
        }
    }
    else if (!strcmp(cmd, "/tools")) {
        if (!arg) printf("tools: %s\n", g_cfg.no_tools ? "off" : "on"); else { g_cfg.no_tools = !strcmp(arg, "off"); printf(C_GREEN "✓ tools %s" C_RESET "\n", g_cfg.no_tools ? "off" : "on"); }
    }
    else if (!strcmp(cmd, "/ctx") || !strcmp(cmd, "/context")) cmd_ctx(arg);
    else if (!strcmp(cmd, "/temp")) {
        if (!arg) printf("temperature: %g\n", g_cfg.temperature); else { g_cfg.temperature = atof(arg); config_save(); printf(C_GREEN "✓ temperature = %g" C_RESET "\n", g_cfg.temperature); }
    }
    else if (!strcmp(cmd, "/host")) {
        if (!arg) printf("host: %s\n", g_cfg.host); else { free(g_cfg.host); g_cfg.host = xstrdup(arg); config_save(); refresh_model_caps(false); printf(C_GREEN "✓ host = %s" C_RESET "\n", g_cfg.host); }
    }
    else if (!strcmp(cmd, "/save")) cmd_save(arg);
    else if (!strcmp(cmd, "/history")) cmd_history(arg);
    else if (!strcmp(cmd, "/pwd")) printf("%s\n", g_cwd);
    else if (!strcmp(cmd, "/cd")) {
        char *d = expand_home(arg ? arg : "~");
        if (chdir(d) == 0) { if (getcwd(g_cwd, sizeof g_cwd)) {} load_project_instructions(); load_memory(); tools_permissions_load(); skills_load(); refresh_slash_completion(); printf(C_GREEN "✓ %s" C_RESET "\n", g_cwd); }
        else printf(C_RED "✗ cd %s: %s" C_RESET "\n", d, strerror(errno));
        free(d);
    }
    else if (!strcmp(cmd, "/skills")) cmd_skills(arg);
    else if (!strcmp(cmd, "/memory")) cmd_memory(arg);
    else {
        const skill_t *sk = skill_find(cmd);
        if (!sk) { printf(C_RED "unknown command %s" C_RESET " — try /help or /skills\n", cmd); return 0; }
        printf(C_DIM "  skill %s: %s" C_RESET "\n", sk->name, *sk->desc ? sk->desc : sk->path);
        char *prompt = skill_expand(sk, arg);
        char *msg = expand_mentions(prompt);
        int first = cJSON_GetArraySize(g_messages);
        add_message("user", msg);
        free(msg); free(prompt);
        bool aborted = run_turn();
        session_save();
        memory_update(first, aborted);
        if (aborted) return -1;
    }
    return 0;
}

/* !cmd : run shell command directly, add to context */
static void handle_bang(const char *cmd) {
    cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "command", cmd);
    tools_no_confirm = true;   /* user typed it: no confirmation */
    sbuf out; sb_init(&out);
    printf(C_GREEN "●" C_RESET " " C_BOLD "bash" C_RESET "\n");
    tools_execute("bash", a, &out);
    tools_no_confirm = false;
    print_result_preview(out.data ? out.data : "", 40);
    sbuf m; sb_init(&m);
    sb_printf(&m, "I ran this shell command myself:\n$ %s\n\nOutput:\n%s", cmd, out.data ? out.data : "");
    add_message("user", m.data);
    add_message("assistant", "Noted the command output.");
    sb_free(&m); sb_free(&out); cJSON_Delete(a);
}

static void banner(void) {
    char ver[128];
    int ok = ollama_ping(ver, sizeof ver);
    int w = term_width(); if (w < 40) w = 40;
    printf(C_ORANGE "╭"); for (int i = 0; i < w - 2; i++) printf("─"); printf("╮" C_RESET "\n");
    printf(C_ORANGE "│" C_RESET " " C_BOLD "🐦‍⬛ Corbie Nest" C_RESET " v%s — local coding agent for Ollama\n", CORBIE_VERSION);
    printf(C_ORANGE "│" C_RESET " " C_DIM "model:" C_RESET " %s%s" C_DIM " · ctx %s%s%s" C_RESET "\n", g_cfg.model ? g_cfg.model : "(none)", g_model_tools ? "" : C_DIM " (chat-only)" C_RESET,
           fmt_ctx(g_cfg.num_ctx), g_model_max_ctx > 0 ? " of " : "", g_model_max_ctx > 0 ? fmt_ctx(g_model_max_ctx) : "");
    printf(C_ORANGE "│" C_RESET " " C_DIM "host: " C_RESET " %s %s\n", g_cfg.host, ok == 0 ? C_GREEN "● connected" C_RESET : C_RED "● unreachable" C_RESET);
    printf(C_ORANGE "│" C_RESET " " C_DIM "cwd:  " C_RESET " %s%s\n", g_cwd, g_project_instructions ? C_DIM " (project instructions loaded)" C_RESET : "");
    if (g_cfg.mode != MODE_MANUAL) printf(C_ORANGE "│" C_RESET " " C_DIM "mode:  " C_RESET " %s%s" C_RESET "\n", g_cfg.mode == MODE_AUTO ? C_RED : g_cfg.mode == MODE_PLAN ? C_CYAN : C_ORANGE, mode_label(g_cfg.mode));
    printf(C_ORANGE "╰"); for (int i = 0; i < w - 2; i++) printf("─"); printf("╯" C_RESET "\n");
    if (ok != 0) printf(C_RED "cannot reach ollama at %s: %s" C_RESET "\n" C_DIM "start it with `ollama serve`, or set the host with /host or OLLAMA_HOST" C_RESET "\n", g_cfg.host, ver);
    printf(C_DIM "/help for commands · @file to attach · !cmd for shell · shift+tab to switch mode · Ctrl-D to quit" C_RESET "\n\n");
}

/* Handle one line of user input (from the editor or the message queue).
 * Returns 1 to quit, -1 if the model turn was interrupted, 0 otherwise. */
static int process_input(char *line) {
    char *s = line; while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    size_t n = strlen(s); while (n && (s[n-1] == ' ' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
    if (!*s) return 0;
    hist_add(s); hist_save();   /* persist as we go, so a crash or kill loses nothing */
    if (*s == '/') { int q = handle_slash(s); if (q == 1) return 1; printf("\n"); term_status_refresh(); return q; }
    if (*s == '!') { handle_bang(s + 1); printf("\n"); return 0; }
    char *msg = expand_mentions(s);
    int first = cJSON_GetArraySize(g_messages);
    add_message("user", msg);
    free(msg);
    bool aborted = run_turn();
    session_save();
    memory_update(first, aborted);
    printf("\n");
    fflush(stdout);
    return aborted ? -1 : 0;
}

static void usage(void) {
    printf("usage: corbienest [options] [-p PROMPT]\n"
           "  -m, --model NAME     model to use (default: saved / first tool-capable model)\n"
           "  -H, --host URL       ollama host (default $OLLAMA_HOST or http://127.0.0.1:11434)\n"
           "  -c, --ctx N          context window (num_ctx), e.g. 32768, 64k, 128k\n"
           "  -s, --system TEXT    extra system instructions\n"
           "  -p, --prompt TEXT    non-interactive: run one prompt and exit (implies tools need --yolo)\n"
           "  -y, --yolo           auto-approve tool calls (same as --mode auto)\n"
           "      --mode NAME      permission mode: manual, accept-edits, plan, auto\n"
           "  -T, --no-tools       disable tool calling\n"
           "      --continue       resume the latest session started in this directory\n"
           "  -r, --resume [ID]    resume a session: by ID, or pick one from a menu\n"
           "      --no-memory      don't update " MEMORY_PATH " after requests\n"
           "      --think          ask model to think; --no-think to disable; --show-thinking to display\n"
           "  -h, --help           this help\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    signal(SIGPIPE, SIG_IGN);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.temperature = -1; g_cfg.think = -1; g_cfg.max_iters = 60; g_cfg.num_ctx = 32768; g_cfg.color = true; g_cfg.memory = true;
    g_cfg.interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    config_load();
    const char *env_host = getenv("OLLAMA_HOST");
    if (env_host && *env_host && !g_cfg.host) g_cfg.host = xstrdup(env_host);
    if (!g_cfg.host) g_cfg.host = xstrdup("http://127.0.0.1:11434");
    if (strncmp(g_cfg.host, "http", 4) != 0) { sbuf b; sb_init(&b); sb_printf(&b, "http://%s", g_cfg.host); free(g_cfg.host); g_cfg.host = sb_detach(&b); }
    const char *env_model = getenv("CORBIENEST_MODEL");
    if (env_model && *env_model) { free(g_cfg.model); g_cfg.model = xstrdup(env_model); }

    const char *oneshot = NULL, *resume_id = NULL; bool resume_latest = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEEDARG() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (char*)NULL))
        if (!strcmp(a, "-m") || !strcmp(a, "--model")) { free(g_cfg.model); g_cfg.model = xstrdup(NEEDARG()); }
        else if (!strcmp(a, "-H") || !strcmp(a, "--host")) { free(g_cfg.host); g_cfg.host = xstrdup(NEEDARG()); }
        else if (!strcmp(a, "-c") || !strcmp(a, "--ctx")) { const char *cv = NEEDARG(); int n = parse_ctx_arg(cv); if (n < 0) { fprintf(stderr, "bad context size %s (use e.g. 32768, 64k, default)\n", cv); return 2; } g_cfg.num_ctx = n; }
        else if (!strcmp(a, "-s") || !strcmp(a, "--system")) { free(g_cfg.system_prompt); g_cfg.system_prompt = xstrdup(NEEDARG()); }
        else if (!strcmp(a, "-p") || !strcmp(a, "--prompt")) oneshot = NEEDARG();
        else if (!strcmp(a, "-y") || !strcmp(a, "--yolo")) g_cfg.mode = MODE_AUTO;
        else if (!strcmp(a, "--mode")) { const char *mn = NEEDARG(); int m = mode_parse(mn); if (m < 0) { fprintf(stderr, "unknown mode %s\n", mn); return 2; } g_cfg.mode = m; }
        else if (!strcmp(a, "-T") || !strcmp(a, "--no-tools")) g_cfg.no_tools = true;
        else if (!strcmp(a, "--no-memory")) g_cfg.memory = false;
        else if (!strcmp(a, "--continue")) resume_latest = true;
        else if (!strcmp(a, "-r") || !strcmp(a, "--resume")) { resume_id = (i + 1 < argc && argv[i+1][0] != '-') ? argv[++i] : ""; }
        else if (!strcmp(a, "--think")) g_cfg.think = 1;
        else if (!strcmp(a, "--no-think")) g_cfg.think = 0;
        else if (!strcmp(a, "--show-thinking")) g_cfg.show_thinking = true;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-v") || !strcmp(a, "--version")) { printf("corbienest %s\n", CORBIE_VERSION); return 0; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    if (!getcwd(g_cwd, sizeof g_cwd)) snprintf(g_cwd, sizeof g_cwd, ".");
    term_init();
    g_messages = cJSON_CreateArray();
    g_tools = tools_definitions();
    load_project_instructions();
    load_memory();
    tools_permissions_load();
    skills_load();
    refresh_model_caps(oneshot != NULL);
    if (!g_cfg.model) { fprintf(stderr, "corbienest: no models found on %s (run `ollama pull <model>`)\n", g_cfg.host); return 1; }
    if (g_cfg.interactive && !oneshot) term_fullscreen(true);   /* alternate screen + bottom status bar */
    if (resume_latest) {
        int n; session_info *v = sessions_list(true, &n);
        if (n) session_load(v[0].id); else printf(C_DIM "no earlier session in this directory — starting fresh" C_RESET "\n");
        sessions_free(v, n);
    } else if (resume_id && *resume_id) {
        if (!session_load(resume_id)) { fprintf(stderr, "corbienest: no session %s\n", resume_id); term_restore(); return 1; }
    } else if (resume_id) {
        if (oneshot) { fprintf(stderr, "corbienest: --resume needs a session ID with -p\n"); return 2; }
        cmd_resume(NULL);
    }

    if (oneshot) {
        char *msg;
        const skill_t *sk = NULL;
        if (oneshot[0] == '/') { char *nm = xstrndup(oneshot, strcspn(oneshot, " ")); sk = skill_find(nm); free(nm); }
        if (sk) { const char *a = strchr(oneshot, ' '); char *pr = skill_expand(sk, a ? a + 1 : ""); msg = expand_mentions(pr); free(pr); }
        else msg = expand_mentions(oneshot);
        int first = cJSON_GetArraySize(g_messages);
        add_message("user", msg); free(msg);
        bool aborted = run_turn();
        session_save();
        memory_update(first, aborted);
        term_restore();
        return 0;
    }

    banner();
    hist_load();
    refresh_slash_completion();
    for (;;) {
        char *line = term_readline(C_BOLD C_ORANGE "› " C_RESET);
        if (!line) break;
        int rc = process_input(line);
        free(line);
        if (rc == 1) break;
        /* Messages queued while the model was busy: after an interrupt they go back to the
         * editor for the user to review; otherwise they are sent one after another. */
        while (rc != -1 && term_queue_count()) {
            char *q = term_queue_pop();
            echo_queued(q);
            rc = process_input(q);
            free(q);
        }
        if (rc == 1) break;
        if (rc == -1) term_queue_to_editor();
    }
    hist_save();
    session_save();
    term_restore();   /* leaves the alternate screen: say goodbye on the normal one */
    printf(C_DIM "bye" C_RESET " · corbienest used %ld tokens this session (%ld in · %ld out) with %s\n",
           g_session.prompt_tokens + g_session.eval_tokens, g_session.prompt_tokens, g_session.eval_tokens, g_cfg.model);
    if (g_session_id[0]) printf(C_DIM "  continue it later with: corbienest --continue   (or --resume %s)" C_RESET "\n", g_session_id);
    return 0;
}
