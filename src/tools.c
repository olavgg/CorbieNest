/* Agent tools: read_file, write_file, edit_file, list_dir, grep, bash, task (sub-agent).
 * Mutating / shell tools ask the user for confirmation depending on the permission mode. */
#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>

#define CONF_EDIT_K 0
#define CONF_BASH_K 1
#define READ_CAP   (256 * 1024)
#define OUT_CAP    (64 * 1024)
#define MAX_ENTRIES 400

static bool g_always_write = false, g_always_edit = false, g_always_bash = false;
bool tools_no_confirm = false;   /* set by the caller for user-typed "!cmd": no prompt, no plan-mode veto */
void tools_reset_permissions(void) { g_always_write = g_always_edit = g_always_bash = false; }
const char *tools_summary_line(void) { return "read_file, write_file, edit_file, list_dir, grep, bash, task"; }

/* ---------- persistent project permissions ----------
 * .corbienest/permissions holds one rule per line: "edit" (file writes/edits are fine in
 * this project) or "bash <words>" (shell commands whose leading words match, e.g. "bash git
 * status", "bash make"). Commands with shell metacharacters (; | & $ ` > < newline) never
 * match a rule — a chained command is not "just git status". */
#define PERM_PATH ".corbienest/permissions"
#define PERM_MAX  200
static char *g_perm[PERM_MAX]; static int g_perm_n = 0;

void tools_permissions_load(void) {
    for (int i = 0; i < g_perm_n; i++) free(g_perm[i]);
    g_perm_n = 0;
    size_t n; char *d = read_whole_file(PERM_PATH, &n, 64 * 1024);
    if (!d) return;
    char *save = NULL;
    for (char *l = strtok_r(d, "\n", &save); l && g_perm_n < PERM_MAX; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ' || *l == '\t') l++;
        size_t k = strlen(l); while (k && (l[k-1] == ' ' || l[k-1] == '\r' || l[k-1] == '\t')) l[--k] = 0;
        if (!*l || *l == '#') continue;
        g_perm[g_perm_n++] = xstrdup(l);
    }
    free(d);
}
static void perm_save(void) {
    if (!g_perm_n) { unlink(PERM_PATH); return; }
    sbuf b; sb_init(&b);
    sb_puts(&b, "# corbienest project permissions — one rule per line: \"edit\" or \"bash <leading words>\" (see /permissions)\n");
    for (int i = 0; i < g_perm_n; i++) sb_printf(&b, "%s\n", g_perm[i]);
    mkdir_p(".corbienest");
    write_whole_file(PERM_PATH, b.data, b.len);
    sb_free(&b);
}
int tools_permissions_count(void) { return g_perm_n; }
const char *tools_permissions_get(int i) { return i >= 0 && i < g_perm_n ? g_perm[i] : NULL; }
bool tools_permissions_add(const char *rule) {
    for (int i = 0; i < g_perm_n; i++) if (!strcmp(g_perm[i], rule)) return false;
    if (g_perm_n >= PERM_MAX) { free(g_perm[0]); memmove(g_perm, g_perm + 1, sizeof(char*) * (PERM_MAX - 1)); g_perm_n--; }
    g_perm[g_perm_n++] = xstrdup(rule);
    perm_save();
    return true;
}
bool tools_permissions_remove(int i) {
    if (i < 0 || i >= g_perm_n) return false;
    free(g_perm[i]); memmove(g_perm + i, g_perm + i + 1, sizeof(char*) * (size_t)(g_perm_n - i - 1)); g_perm_n--;
    perm_save();
    return true;
}
void tools_permissions_clear(void) { for (int i = 0; i < g_perm_n; i++) free(g_perm[i]); g_perm_n = 0; unlink(PERM_PATH); }

/* Rule text a command would be remembered under: its first word, plus the subcommand for
 * tools that have them (git, npm, cargo, docker …). Empty if the command is not "simple". */
