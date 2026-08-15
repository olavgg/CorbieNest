/* Minimal HTTP/1.1 client over POSIX sockets: enough for Ollama's local API.
 * Supports chunked and content-length bodies, streamed line delivery, and
 * an interrupt fd (Ctrl-C) checked while waiting. */
#define _GNU_SOURCE
#include "common.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int http_interrupt_fd = -1;
int (*http_interrupt_check)(void) = NULL;
http_idle_cb http_idle = NULL;
void *http_idle_ud = NULL;

typedef struct {
    http_line_cb cb;
    void *ud;
    sbuf *out;
    sbuf linebuf;
    int abort;
} body_sink;

static void sink_deliver(body_sink *s, const char *p, size_t n) {
    if (s->abort) return;
    if (!s->cb) { if (s->out) sb_append(s->out, p, n); return; }
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') {
            if (s->cb(s->linebuf.data ? s->linebuf.data : "", s->linebuf.len, s->ud)) { s->abort = 1; return; }
            sb_clear(&s->linebuf);
        } else sb_putc(&s->linebuf, p[i]);
    }
}
static void sink_finish(body_sink *s) {
    if (!s->abort && s->cb && s->linebuf.len) {
        s->cb(s->linebuf.data, s->linebuf.len, s->ud);
        sb_clear(&s->linebuf);
    }
}

static int parse_url(const char *url, char *host, size_t hl, char *port, size_t pl) {
    const char *p = url;
    if (!strncasecmp(p, "http://", 7)) p += 7;
    else if (!strncasecmp(p, "https://", 8)) return -1;   /* not supported */
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    char hp[256];
    if (n >= sizeof hp) return -1;
    memcpy(hp, p, n); hp[n] = 0;
    char *colon = strrchr(hp, ':');
    if (colon && !strchr(colon, ']')) {   /* host:port */
        *colon = 0;
        snprintf(port, pl, "%s", colon + 1);
    } else snprintf(port, pl, "11434");
    /* strip IPv6 brackets */
    if (hp[0] == '[' && hp[strlen(hp)-1] == ']') { hp[strlen(hp)-1] = 0; snprintf(host, hl, "%s", hp + 1); }
    else snprintf(host, hl, "%s", hp);
    if (!host[0]) snprintf(host, hl, "127.0.0.1");
    return 0;
}

static int connect_timeout(const char *host, const char *port, int timeout_ms, char *err, size_t errlen) {
    struct addrinfo hints = {0}, *res = NULL, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) { snprintf(err, errlen, "resolve %s: %s", host, gai_strerror(rc)); return -1; }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            /* wait in 100ms slices so the idle callback keeps the spinner alive */
            int left = timeout_ms;
            for (;;) {
                fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
                struct timeval tv = { 0, (left > 100 ? 100 : left) * 1000 };
                rc = select(fd + 1, NULL, &wf, NULL, &tv);
                if (rc < 0 && errno == EINTR) continue;
                if (rc != 0) break;
                left -= 100;
                if (left <= 0) break;
                if (http_idle) http_idle(http_idle_ud);
            }
            if (rc > 0) {
                int soerr = 0; socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) rc = 0; else { errno = soerr; rc = -1; }
            } else if (rc == 0) { errno = ETIMEDOUT; rc = -1; }
        }
        if (rc == 0) { fcntl(fd, F_SETFL, fl); break; }
        snprintf(err, errlen, "connect %s:%s: %s", host, port, strerror(errno));
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && !err[0]) snprintf(err, errlen, "connect %s:%s failed", host, port);
    return fd;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* Wait for socket readable; also watch interrupt fd. Returns 1 data, 0 interrupted, -1 error/timeout */
static int wait_readable(int fd, int idle_ms) {
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        int mx = fd;
        if (http_interrupt_fd >= 0) { FD_SET(http_interrupt_fd, &rf); if (http_interrupt_fd > mx) mx = http_interrupt_fd; }
        struct timeval tv = { 0, 100 * 1000 };
        int rc = select(mx + 1, &rf, NULL, NULL, &tv);
        if (rc < 0) { if (errno == EINTR) continue; return -1; }
        if (rc == 0) {
            if (http_idle) http_idle(http_idle_ud);
            idle_ms -= 100;
            if (idle_ms <= 0) { errno = ETIMEDOUT; return -1; }
            continue;
        }
        if (http_interrupt_fd >= 0 && FD_ISSET(http_interrupt_fd, &rf)) {
            if (http_interrupt_check) { if (http_interrupt_check()) return 0; }
            else { unsigned char kb[64]; ssize_t k = read(http_interrupt_fd, kb, sizeof kb); for (ssize_t i = 0; i < k; i++) if (kb[i] == 3) return 0; }
            if (!FD_ISSET(fd, &rf)) continue;
        }
        if (FD_ISSET(fd, &rf)) return 1;
    }
}

