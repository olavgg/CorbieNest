/* Skills: reusable instruction files (SKILL.md with a small YAML-ish
 * frontmatter), invoked as /name [args] from the prompt. Looked up in
 *   ./.corbienest/skills/<name>/SKILL.md   ./.corbienest/skills/<name>.md
 *   ./.corbienest/commands/<name>.md       ./.claude/commands/<name>.md   ~/.claude/commands/<name>.md
 *   ./.agents/skills/<name>/SKILL.md     (the cross-client convention of the Agent Skills standard, agentskills.io)
 *   ./.claude/skills/<name>/SKILL.md     (Claude Code layout, for sharing)
 *   $XDG_CONFIG_HOME/corbienest/skills/<name>/SKILL.md   (and <name>.md)
 *   ~/.agents/skills/<name>/SKILL.md     (the same convention, for every project)
 *   the bundled skills: $(PREFIX)/share/corbienest/skills (make install) and the checkout's skills/ (the build)
 *   ~/.claude/skills/<name>/SKILL.md     (Claude Code's own, read last: every other place wins over it)
 * Project skills shadow user skills, and user skills shadow the bundled ones, of the same name. The bundled
 * skills are read only: nothing is copied into the user's configuration, so an upgrade refreshes them. */
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

static void skill_free(skill_t *s) { free(s->name); free(s->desc); free(s->path); free(s->dir); free(s->body); free(s->model); }

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
static void parse_skill(const char *text, char **name, char **desc, char **body, char **model) {
    *name = *desc = *model = NULL;
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
            else if (!strcmp(k, "model") && v && *v) { free(*model); *model = v; v = NULL; }
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
    char *name, *desc, *body, *model;
    parse_skill(d, &name, &desc, &body, &model);
    free(d);
    if (!name || !*name) { free(name); name = xstrdup(fallback_name); }
    if (!valid_name(name) || name[0] == '/') { free(name); free(desc); free(body); free(model); return; }
    for (int i = 0; i < g_nskills; i++)   /* first hit wins (project before user) */
        if (!strcmp(g_skills[i].name, name)) { free(name); free(desc); free(body); free(model); return; }
    if (g_nskills == g_cap) { g_cap = g_cap ? g_cap * 2 : 16; g_skills = xrealloc(g_skills, sizeof(skill_t) * (size_t)g_cap); }
    skill_t *s = &g_skills[g_nskills++];
    s->name = name; s->desc = desc ? desc : xstrdup(""); s->path = xstrdup(path); s->dir = xstrdup(dir); s->body = body; s->model = model; s->source = source;
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
    scan_dir(".corbienest/commands", "project");   /* Claude-Code-style flat NAME.md custom commands */
    scan_dir(".agents/skills", "project");          /* the Agent Skills standard's cross-client folder: after our own, before Claude Code's */
    scan_dir(".claude/skills", "project");
    scan_dir(".claude/commands", "project");
    char user[1200]; snprintf(user, sizeof user, "%s/skills", config_dir());
    scan_dir(user, "user");
    snprintf(user, sizeof user, "%s/commands", config_dir());
    scan_dir(user, "user");
    char *ag = expand_home("~/.agents/skills"); scan_dir(ag, "user"); free(ag);   /* the same convention, for every project */
    char *home = expand_home("~/.claude/commands"); scan_dir(home, "user"); free(home);
#ifdef BUNDLED_SKILLS_DIR
    scan_dir(BUNDLED_SKILLS_DIR, "bundled");   /* last: a user's or a project's skill of the same name wins */
#endif
#ifdef BUNDLED_SKILLS_SRC
    scan_dir(BUNDLED_SKILLS_SRC, "bundled");   /* the checkout's own: the same names as the installed ones are not added twice */
#endif
    char *cc = expand_home("~/.claude/skills"); scan_dir(cc, "user"); free(cc);   /* Claude Code's, as they are: last, so a skill of ours of the same name wins (its synced/ folder holds no SKILL.md at this level, so the Claude app's skills do not show) */
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
    sb_puts(&b, "When asked to create or extend a skill: put it in .agents/skills/<name>/SKILL.md (frontmatter with name + description, then the instructions; "
                "$ARGUMENTS is replaced by the user's arguments). Helper programs that a skill needs must be written in C (compiled with cc/make); "
                "use Rust only when third-party libraries are genuinely required; Python is a last resort only.\n");
    return sb_detach(&b);
}