static void bash_rule_for(const char *cmd, char *out, size_t n) {
    out[0] = 0;
    if (strpbrk(cmd, ";|&$`<>\n")) return;
    static const char *sub[] = { "git", "npm", "npx", "pnpm", "yarn", "cargo", "go", "docker", "kubectl", "gh", "pip", "pip3", "python", "python3", "uv", "poetry", NULL };
    const char *p = cmd; while (*p == ' ') p++;
    const char *e = p; while (*e && *e != ' ') e++;
    if (e == p) return;
    size_t l = (size_t)(e - p);
    snprintf(out, n, "bash %.*s", (int)l, p);
    for (int i = 0; sub[i]; i++) if (strlen(sub[i]) == l && !strncmp(sub[i], p, l)) {
        const char *q = e; while (*q == ' ') q++;
        const char *qe = q; while (*qe && *qe != ' ') qe++;
        if (qe > q && *q != '-') { size_t used = strlen(out); snprintf(out + used, n - used, " %.*s", (int)(qe - q), q); }
        break;
    }
}
/* does a saved rule cover this command? (rule words are a prefix of the command's words) */
static bool bash_rule_matches(const char *rule, const char *cmd) {
    if (strncmp(rule, "bash ", 5)) return false;
    if (strpbrk(cmd, ";|&$`<>\n")) return false;
    const char *r = rule + 5, *c = cmd;
    while (*c == ' ') c++;
    for (;;) {
        while (*r == ' ') r++;
        if (!*r) return true;
        const char *re = r; while (*re && *re != ' ') re++;
        while (*c == ' ') c++;
        const char *ce = c; while (*ce && *ce != ' ') ce++;
        if (ce - c != re - r || strncmp(r, c, (size_t)(re - r))) return false;
        r = re; c = ce;
    }
}
static const char *perm_match(int kind, const char *cmd) {
    for (int i = 0; i < g_perm_n; i++) {
        if (kind == CONF_EDIT_K && !strcmp(g_perm[i], "edit")) return g_perm[i];
        if (kind == CONF_BASH_K && cmd && bash_rule_matches(g_perm[i], cmd)) return g_perm[i];
    }
    return NULL;
}

/* ---------- checkpoints (for /rewind) ----------
 * Before write_file/edit_file touch a file, its previous content (or "did not exist") is
 * kept together with the request ("turn") it happened in. tools_checkpoint_restore(turn)
 * puts every file changed in that request or later back the way it was — newest change
 * first, so a file edited several times ends up at its oldest saved state. In memory
 * only (per process), capped at CKPT_MAX_BYTES. */
#define CKPT_MAX_BYTES (64u * 1024 * 1024)
typedef struct { int turn; char *path; char *content; size_t len; bool existed; } ckpt_t;
static ckpt_t *g_ckpt; static int g_ckpt_n, g_ckpt_cap; static size_t g_ckpt_bytes; static int g_ckpt_turn = -1;

void tools_checkpoint_turn(int turn) { g_ckpt_turn = turn; }
static void ckpt_free(ckpt_t *c) { free(c->path); free(c->content); g_ckpt_bytes -= c->len; }
void tools_checkpoint_clear(void) { for (int i = 0; i < g_ckpt_n; i++) ckpt_free(&g_ckpt[i]); g_ckpt_n = 0; }
static void ckpt_save(const char *path) {
    if (g_ckpt_turn < 0) return;
    for (int i = 0; i < g_ckpt_n; i++) if (g_ckpt[i].turn == g_ckpt_turn && !strcmp(g_ckpt[i].path, path)) return;   /* first state in this turn wins */
    ckpt_t c = { g_ckpt_turn, xstrdup(path), NULL, 0, false };
    if (is_file(path)) { c.content = read_whole_file(path, &c.len, CKPT_MAX_BYTES); if (!c.content) { free(c.path); return; } c.existed = true; }
    while (g_ckpt_n && g_ckpt_bytes + c.len > CKPT_MAX_BYTES) { ckpt_free(&g_ckpt[0]); memmove(g_ckpt, g_ckpt + 1, sizeof *g_ckpt * (size_t)(g_ckpt_n - 1)); g_ckpt_n--; }
    if (g_ckpt_n == g_ckpt_cap) { g_ckpt_cap = g_ckpt_cap ? g_ckpt_cap * 2 : 32; g_ckpt = xrealloc(g_ckpt, sizeof *g_ckpt * (size_t)g_ckpt_cap); }
    g_ckpt[g_ckpt_n++] = c; g_ckpt_bytes += c.len;
}
/* files changed in `turn` or later: distinct paths, appended comma-separated to `names` (may be NULL); returns the count */
int tools_checkpoint_files(int turn, sbuf *names) {
    int n = 0;
    for (int i = 0; i < g_ckpt_n; i++) {
        if (g_ckpt[i].turn < turn) continue;
        bool seen = false; for (int j = 0; j < i; j++) if (g_ckpt[j].turn >= turn && !strcmp(g_ckpt[j].path, g_ckpt[i].path)) { seen = true; break; }
        if (seen) continue;
        if (names) sb_printf(names, "%s%s", n ? ", " : "", g_ckpt[i].path);
        n++;
    }
    return n;
}
int tools_checkpoint_restore(int turn) {
    int n = 0;
    for (int i = g_ckpt_n - 1; i >= 0; i--) {
        ckpt_t *c = &g_ckpt[i];
        if (c->turn < turn) continue;
        if (c->existed) write_whole_file(c->path, c->content, c->len); else unlink(c->path);
        n++;
    }
    /* drop the used records (they are ≥ turn; all such sit at the tail because turns only grow) */
    int keep = 0; for (int i = 0; i < g_ckpt_n; i++) { if (g_ckpt[i].turn >= turn) ckpt_free(&g_ckpt[i]); else g_ckpt[keep++] = g_ckpt[i]; }
    g_ckpt_n = keep;
    return n;
}

