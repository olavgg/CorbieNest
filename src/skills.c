/* Skills: reusable instruction files (SKILL.md with a small YAML-ish
 * frontmatter), invoked as /name [args] from the prompt. Looked up in
 *   ./.corbienest/skills/<name>/SKILL.md   ./.corbienest/skills/<name>.md
 *   ./.claude/skills/<name>/SKILL.md     (Claude Code layout, for sharing)
 *   $XDG_CONFIG_HOME/corbienest/skills/<name>/SKILL.md   (and <name>.md)
 * Project skills shadow user skills of the same name. */
#define _GNU_SOURCE
#include "common.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static skill_t *g_skills = NULL;
static int g_nskills = 0, g_cap = 0;

static void skill_free(skill_t *s) { free(s->name); free(s->desc); free(s->path); free(s->dir); free(s->body); }

static void skills_clear(void) {
    for (int i = 0; i < g_nskills; i++) skill_free(&g_skills[i]);
    g_nskills = 0;
}

static char *trim_dup(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t' || *s == '\r')) { s++; n--; }
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) n--;
    /* strip matching quotes */
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') || (s[0] == '\'' && s[n-1] == '\''))) { s++; n -= 2; }
    return xstrndup(s, n);
}

/* Parse "---\nkey: value\n---\nbody". Missing frontmatter => whole file is body. */
static void parse_skill(const char *text, char **name, char **desc, char **body) {
    *name = *desc = NULL;
    const char *p = text;
    if (strncmp(p, "---", 3) != 0) { *body = xstrdup(text); return; }
    p += 3; while (*p == ' ' || *p == '\r') p++;
    if (*p != '\n') { *body = xstrdup(text); return; }
    p++;
    for (;;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= 3 && !strncmp(p, "---", 3)) { p = e ? e + 1 : p + n; break; }
        const char *colon = memchr(p, ':', n);
        if (colon) {
            size_t kl = (size_t)(colon - p);
            char *k = trim_dup(p, kl);
            char *v = trim_dup(colon + 1, n - kl - 1);
            if (!strcmp(k, "name")) { free(*name); *name = v; v = NULL; }
            else if (!strcmp(k, "description")) { free(*desc); *desc = v; v = NULL; }
            free(k); free(v);
        }
        if (!e) { p += n; break; }
        p = e + 1;
    }
    while (*p == '\n') p++;
    *body = xstrdup(p);
}

static bool valid_name(const char *n) {
    if (!*n || strlen(n) > 64) return false;
    for (const char *c = n; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return false;
    return true;
}

static void add_skill(const char *path, const char *dir, const char *fallback_name, const char *source) {
    size_t n; char *d = read_whole_file(path, &n, 256 * 1024);
    if (!d) return;
    char *name, *desc, *body;
    parse_skill(d, &name, &desc, &body);
    free(d);
    if (!name || !*name) { free(name); name = xstrdup(fallback_name); }
    if (!valid_name(name) || name[0] == '/') { free(name); free(desc); free(body); return; }
    for (int i = 0; i < g_nskills; i++)   /* first hit wins (project before user) */
        if (!strcmp(g_skills[i].name, name)) { free(name); free(desc); free(body); return; }
    if (g_nskills == g_cap) { g_cap = g_cap ? g_cap * 2 : 16; g_skills = xrealloc(g_skills, sizeof(skill_t) * (size_t)g_cap); }
    skill_t *s = &g_skills[g_nskills++];
    s->name = name; s->desc = desc ? desc : xstrdup(""); s->path = xstrdup(path); s->dir = xstrdup(dir); s->body = body; s->source = source;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char* const*)a, *(char* const*)b); }

static void scan_dir(const char *root, const char *source) {
    DIR *dp = opendir(root);
    if (!dp) return;
    char **names = NULL; int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; names = xrealloc(names, sizeof(char*) * (size_t)cap); }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof(char*), cmp_name);
    for (int i = 0; i < n; i++) {
        char sub[2048], file[2100];
        snprintf(sub, sizeof sub, "%s/%s", root, names[i]);
        if (is_dir(sub)) {
            snprintf(file, sizeof file, "%s/SKILL.md", sub);
            if (is_file(file)) add_skill(file, sub, names[i], source);
        } else {
            size_t l = strlen(names[i]);
            if (l > 3 && !strcmp(names[i] + l - 3, ".md")) {
                char *base = xstrndup(names[i], l - 3);
                add_skill(sub, root, base, source);
                free(base);
            }
        }
        free(names[i]);
    }
    free(names);
}

