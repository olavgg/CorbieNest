/* capture: run corbienest's notcurses screen in a pty and save the bytes it draws.
 *
 *   capture PORT OUT COLS ROWS MODEL SECONDS CWD [BINARY]
 *
 * The program runs at COLS x ROWS with OLLAMA_HOST=127.0.0.1:PORT (a fake server, see SKILL.md), in CWD,
 * with a scratch config directory. notcurses asks the terminal two questions when it starts (the device
 * attributes query and the cursor report); both are answered here, so it gets past its start. The output is
 * read for SECONDS, then the program is killed. Only the child this starts is killed.
 *
 * The bytes go to OUT; screen.c draws them. Built by the Makefile next to this file. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* nftw callback: removes what it visits (files first, then the directories: FTW_DEPTH) */
static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(path);
}

static int has(const unsigned char *b, size_t n, const char *pat) {
    return memmem(b, n, pat, strlen(pat)) != NULL;
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: capture PORT OUT COLS ROWS MODEL SECONDS CWD [BINARY]\n");
        return 2;
    }
    const char *port = argv[1], *out = argv[2], *model = argv[5], *cwd = argv[7];
    int cols = atoi(argv[3]), rows = atoi(argv[4]);
    double secs = atof(argv[6]);
    const char *bin = argc > 8 ? argv[8] : "./corbie";

    char *abs = realpath(bin, NULL);
    if (!abs) { perror(bin); return 1; }

    char home[] = "/tmp/corbie-shot-XXXXXX";
    if (!mkdtemp(home)) { perror("mkdtemp"); return 1; }
    char xdg[512], host[128];
    snprintf(xdg, sizeof xdg, "%s/cfg", home);
    snprintf(host, sizeof host, "127.0.0.1:%s", port);

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return 1; }
    if (pid == 0) {
        setenv("XDG_CONFIG_HOME", xdg, 1);
        setenv("HOME", home, 1);
        setenv("OLLAMA_HOST", host, 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        if (chdir(cwd) != 0) _exit(127);
        execl(abs, abs, "--ui", "notcurses", "-m", model, (char *)NULL);
        _exit(127);
    }

    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = malloc(cap);
    int said_da = 0, said_cpr = 0;
    double t0 = now();
    while (now() - t0 < secs) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { 0, 50000 };
        if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
        unsigned char chunk[8192];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        if (len + (size_t)n > cap) { cap = (len + (size_t)n) * 2; buf = realloc(buf, cap); }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
        if (!said_da && has(chunk, (size_t)n, "\x1b[c")) {
            if (write(fd, "\x1b[?62;22c", 9) < 0) break;
            said_da = 1;
        }
        if (!said_cpr && has(chunk, (size_t)n, "\x1b[6n")) {
            if (write(fd, "\x1b[1;1R", 6) < 0) break;
            said_cpr = 1;
        }
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    free(abs);
    nftw(home, rm_entry, 16, FTW_DEPTH | FTW_PHYS);   /* the scratch config the program wrote */

    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    printf("%zu bytes -> %s\n", len, out);
    return 0;
}