/* ---------- tool definitions ---------- */
static cJSON *mk_tool(const char *name, const char *desc, cJSON *props, const char *required[]) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "function");
    cJSON *f = cJSON_AddObjectToObject(t, "function");
    cJSON_AddStringToObject(f, "name", name);
    cJSON_AddStringToObject(f, "description", desc);
    cJSON *p = cJSON_AddObjectToObject(f, "parameters");
    cJSON_AddStringToObject(p, "type", "object");
    cJSON_AddItemToObject(p, "properties", props);
    cJSON *req = cJSON_AddArrayToObject(p, "required");
    for (int i = 0; required[i]; i++) cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
    return t;
}
static cJSON *prop(cJSON *props, const char *name, const char *type, const char *desc) {
    cJSON *o = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "description", desc);
    return o;
}

cJSON *tools_definitions(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *p;

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Path to the file (relative to the working directory or absolute).");
    prop(p, "offset", "integer", "Optional 1-based line number to start reading from.");
    prop(p, "limit", "integer", "Optional maximum number of lines to return.");
    cJSON_AddItemToArray(arr, mk_tool("read_file",
        "Read a text file. Returns the content with line numbers. Use offset/limit for large files.",
        p, (const char*[]){"path", NULL}));

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Path of the file to create or overwrite.");
    prop(p, "content", "string", "The full new content of the file.");
    cJSON_AddItemToArray(arr, mk_tool("write_file",
        "Create a new file or completely overwrite an existing file with the given content. Parent directories are created automatically. Prefer edit_file for small changes to existing files.",
        p, (const char*[]){"path", "content", NULL}));

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Path of the file to edit.");
    prop(p, "old_string", "string", "Exact text to find (must match exactly, including whitespace/indentation, and be unique in the file unless replace_all is true).");
    prop(p, "new_string", "string", "Replacement text.");
    prop(p, "replace_all", "boolean", "Replace every occurrence instead of requiring uniqueness. Default false.");
    cJSON_AddItemToArray(arr, mk_tool("edit_file",
        "Edit an existing file by replacing an exact string with a new string. Read the file first so old_string matches exactly.",
        p, (const char*[]){"path", "old_string", "new_string", NULL}));

    p = cJSON_CreateObject();
    prop(p, "path", "string", "Directory to list. Defaults to the current working directory.");
    cJSON_AddItemToArray(arr, mk_tool("list_dir",
        "List files and subdirectories in a directory (non-recursive).",
        p, (const char*[]){NULL}));

    p = cJSON_CreateObject();
    prop(p, "pattern", "string", "Regular expression (grep -E syntax) to search for.");
    prop(p, "path", "string", "File or directory to search recursively. Defaults to the current working directory.");
    prop(p, "include", "string", "Optional glob to restrict files, e.g. \"*.c\" or \"*.py\".");
    cJSON_AddItemToArray(arr, mk_tool("grep",
        "Search file contents recursively with a regular expression. Returns matching lines as path:line:text.",
        p, (const char*[]){"pattern", NULL}));

    p = cJSON_CreateObject();
    prop(p, "command", "string", "The shell command to run (executed with /bin/sh -c). Use it for git, build tools, tests, package managers, find, etc.");
    prop(p, "timeout", "integer", "Optional timeout in seconds (default 120, max 600).");
    cJSON_AddItemToArray(arr, mk_tool("bash",
        "Run a shell command in the working directory and return its combined stdout/stderr and exit code. Use for git operations, running builds/tests, installing packages, and anything the other tools do not cover. Avoid interactive commands.",
        p, (const char*[]){"command", NULL}));

    p = cJSON_CreateObject();
    prop(p, "description", "string", "A short (3-6 word) label for what the sub-agent does, shown to the user.");
    prop(p, "prompt", "string", "The complete task for the sub-agent. It starts with a fresh context and sees nothing of this conversation, so include every relevant detail: what to look for, where, and exactly what to report back.");
    cJSON_AddItemToArray(arr, mk_tool("task",
        "Delegate a self-contained research or exploration task to a sub-agent with its own fresh context. It can read files, list directories, grep and run read-only shell commands, but cannot modify files, and returns a text report. "
        "Use it for broad searches or investigations whose intermediate output would clutter your context (\"find every place X is handled and summarise\", \"figure out how the build works\"), not for simple one-file lookups.",
        p, (const char*[]){"description", "prompt", NULL}));
    return arr;
}

