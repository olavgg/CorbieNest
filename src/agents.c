/* Sub-agent files: a role the main agent can start by name with task(agent=NAME).
 *
 * An agent is a Markdown file, the frontmatter for its settings and the body for its role (added to the sub-agent's
 * prompt). Where they are read, in this order (the first file of a name is the one used):
 *   .corbienest/agents/NAME.md     this project
 *   .claude/agents/NAME.md         this project (Claude Code's folder)
 *   <config>/agents/NAME.md        yours, for every project
 * The frontmatter keys: name (the file name by default), description, tools (a comma list of sub-agent tools; the
 * default is all of them), model (a model for this agent; the default is the sub-agents' model). An agent can use only
 * the tools a sub-agent has (read_file, list_dir, grep, bash, web_fetch, web_search): it cannot be given more, and it
 * cannot start a sub-agent or ask the advisor. Its shell commands and fetches are confirmed like anyone's. */
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGENT_MAX 32
#define AGENT_BODY_MAX (16 * 1024)

static agent_t g_agent[AGENT_MAX];
static int g_nagent = 0;

static char *trim_dup_a(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) n--;
    char *d = xmalloc(n + 1); memcpy(d, s, n); d[n] = 0;
    return d;
}
/* "---\nkey: value\n---\nbody"; a file without frontmatter is all body. */
static void parse_agent(const char *text, agent_t *a) {
    const char *p = text;
    if (strncmp(p, "---", 3) != 0 || (p[3] != '\n' && p[3] != '\r')) { a->body = xstrdup(text); return; }
    p += 3; while (*p == '\r') p++;
    p++;   /* the newline */
    for (;;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= 3 && !strncmp(p, "---", 3)) { p = e ? e + 1 : p + n; break; }
        const char *colon = memchr(p, ':', n);
        if (colon) {
            char *k = trim_dup_a(p, (size_t)(colon - p));
            char *v = trim_dup_a(colon + 1, n - (size_t)(colon - p) - 1);
            if (!strcmp(k, "name")) { free(a->name); a->name = v; v = NULL; }
            else if (!strcmp(k, "description")) { free(a->description); a->description = v; v = NULL; }
            else if (!strcmp(k, "tools")) { free(a->tools); a->tools = v; v = NULL; }
            else if (!strcmp(k, "model")) { free(a->model); a->model = v; v = NULL; }
            free(k); free(v);
        }
        if (!e) { p += n; break; }
        p = e + 1;
    }
    while (*p == '\n' || *p == '\r') p++;
    a->body = xstrdup(p);
}
static bool valid_name(const char *s) {
    if (!*s || strlen(s) > 40) return false;
    for (const char *p = s; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    return true;
}
static bool have(const char *name) { for (int i = 0; i < g_nagent; i++) if (!strcmp(g_agent[i].name, name)) return true; return false; }
static void read_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nagent < AGENT_MAX) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 3, ".md")) continue;
        char path[1400]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len; char *text = read_whole_file(path, &len, 64 * 1024);
        if (!text) continue;
        agent_t a; memset(&a, 0, sizeof a);
        parse_agent(text, &a);
        free(text);
        if (!a.name) a.name = xstrndup(e->d_name, n - 3);
        if (!valid_name(a.name) || have(a.name) || !a.description || !*a.description) { free(a.name); free(a.description); free(a.tools); free(a.model); free(a.body); continue; }
        if (strlen(a.body) > AGENT_BODY_MAX) a.body[AGENT_BODY_MAX] = 0;
        a.path = xstrdup(path);
        g_agent[g_nagent++] = a;
    }
    closedir(d);
}

/* (Re)reads the agent files of this project and of the user. Returns how many there are. */
int agents_load(void) {
    for (int i = 0; i < g_nagent; i++) { free(g_agent[i].name); free(g_agent[i].description); free(g_agent[i].tools); free(g_agent[i].model); free(g_agent[i].body); free(g_agent[i].path); }
    g_nagent = 0;
    char cwd[1024], p[1200];
    if (getcwd(cwd, sizeof cwd)) {
        snprintf(p, sizeof p, "%s/.corbienest/agents", cwd); read_dir(p);
        snprintf(p, sizeof p, "%s/.claude/agents", cwd); read_dir(p);
    }
    snprintf(p, sizeof p, "%s/agents", config_dir()); read_dir(p);
    return g_nagent;
}
int agents_count(void) { return g_nagent; }
const agent_t *agent_get(int i) { return i >= 0 && i < g_nagent ? &g_agent[i] : NULL; }
const agent_t *agent_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_nagent; i++) if (!strcmp(g_agent[i].name, name)) return &g_agent[i];
    return NULL;
}
/* Claude Code's names for the same tools (its agent files say "tools: Read, Grep, Bash"): the name corbienest gives it, or NULL. */
static const char *tool_alias(const char *s, size_t n) {
    static const char *map[][2] = { {"Read", "read_file"}, {"Grep", "grep"}, {"Glob", "list_dir"}, {"LS", "list_dir"},
        {"Bash", "bash"}, {"WebFetch", "web_fetch"}, {"WebSearch", "web_search"} };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (strlen(map[i][0]) == n && !strncmp(s, map[i][0], n)) return map[i][1];
    return NULL;
}
/* Each entry of the agent's list, as the tool it names (its own name, or Claude Code's): fn is called with each.
 * Entries that are not sub-agent tools are passed on as they are; the caller decides. */
static bool entry_is(const char *p, size_t n, const char *tool) {
    const char *alias = tool_alias(p, n);
    if (alias) return !strcmp(alias, tool);
    return n == strlen(tool) && !strncmp(p, tool, n);
}
/* Is `tool` in the agent's list? No list means all of them. */
bool agent_allows(const agent_t *a, const char *tool) {
    if (!a->tools || !*a->tools) return true;
    for (const char *p = a->tools; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p; while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p); while (n && p[n-1] == ' ') n--;
        if (n && entry_is(p, n, tool)) return true;
        p = e;
    }
    return false;
}
/* Does the agent's list name at least one tool a sub-agent has? (An empty or missing list means all of them.) */
bool agent_has_usable_tools(const agent_t *a) {
    static const char *base[] = { "read_file", "list_dir", "grep", "bash", "web_fetch", "web_search" };
    if (!a->tools || !*a->tools) return true;
    for (size_t k = 0; k < sizeof base / sizeof *base; k++) if (agent_allows(a, base[k])) return true;
    return false;
}
/* The model a file names for its sub-agent runs, or NULL for the sub-agents' own model: a file without one, "inherit",
 * and Claude Code's aliases (sonnet, opus, haiku) — those are not models of this server, so they mean the same. */
const char *agent_model_override(const agent_t *a) {
    if (!a || !a->model || !*a->model) return NULL;
    static const char *none[] = { "inherit", "sonnet", "opus", "haiku" };
    for (size_t k = 0; k < sizeof none / sizeof *none; k++) if (!strcmp(a->model, none[k])) return NULL;
    return a->model;
}
/* The part of the main agent's system prompt that names the agents: NULL when there are none. */
char *agents_prompt_section(void) {
    if (!g_nagent) return NULL;
    sbuf b; sb_init(&b);
    sb_puts(&b, "\nSub-agents defined for this project. Start one with task and agent=NAME (its prompt is the task you give; its role and tools are its own):\n");
    for (int i = 0; i < g_nagent; i++) sb_printf(&b, "- %s: %s\n", g_agent[i].name, g_agent[i].description);
    return sb_detach(&b);
}