/* Scaffold .agents/skills/<name>/SKILL.md; returns 0 ok, -1 error (errno) , 1 exists */
int skill_scaffold(const char *name, char *path_out, size_t n) {
    if (!valid_name(name)) { errno = EINVAL; return -1; }
    char dir[1024]; snprintf(dir, sizeof dir, ".agents/skills/%s", name);
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

/* ---------- moving the skills and agents of .corbienest into .agents ----------
 * Only the skills/ and agents/ folders move: memory.md (rewritten by the program itself), commands/, hooks and the
 * rest of the config directory stay where they are, so .corbienest is removed only when nothing is left in it.
 * Nothing is overwritten, nothing is copied, nothing is removed that is not empty. */
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>

static bool is_real_dir(const char *path) {   /* a folder that is not a symlink */
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_symlink(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}
int dir_entries(const char *dir) {
    if (!is_real_dir(dir)) return -1;
    DIR *dp = opendir(dir);
    if (!dp) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp))) if (de->d_name[0] != '.') n++;   /* as scan_dir: hidden entries are not skills */
    closedir(dp);
    return n;
}

dir_move_result dir_migrate(const char *from, const char *to) {
    dir_move_result r; memset(&r, 0, sizeof r);
    if (!is_real_dir(from)) { r.refused = true; return r; }   /* a symlinked source may be shared: moving out of it would take files from elsewhere */
    char parent[1100]; snprintf(parent, sizeof parent, "%s", to);
    char *sl = strrchr(parent, '/'); if (sl) *sl = 0;
    if ((sl && is_symlink(parent)) || is_symlink(to)) { r.refused = true; return r; }   /* ... and so could a symlinked destination carry them out of the project */
    if (mkdir_p(to) != 0) { r.refused = true; return r; }
    DIR *dp = opendir(from);
    if (!dp) { r.refused = true; return r; }
    char **names = NULL; int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = xrealloc(names, sizeof(char*) * (size_t)cap); }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof(char*), cmp_name);
    size_t nl = 0;
    for (int i = 0; i < n; i++) {
        char src[2300], dst[2300]; struct stat st;
        snprintf(src, sizeof src, "%s/%s", from, names[i]); snprintf(dst, sizeof dst, "%s/%s", to, names[i]);
        bool left = false;
        if (lstat(dst, &st) == 0 || errno != ENOENT) { r.clashed++; left = true; }   /* rename(2) would replace it, silently: look first */
        else if (rename(src, dst) == 0) r.moved++;
        else if (errno == ENOENT) { /* another session moved it first */ }
        else { r.failed++; left = true; }   /* another filesystem (EXDEV) and the like: left where it is */
        if (left && nl + strlen(names[i]) + 3 < sizeof r.names) nl += (size_t)snprintf(r.names + nl, sizeof r.names - nl, "%s%s", nl ? ", " : "", names[i]);
        free(names[i]);
    }
    free(names);
    rmdir(from);   /* only when it ended empty */
    return r;
}