/* ---------- helpers ---------- */
static const char *jstr(cJSON *o, const char *k, const char *dflt) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    if (cJSON_IsNumber(v)) { static char nb[64]; snprintf(nb, sizeof nb, "%g", v->valuedouble); return nb; }
    return dflt;
}
static int jint(cJSON *o, const char *k, int dflt) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (int)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && isdigit((unsigned char)v->valuestring[0])) return atoi(v->valuestring);
    return dflt;
}
static bool jbool(cJSON *o, const char *k, bool dflt) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring) return !strcasecmp(v->valuestring, "true");
    return dflt;
}

/* Truncate a big output keeping head and tail. */
static void cap_output(sbuf *b, size_t cap) {
    if (b->len <= cap) return;
    size_t head = cap * 2 / 3, tail = cap - head;
    sbuf n; sb_init(&n);
    sb_append(&n, b->data, head);
    sb_printf(&n, "\n\n[... output truncated: %zu bytes omitted ...]\n\n", b->len - head - tail);
    sb_append(&n, b->data + b->len - tail, tail);
    sb_free(b);
    *b = n;
}

/* Print a few lines of text with a prefix, coloured, for previews. */
static void preview_lines(const char *text, int max_lines, const char *color) {
    int lines = 0, total = 0;
    for (const char *p = text; *p; p++) if (*p == '\n') total++;
    if (*text && text[strlen(text)-1] != '\n') total++;
    const char *p = text;
    size_t w = (size_t)term_width(); w = w > 8 ? w - 4 : 4;   /* 2 indent + room for the ellipsis */
    while (*p && lines < max_lines) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        printf("  %s%.*s%s" C_RESET "\n", color, (int)(n > w ? w : n), p, n > w ? "…" : "");
        lines++;
        if (!e) break;
        p = e + 1;
    }
    if (total > lines) printf("  " C_DIM "… (%d more lines)" C_RESET "\n", total - lines);
}

/* Confirmation prompt. Returns 1 allow, 0 deny. On deny, *reason may be set (malloc'd).
 * kind: CONF_EDIT for write/edit, CONF_BASH for shell commands; `cmd` is the shell command
 * (for the project rules), NULL for edits. */