typedef struct {
    body_sink *sink;
    bool chunked, done;
    long content_len, body_got;
    long chunk_left;
    int chunk_state;   /* 0 size line, 1 data, 2 trailing CRLF */
    sbuf cline;
} body_parser;

static void body_feed(body_parser *b, const char *p, size_t n) {
    if (!b->chunked) {
        sink_deliver(b->sink, p, n);
        b->body_got += (long)n;
        if (b->content_len >= 0 && b->body_got >= b->content_len) b->done = true;
        return;
    }
    while (n && !b->done && !b->sink->abort) {
        if (b->chunk_state == 0) {
            while (n) { char c = *p++; n--; sb_putc(&b->cline, c); if (c == '\n') break; }
            if (b->cline.len && b->cline.data[b->cline.len - 1] == '\n') {
                b->chunk_left = strtol(b->cline.data, NULL, 16);
                sb_clear(&b->cline);
                if (b->chunk_left == 0) b->done = true; else b->chunk_state = 1;
            }
        } else if (b->chunk_state == 1) {
            size_t take = n < (size_t)b->chunk_left ? n : (size_t)b->chunk_left;
            sink_deliver(b->sink, p, take);
            p += take; n -= take; b->chunk_left -= (long)take;
            if (b->chunk_left == 0) b->chunk_state = 2;
        } else {
            while (n && (*p == '\r' || *p == '\n')) {
                bool nl = (*p == '\n'); p++; n--;
                if (nl) { b->chunk_state = 0; break; }
            }
        }
    }
}

int http_request(const char *base_url, const char *method, const char *path,
                 const char *body, sbuf *out, http_line_cb line_cb, void *ud,
                 http_result *res) {
    memset(res, 0, sizeof *res);
    char host[256], port[16];
    if (parse_url(base_url, host, sizeof host, port, sizeof port) != 0) {
        snprintf(res->err, sizeof res->err, "bad host url: %s (only http:// supported)", base_url);
        return -1;
    }
    int fd = connect_timeout(host, port, 5000, res->err, sizeof res->err);
    if (fd < 0) return -1;

    sbuf req; sb_init(&req);
    size_t blen = body ? strlen(body) : 0;
    sb_printf(&req, "%s %s HTTP/1.1\r\nHost: %s:%s\r\nUser-Agent: corbienest/%s\r\nAccept: application/json\r\nConnection: close\r\n",
              method, path, host, port, CORBIE_VERSION);
    if (body) sb_printf(&req, "Content-Type: application/json\r\nContent-Length: %zu\r\n", blen);
    sb_puts(&req, "\r\n");
    if (body) sb_append(&req, body, blen);
    if (send_all(fd, req.data, req.len) != 0) {
        snprintf(res->err, sizeof res->err, "send: %s", strerror(errno));
        sb_free(&req); close(fd); return -1;
    }
    sb_free(&req);

    body_sink sink = { line_cb, ud, out, {0}, 0 };
    sb_init(&sink.linebuf);
    body_parser bp = { &sink, false, false, -1, 0, 0, 0, {0} };
    sb_init(&bp.cline);

    sbuf hdr; sb_init(&hdr);
    bool have_hdr = false;
    int rv = 0;
    char buf[16384];

    for (;;) {
        int w = wait_readable(fd, 600 * 1000);
        if (w == 0) { res->aborted = true; break; }
        if (w < 0) { snprintf(res->err, sizeof res->err, "recv: %s", strerror(errno)); rv = -1; break; }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) { if (errno == EINTR) continue; snprintf(res->err, sizeof res->err, "recv: %s", strerror(errno)); rv = -1; break; }
        if (n == 0) break;   /* EOF */
        const char *p = buf; size_t left = (size_t)n;

        if (!have_hdr) {
            sb_append(&hdr, p, left);
            char *e = strstr(hdr.data, "\r\n\r\n");
            if (!e) { if (hdr.len > 65536) { snprintf(res->err, sizeof res->err, "header too large"); rv = -1; break; } continue; }
            *e = 0;
            size_t hlen = (size_t)(e - hdr.data) + 4;
            const char *sp = strchr(hdr.data, ' ');
            res->status = sp ? atoi(sp + 1) : 0;
            for (char *l = strstr(hdr.data, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
                const char *h = l + 2;
                if (!strncasecmp(h, "transfer-encoding:", 18) && strcasestr(h, "chunked")) bp.chunked = true;
                else if (!strncasecmp(h, "content-length:", 15)) bp.content_len = atol(h + 15);
            }
            have_hdr = true;
            p = hdr.data + hlen; left = hdr.len - hlen;
        }
        body_feed(&bp, p, left);
        if (sink.abort) { res->aborted = true; break; }
        if (bp.done) break;
    }
    if (!res->aborted && rv == 0) sink_finish(&sink);
    if (!have_hdr && rv == 0 && !res->aborted) { snprintf(res->err, sizeof res->err, "empty response from server"); rv = -1; }
    close(fd);
    sb_free(&hdr); sb_free(&bp.cline); sb_free(&sink.linebuf);
    return rv;
}