static char *declined_path(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/migrate-declined", config_dir());
    return xstrdup(p);
}
bool migrate_declined(const char *key) {
    char *p = declined_path(); size_t n; char *d = read_whole_file(p, &n, 64 * 1024); free(p);
    if (!d) return false;
    bool hit = false;
    char *save = NULL;
    for (char *l = strtok_r(d, "\n", &save); l && !hit; l = strtok_r(NULL, "\n", &save)) hit = !strcmp(l, key);
    free(d);
    return hit;
}
void migrate_decline(const char *key) {
    if (migrate_declined(key)) return;
    char *p = declined_path(); size_t n = 0; char *d = read_whole_file(p, &n, 64 * 1024);
    sbuf b; sb_init(&b);
    if (d) { sb_append(&b, d, n); if (n && d[n-1] != '\n') sb_putc(&b, '\n'); }
    sb_printf(&b, "%s\n", key);
    write_whole_file_atomic(p, b.data, b.len);
    sb_free(&b); free(d); free(p);
}

int migrate_pairs(bool user_scope, migrate_pair out[2]) {
    static const char *kinds[2] = { "skills", "agents" };
    char *home = expand_home("~");
    if (!user_scope) {   /* in $HOME the project's .agents IS ~/.agents: the user scope covers it */
        char cwdp[PATH_MAX], homep[PATH_MAX];   /* realpath(3) may write PATH_MAX bytes */
        if (!realpath(".", cwdp) || (home && realpath(home, homep) && !strcmp(cwdp, homep))) { free(home); return 0; }
    }
    int n = 0;
    for (int k = 0; k < 2; k++) {
        migrate_pair *p = &out[n];
        memset(p, 0, sizeof *p);
        if (user_scope) {
            snprintf(p->from, sizeof p->from, "%s/%s", config_dir(), kinds[k]);
            snprintf(p->to, sizeof p->to, "%s/.agents/%s", home ? home : "", kinds[k]);
        } else {
            snprintf(p->from, sizeof p->from, ".corbienest/%s", kinds[k]);
            snprintf(p->to, sizeof p->to, ".agents/%s", kinds[k]);
        }
        p->kind = kinds[k];
        p->n = dir_entries(p->from);
        if (p->n <= 0) continue;
        char rt[PATH_MAX], rf[PATH_MAX];
        if (!realpath(p->from, rf)) continue;
        size_t kl = strlen(rf);
        if (kl >= sizeof p->key) continue;   /* a path that long is not one we keep a decline for */
        memcpy(p->key, rf, kl + 1);
        if (realpath(p->to, rt) && !strcmp(rf, rt)) continue;   /* the same folder (a config dir that is ~/.agents) */
        if (migrate_declined(p->key)) continue;
        n++;
    }
    free(home);
    return n;
}

/* Files below dir that still name an old path: a skill that says "run .corbienest/skills/x/run.sh" is broken by the move. */
static void mentions_walk(const char *dir, int depth, const char *const *needles, int nn, sbuf *out, int *hits, int max_hits, int *files) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) && *hits < max_hits && *files < 400) {
        if (de->d_name[0] == '.') continue;
        char path[2300]; snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) mentions_walk(path, depth + 1, needles, nn, out, hits, max_hits, files); continue; }
        if (!S_ISREG(st.st_mode) || st.st_size > 256 * 1024) continue;
        (*files)++;
        size_t len; char *d = read_whole_file(path, &len, 256 * 1024);
        if (!d) continue;
        if (memchr(d, 0, len)) { free(d); continue; }   /* a binary */
        int line = 1;
        for (char *p = d; p && *p && *hits < max_hits; line++) {
            char *e = strchr(p, '\n');
            size_t ll = e ? (size_t)(e - p) : strlen(p);
            for (int i = 0; i < nn; i++) {
                size_t nl = strlen(needles[i]);
                for (const char *q = p; q + nl <= p + ll; q++) if (!memcmp(q, needles[i], nl)) { sb_printf(out, "%s:%d\n", path, line); (*hits)++; goto next_line; }
            }
            next_line:
            p = e ? e + 1 : NULL;
        }
        free(d);
    }
    closedir(dp);
}
int text_mentions(const char *dir, const char *const *needles, int nn, sbuf *out, int max_hits) {
    int hits = 0, files = 0;
    mentions_walk(dir, 0, needles, nn, out, &hits, max_hits, &files);
    return hits;
}