enum { CONF_EDIT = CONF_EDIT_K, CONF_BASH = CONF_BASH_K };
static int confirm(const char *what, int kind, bool *always_flag, const char *cmd, char **reason) {
    *reason = NULL;
    if (always_flag && *always_flag) return 1;
    if (tools_no_confirm) return 1;
    if (YOLO() || (kind == CONF_EDIT && g_cfg.mode == MODE_ACCEPT_EDITS)) {
        if (g_cfg.interactive) printf("  " C_YELLOW "%s" C_RESET "  " C_DIM "auto-approved (%s mode)" C_RESET "\n", what, mode_name(g_cfg.mode));
        return 1;
    }
    if (kind == CONF_EDIT && g_cfg.mode == MODE_PLAN) {
        printf("  " C_YELLOW "%s" C_RESET " " C_RED "denied (plan mode is read-only)" C_RESET "\n", what);
        *reason = xstrdup("corbienest is in plan mode (read-only): do not modify files. Present your plan as text; the user will switch modes (shift+tab) when they want it implemented.");
        return 0;
    }
    const char *rule = perm_match(kind, cmd);
    if (rule) {
        printf("  " C_YELLOW "%s" C_RESET "  " C_DIM "auto-approved (project rule: %s)" C_RESET "\n", what, rule);
        return 1;
    }
    if (!g_cfg.interactive) {
        printf("  " C_YELLOW "%s" C_RESET " " C_RED "denied (non-interactive; use --yolo or --mode to allow)" C_RESET "\n", what);
        *reason = xstrdup("corbienest is running non-interactively and cannot ask for confirmation. Continue without this action.");
        return 0;
    }
    const char *always = kind == CONF_BASH ? "Yes, and don't ask again for shell commands this session"
                                           : "Yes, and don't ask again for file edits this session";
    char newrule[256] = "", plabel[320] = "";
    if (kind == CONF_BASH) { bash_rule_for(cmd, newrule, sizeof newrule); if (newrule[0]) snprintf(plabel, sizeof plabel, "Yes, and always allow `%s …` in this project", newrule + 5); }
    else { snprintf(newrule, sizeof newrule, "edit"); snprintf(plabel, sizeof plabel, "Yes, and always allow file edits in this project"); }
    int r = term_confirm(what, always, newrule[0] ? plabel : NULL, reason);
    if (r == 2 && always_flag) *always_flag = true;
    if (r == 3 && newrule[0]) { tools_permissions_add(newrule); printf("  " C_DIM "saved to " PERM_PATH ": %s" C_RESET "\n", newrule); }
    return r > 0;
}