int skills_load(void) {
    skills_clear();
    scan_dir(".corbienest/skills", "project");
    scan_dir(".claude/skills", "project");
    char user[1200]; snprintf(user, sizeof user, "%s/skills", config_dir());
    scan_dir(user, "user");
    return g_nskills;
}

int skills_count(void) { return g_nskills; }
const skill_t *skill_get(int i) { return i >= 0 && i < g_nskills ? &g_skills[i] : NULL; }
const skill_t *skill_find(const char *name) {
    if (!name) return NULL;
    if (*name == '/') name++;
    for (int i = 0; i < g_nskills; i++) if (!strcmp(g_skills[i].name, name)) return &g_skills[i];
    return NULL;
}

/* Build the message sent to the model when the user runs /name args. */
char *skill_expand(const skill_t *s, const char *args) {
    sbuf b; sb_init(&b);
    sb_printf(&b, "<skill name=\"%s\" path=\"%s\">\n", s->name, s->path);
    const char *body = s->body, *m;
    bool subst = false;
    while ((m = strstr(body, "$ARGUMENTS"))) {
        sb_append(&b, body, (size_t)(m - body));
        sb_puts(&b, args && *args ? args : "");
        body = m + strlen("$ARGUMENTS");
        subst = true;
    }
    sb_puts(&b, body);
    if (b.len && b.data[b.len-1] != '\n') sb_putc(&b, '\n');
    if (!subst && args && *args) sb_printf(&b, "\nArguments: %s\n", args);
    sb_printf(&b, "</skill>\n(Supporting files for this skill, if any, are in %s — read them with read_file or run them with bash as the skill instructs.)", s->dir);
    return sb_detach(&b);
}

/* Short listing for the system prompt, or NULL if there are no skills. */
char *skills_prompt_section(void) {
    if (!g_nskills) return NULL;
    sbuf b; sb_init(&b);
    sb_puts(&b, "\n# Skills\nSkills are reusable instruction files (SKILL.md). The user runs one with /name; "
                "if a task clearly matches a skill's description you may also read its SKILL.md with read_file and follow it. Available:\n");
    for (int i = 0; i < g_nskills; i++)
        sb_printf(&b, "- /%s — %s (%s)\n", g_skills[i].name, *g_skills[i].desc ? g_skills[i].desc : "no description", g_skills[i].path);
    sb_puts(&b, "When asked to create or extend a skill: put it in .corbienest/skills/<name>/SKILL.md (frontmatter with name + description, then the instructions; "
                "$ARGUMENTS is replaced by the user's arguments). Helper programs that a skill needs must be written in C (compiled with cc/make); "
                "use Rust only when third-party libraries are genuinely required; Python is a last resort only.\n");
    return sb_detach(&b);
}

/* Scaffold .corbienest/skills/<name>/SKILL.md; returns 0 ok, -1 error (errno) , 1 exists */
int skill_scaffold(const char *name, char *path_out, size_t n) {
    if (!valid_name(name)) { errno = EINVAL; return -1; }
    char dir[1024]; snprintf(dir, sizeof dir, ".corbienest/skills/%s", name);
    snprintf(path_out, n, "%s/SKILL.md", dir);
    if (is_file(path_out)) return 1;
    if (mkdir_p(dir) != 0) return -1;
    sbuf b; sb_init(&b);
    sb_printf(&b,
        "---\n"
        "name: %s\n"
        "description: One line saying when this skill applies (the model and /skills show this).\n"
        "---\n"
        "\n"
        "Instructions for the model go here. Be concrete: which files to look at, which\n"
        "commands to run, what the result should look like.\n"
        "\n"
        "Arguments given on the command line (/%s foo bar) replace $ARGUMENTS below:\n"
        "\n"
        "Task: $ARGUMENTS\n"
        "\n"
        "<!-- Helper programs for this skill live next to this file. Write them in C\n"
        "     (build with cc / a Makefile in this directory); reach for Rust only when a\n"
        "     third-party library is really needed, and Python only as a last resort. -->\n",
        name, name);
    int rc = write_whole_file(path_out, b.data, b.len);
    sb_free(&b);
    return rc == 0 ? 0 : -1;
}