/* ---------- read_file ---------- */
static tool_status t_read_file(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", NULL);
    if (!path) { sb_puts(out, "error: missing 'path'"); return TOOL_ERROR; }
    char *p = expand_home(path);
    if (is_dir(p)) { sb_printf(out, "error: %s is a directory (use list_dir)", path); free(p); return TOOL_ERROR; }
    size_t len = 0;
    char *d = read_whole_file(p, &len, READ_CAP + 1);
    if (!d) { sb_printf(out, "error: cannot read %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    bool truncated = len > READ_CAP;
    if (truncated) len = READ_CAP;
    /* binary check */
    for (size_t i = 0; i < len && i < 4096; i++) if (d[i] == 0) { sb_printf(out, "error: %s appears to be a binary file", path); free(d); free(p); return TOOL_ERROR; }
    int offset = jint(args, "offset", 1), limit = jint(args, "limit", 0);
    if (offset < 1) offset = 1;
    int lineno = 0, emitted = 0;
    const char *s = d, *end = d + len;
    while (s < end) {
        const char *e = memchr(s, '\n', (size_t)(end - s));
        size_t n = e ? (size_t)(e - s) : (size_t)(end - s);
        lineno++;
        if (lineno >= offset && (limit <= 0 || emitted < limit)) {
            sb_printf(out, "%6d| ", lineno);
            sb_append(out, s, n);
            sb_putc(out, '\n');
            emitted++;
        }
        if (!e) break;
        s = e + 1;
        if (limit > 0 && emitted >= limit) break;
    }
    if (emitted == 0) sb_printf(out, "(no lines in range; file has %d lines)\n", lineno);
    if (truncated && (limit <= 0)) sb_printf(out, "\n[file truncated at %d KB; use offset/limit to read more]\n", READ_CAP / 1024);
    free(d); free(p);
    return TOOL_OK;
}

/* ---------- write_file ---------- */
static tool_status t_write_file(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", NULL);
    const char *content = jstr(args, "content", NULL);
    if (!path || !content) { sb_puts(out, "error: need 'path' and 'content'"); return TOOL_ERROR; }
    char *p = expand_home(path);
    bool exists = is_file(p);
    printf("  " C_DIM "%s %s (%zu bytes)" C_RESET "\n", exists ? "overwrite" : "create", path, strlen(content));
    preview_lines(content, 12, C_GREEN);
    char *reason = NULL;
    if (!confirm(exists ? "Overwrite this file?" : "Create this file?", CONF_EDIT, &g_always_write, NULL, &reason)) {
        sb_printf(out, "User denied writing %s.%s%s", path, reason ? " Reason: " : "", reason ? reason : "");
        free(reason); free(p); return TOOL_DENIED;
    }
    /* mkdir parents */
    char *dir = xstrdup(p); char *sl = strrchr(dir, '/');
    if (sl && sl != dir) { *sl = 0; mkdir_p(dir); }
    free(dir);
    ckpt_save(p);
    if (write_whole_file(p, content, strlen(content)) != 0) { sb_printf(out, "error: cannot write %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    int lines = 0; for (const char *c = content; *c; c++) if (*c == '\n') lines++;
    sb_printf(out, "Wrote %zu bytes (%d lines) to %s", strlen(content), lines, path);
    free(p);
    return TOOL_OK;
}

/* ---------- edit_file ---------- */
static int count_occ(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    if (!nl) return 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += nl) c++;
    return c;
}
static tool_status t_edit_file(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", NULL);
    const char *olds = jstr(args, "old_string", NULL);
    const char *news = jstr(args, "new_string", NULL);
    bool all = jbool(args, "replace_all", false);
    if (!path || !olds || !news) { sb_puts(out, "error: need 'path', 'old_string' and 'new_string'"); return TOOL_ERROR; }
    if (!*olds) { sb_puts(out, "error: old_string must not be empty (use write_file to create files)"); return TOOL_ERROR; }
    char *p = expand_home(path);
    size_t len; char *d = read_whole_file(p, &len, 0);
    if (!d) { sb_printf(out, "error: cannot read %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    int occ = count_occ(d, olds);
    if (occ == 0) { sb_printf(out, "error: old_string not found in %s. Re-read the file and match the text exactly (whitespace matters).", path); free(d); free(p); return TOOL_ERROR; }
    if (occ > 1 && !all) { sb_printf(out, "error: old_string occurs %d times in %s; include more surrounding context to make it unique, or set replace_all=true.", occ, path); free(d); free(p); return TOOL_ERROR; }
    /* build result */
    sbuf nb; sb_init(&nb);
    size_t ol = strlen(olds);
    const char *s = d;
    for (;;) {
        const char *m = strstr(s, olds);
        if (!m) { sb_puts(&nb, s); break; }
        sb_append(&nb, s, (size_t)(m - s));
        sb_puts(&nb, news);
        s = m + ol;
        if (!all) { sb_puts(&nb, s); break; }
    }
    printf("  " C_DIM "edit %s (%d replacement%s)" C_RESET "\n", path, occ, occ == 1 ? "" : "s");
    /* diff-ish preview */
    { sbuf t; sb_init(&t); const char *q = olds; while (*q) { const char *e = strchr(q, '\n'); size_t n = e ? (size_t)(e-q) : strlen(q); sb_printf(&t, "- %.*s\n", (int)n, q); if (!e) break; q = e + 1; } preview_lines(t.data, 10, C_RED); sb_free(&t); }
    { sbuf t; sb_init(&t); const char *q = news; if (!*q) sb_puts(&t, "+ (deleted)\n"); while (*q) { const char *e = strchr(q, '\n'); size_t n = e ? (size_t)(e-q) : strlen(q); sb_printf(&t, "+ %.*s\n", (int)n, q); if (!e) break; q = e + 1; } preview_lines(t.data, 10, C_GREEN); sb_free(&t); }
    char *reason = NULL;
    if (!confirm("Apply this edit?", CONF_EDIT, &g_always_edit, NULL, &reason)) {
        sb_printf(out, "User denied editing %s.%s%s", path, reason ? " Reason: " : "", reason ? reason : "");
        free(reason); sb_free(&nb); free(d); free(p); return TOOL_DENIED;
    }
    ckpt_save(p);
    if (write_whole_file(p, nb.data ? nb.data : "", nb.len) != 0) { sb_printf(out, "error: cannot write %s: %s", path, strerror(errno)); sb_free(&nb); free(d); free(p); return TOOL_ERROR; }
    sb_printf(out, "Edited %s: %d replacement%s made.", path, occ, occ == 1 ? "" : "s");
    sb_free(&nb); free(d); free(p);
    return TOOL_OK;
}

/* ---------- list_dir ---------- */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static tool_status t_list_dir(cJSON *args, sbuf *out) {
    const char *path = jstr(args, "path", ".");
    if (!*path) path = ".";
    char *p = expand_home(path);
    DIR *d = opendir(p);
    if (!d) { sb_printf(out, "error: cannot open %s: %s", path, strerror(errno)); free(p); return TOOL_ERROR; }
    char **names = NULL; int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = xrealloc(names, sizeof(char*) * (size_t)cap); }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char*), cmp_str);
    sb_printf(out, "%s/ (%d entries)\n", path, n);
    for (int i = 0; i < n && i < MAX_ENTRIES; i++) {
        char full[4096]; snprintf(full, sizeof full, "%s/%s", p, names[i]);
        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) sb_printf(out, "  %s/\n", names[i]);
            else if (S_ISLNK(st.st_mode)) sb_printf(out, "  %s@\n", names[i]);
            else sb_printf(out, "  %s  (%lld bytes)\n", names[i], (long long)st.st_size);
        } else sb_printf(out, "  %s\n", names[i]);
    }
    if (n > MAX_ENTRIES) sb_printf(out, "  ... %d more entries not shown\n", n - MAX_ENTRIES);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names); free(p);
    return TOOL_OK;
}

/* ---------- shell runner ---------- */
/* Runs cmd via /bin/sh -c, capturing stdout+stderr. Returns exit code (or -1 on failure,
 * 124 on timeout, 130 if interrupted by user). */
static int run_shell(const char *cmd, int timeout_s, sbuf *out, bool *interrupted) {
    *interrupted = false;
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        dup2(pfd[1], STDOUT_FILENO); dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]); close(pfd[1]);
        /* restore default signals */
        signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        setenv("GIT_TERMINAL_PROMPT", "0", 1);
        setenv("PAGER", "cat", 1);
        setenv("GIT_PAGER", "cat", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    setpgid(pid, pid);
    struct pollfd fds[2] = { { pfd[0], POLLIN, 0 }, { g_cfg.interactive ? STDIN_FILENO : -1, POLLIN, 0 } };
    long remaining_ms = (long)timeout_s * 1000;
    bool timed_out = false;
    term_raw(true);
    term_busy("running command");
    struct timeval t0; gettimeofday(&t0, NULL);
    int spin = 0; bool spin_shown = false;
    for (;;) {
        int r = poll(fds, 2, 100);
        if (r < 0) { if (errno == EINTR) continue; break; }
        term_busy_tick();
        if (r == 0) {
            remaining_ms -= 100; if (remaining_ms <= 0) { timed_out = true; break; }
            if (g_cfg.interactive) {   /* inline spinner: nothing else moves while the command runs */
                struct timeval t; gettimeofday(&t, NULL);
                double el = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_usec - t0.tv_usec) / 1e6;
                static const char *SP[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
                spin = (spin + 1) % 10;
                printf("\r\x1b[2K  " C_ORANGE "%s" C_RESET C_DIM " running… (%.0fs)  " C_GRAY "ctrl-c to interrupt" C_RESET, SP[spin], el);
                fflush(stdout); spin_shown = true;
            }
            continue;
        }
        if (fds[1].revents & POLLIN) {
            if (term_poll_interrupt()) { *interrupted = true; break; }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[8192]; ssize_t n = read(pfd[0], buf, sizeof buf);
            if (n > 0) { sb_append(out, buf, (size_t)n); if (out->len > OUT_CAP * 4) cap_output(out, OUT_CAP * 2); }
            else break;   /* EOF */
        }
    }
    if (spin_shown) { fputs("\r\x1b[2K", stdout); fflush(stdout); }
    term_busy(NULL);
    if (timed_out || *interrupted) { kill(-pid, SIGTERM); usleep(200 * 1000); kill(-pid, SIGKILL); }
    /* drain */
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (timed_out) return 124;
    if (*interrupted) return 130;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static tool_status t_bash(cJSON *args, sbuf *out) {
    const char *cmd = jstr(args, "command", NULL);
    if (!cmd || !*cmd) { sb_puts(out, "error: missing 'command'"); return TOOL_ERROR; }
    int timeout = jint(args, "timeout", 120);
    if (timeout <= 0) timeout = 120;
    if (timeout > 600) timeout = 600;
    printf("  " C_DIM "$ " C_RESET C_BOLD "%s" C_RESET "\n", cmd);
    char *reason = NULL;
    if (!confirm("Run this command?", CONF_BASH, &g_always_bash, cmd, &reason)) {
        sb_printf(out, "User denied running the command.%s%s", reason ? " Reason: " : "", reason ? reason : "");
        free(reason); return TOOL_DENIED;
    }
    sbuf o; sb_init(&o);
    bool intr = false;
    int rc = run_shell(cmd, timeout, &o, &intr);
    cap_output(&o, OUT_CAP);
    if (rc == 124) sb_printf(out, "(command timed out after %ds and was killed)\n", timeout);
    if (intr) sb_puts(out, "(command interrupted by user with Ctrl-C)\n");
    if (o.len) { sb_append(out, o.data, o.len); if (o.data[o.len-1] != '\n') sb_putc(out, '\n'); }
    else sb_puts(out, "(no output)\n");
    sb_printf(out, "exit code: %d", rc);
    sb_free(&o);
    return TOOL_OK;
}

/* ---------- grep ---------- */
static void sh_quote(sbuf *b, const char *s) {
    sb_putc(b, '\'');
    for (; *s; s++) { if (*s == '\'') sb_puts(b, "'\\''"); else sb_putc(b, *s); }
    sb_putc(b, '\'');
}
static tool_status t_grep(cJSON *args, sbuf *out) {
    const char *pat = jstr(args, "pattern", NULL);
    const char *path = jstr(args, "path", ".");
    const char *inc = jstr(args, "include", NULL);
    if (!pat || !*pat) { sb_puts(out, "error: missing 'pattern'"); return TOOL_ERROR; }
    if (!*path) path = ".";
    sbuf cmd; sb_init(&cmd);
    sb_puts(&cmd, "grep -rnIE --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=.venv --exclude-dir=__pycache__ ");
    if (inc && *inc) { sb_puts(&cmd, "--include="); sh_quote(&cmd, inc); sb_putc(&cmd, ' '); }
    sb_puts(&cmd, "-e "); sh_quote(&cmd, pat); sb_puts(&cmd, " -- ");
    char *p = expand_home(path); sh_quote(&cmd, p); free(p);
    sb_puts(&cmd, " 2>/dev/null | head -n 300");
    sbuf o; sb_init(&o); bool intr;
    run_shell(cmd.data, 60, &o, &intr);
    cap_output(&o, OUT_CAP);
    if (!o.len) sb_printf(out, "No matches for /%s/ in %s", pat, path);
    else { sb_append(out, o.data, o.len); }
    sb_free(&o); sb_free(&cmd);
    return TOOL_OK;
}

/* ---------- task (sub-agent) ---------- */
tools_subagent_fn tools_subagent = NULL;
static bool g_in_subagent = false;
static tool_status t_task(cJSON *args, sbuf *out) {
    const char *desc = jstr(args, "description", "sub-agent");
    const char *prompt = jstr(args, "prompt", NULL);
    if (!prompt || !*prompt) { sb_puts(out, "error: missing 'prompt'"); return TOOL_ERROR; }
    if (!tools_subagent) { sb_puts(out, "error: sub-agents are not available here"); return TOOL_ERROR; }
    if (g_in_subagent) { sb_puts(out, "error: a sub-agent cannot start another sub-agent; do the work yourself"); return TOOL_ERROR; }
    g_in_subagent = true;
    int rc = tools_subagent(desc, prompt, out);
    g_in_subagent = false;
    return rc == 0 ? TOOL_OK : TOOL_ERROR;
}

/* ---------- dispatch ---------- */
tool_status tools_execute(const char *name, cJSON *args, sbuf *out) {
    cJSON *tmp = NULL;
    if (!args || !cJSON_IsObject(args)) { tmp = cJSON_CreateObject(); args = tmp; }
    tool_status st;
    if (!strcmp(name, "read_file")) st = t_read_file(args, out);
    else if (!strcmp(name, "write_file")) st = t_write_file(args, out);
    else if (!strcmp(name, "edit_file")) st = t_edit_file(args, out);
    else if (!strcmp(name, "list_dir")) st = t_list_dir(args, out);
    else if (!strcmp(name, "grep")) st = t_grep(args, out);
    else if (!strcmp(name, "bash") || !strcmp(name, "shell") || !strcmp(name, "run_command")) st = t_bash(args, out);
    else if (!strcmp(name, "task")) st = t_task(args, out);
    else { sb_printf(out, "error: unknown tool '%s'. Available tools: %s", name, tools_summary_line()); st = TOOL_ERROR; }
    if (tmp) cJSON_Delete(tmp);
    return st;
}
