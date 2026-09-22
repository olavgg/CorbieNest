/* HTTP client over libcurl: plain http:// for Ollama, https:// for the hosted model APIs the
 * advisor may consult (and an Ollama behind TLS). Streamed line delivery, an interrupt fd
 * (Esc/Ctrl-C) watched while waiting, an idle callback that keeps the spinner alive, and a
 * timeout on *silence* rather than on the whole request: a model may think for many minutes, but
 * a server that has sent nothing at all for ten is gone. */
#define _GNU_SOURCE
#include "common.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

int http_interrupt_fd = -1;
int http_idle_timeout_ms = 600 * 1000;
int (*http_interrupt_check)(void) = NULL;
http_idle_cb http_idle = NULL;
void *http_idle_ud = NULL;
const char *const *http_headers = NULL;

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

/* base + path as one URL. The base is what the user gave as the host, so it may lack the scheme
 * ("localhost:11434", "0.0.0.0") and, the way OLLAMA_HOST is written, the port: plain http
 * without one means Ollama's 11434, not 80. An https base keeps its own default (443). A base
 * path ("https://api.x.ai/v1") stays in front of `path`. */
char *http_url(const char *base, const char *path) {
    if (!base) base = "";
    while (*base == ' ') base++;
    const char *sep = strstr(base, "://");
    const char *scheme = sep ? base : "http";
    size_t scheme_len = sep ? (size_t)(sep - base) : 4;
    const char *auth = sep ? sep + 3 : base;
    size_t auth_len = strcspn(auth, "/?#");
    const char *rest = auth + auth_len;
    size_t rest_len = strlen(rest);
    while (rest_len && rest[rest_len - 1] == '/') rest_len--;
    bool has_port = auth[0] == '['
        ? (memchr(auth, ']', auth_len) && ((const char *)memchr(auth, ']', auth_len))[1] == ':')
        : memchr(auth, ':', auth_len) != NULL;
    bool http = scheme_len == 4 && !strncasecmp(scheme, "http", 4);
    sbuf u; sb_init(&u);
    sb_append(&u, scheme, scheme_len); sb_puts(&u, "://");
    if (auth_len == 0 || auth[0] == ':') sb_puts(&u, "127.0.0.1");   /* ":11434" = this machine */
    sb_append(&u, auth, auth_len);
    if (http && !has_port) sb_puts(&u, ":11434");
    sb_append(&u, rest, rest_len);
    if (path && *path && *path != '/') sb_putc(&u, '/');
    if (path) sb_puts(&u, path);
    return sb_detach(&u);
}

static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
    body_sink sink;
    long long last_rx;   /* when the server last sent anything: headers count */
} xfer;

static size_t on_body(char *p, size_t size, size_t n, void *ud) {
    xfer *x = ud; size_t len = size * n;
    x->last_rx = now_ms();
    sink_deliver(&x->sink, p, len);
    return x->sink.abort ? 0 : len;   /* anything but len: libcurl stops the transfer */
}
static size_t on_header(char *p, size_t size, size_t n, void *ud) {
    (void)p; xfer *x = ud; x->last_rx = now_ms();
    return size * n;
}

static bool curl_ready(void) {
    static int state = 0;   /* 0 not yet, 1 ok, -1 failed */
    if (!state) state = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 1 : -1;
    return state > 0;
}

/* Ctrl-C among the keys typed, for callers that did not set http_interrupt_check */
static int ctrl_c_pending(void) {
    unsigned char kb[64]; ssize_t k = read(http_interrupt_fd, kb, sizeof kb);
    for (ssize_t i = 0; i < k; i++) if (kb[i] == 3) return 1;
    return 0;
}

int http_request(const char *base_url, const char *method, const char *path,
                 const char *body, sbuf *out, http_line_cb line_cb, void *ud,
                 http_result *res) {
    memset(res, 0, sizeof *res);
    if (!curl_ready()) { snprintf(res->err, sizeof res->err, "libcurl could not be initialised"); return -1; }
    CURL *h = curl_easy_init();
    CURLM *m = curl_multi_init();
    if (!h || !m) { if (h) curl_easy_cleanup(h); if (m) curl_multi_cleanup(m); snprintf(res->err, sizeof res->err, "libcurl could not be initialised"); return -1; }
    char *url = http_url(base_url, path);
    char errbuf[CURL_ERROR_SIZE] = "";
    xfer x; memset(&x, 0, sizeof x);
    x.sink.cb = line_cb; x.sink.ud = ud; x.sink.out = out; sb_init(&x.sink.linebuf);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: application/json");
    if (body) hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Expect:");              /* no 100-continue round trip for a big brief */
    /* a server that serves one connection at a time (a test's) is not held up; over TLS the
     * connection may be HTTP/2, where the header is not allowed (FORBID_REUSE closes it anyway) */
    if (!strncasecmp(url, "http://", 7)) hdrs = curl_slist_append(hdrs, "Connection: close");
    for (const char *const *e = http_headers; e && *e; e++) hdrs = curl_slist_append(hdrs, *e);

    char ua[64]; snprintf(ua, sizeof ua, "corbienest/%s", CORBIE_VERSION);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);          /* a long silent wait (an answer that is not streamed) through a NAT */
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &x);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    /* the proxy variables are honoured, but never for this machine: a local Ollama is not
     * something to send through a corporate proxy */
    const char *np = getenv("no_proxy"); if (!np || !*np) np = getenv("NO_PROXY");
    char noproxy[1024]; snprintf(noproxy, sizeof noproxy, "%s%slocalhost,127.0.0.1,::1", np && *np ? np : "", np && *np ? "," : "");
    curl_easy_setopt(h, CURLOPT_NOPROXY, noproxy);
    /* where the trusted certificates are, the way the curl program is told (a private CA, a test's own) */
    const char *ca = getenv("CURL_CA_BUNDLE"); if (!ca || !*ca) ca = getenv("SSL_CERT_FILE");
    if (ca && *ca) curl_easy_setopt(h, CURLOPT_CAINFO, ca);
    if (body || !strcmp(method, "POST")) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
    }
    if (strcmp(method, "GET") && strcmp(method, "POST")) curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
    else if (!strcmp(method, "GET") && !body) curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);

    curl_multi_add_handle(m, h);
    long long t0 = now_ms(), last_tick = t0;
    x.last_rx = t0;
    bool interrupted = false, silent = false;
    int running = 1, rv = 0;
    CURLcode cc = CURLE_OK;
    while (running) {
        CURLMcode mc = curl_multi_perform(m, &running);
        if (mc != CURLM_OK) { snprintf(res->err, sizeof res->err, "libcurl: %s", curl_multi_strerror(mc)); rv = -1; break; }
        if (!running || x.sink.abort) break;
        struct curl_waitfd w = { http_interrupt_fd, CURL_WAIT_POLLIN, 0 };
        bool watch = http_interrupt_fd >= 0;
        mc = curl_multi_poll(m, watch ? &w : NULL, watch ? 1u : 0u, 100, NULL);
        if (mc != CURLM_OK) { snprintf(res->err, sizeof res->err, "libcurl: %s", curl_multi_strerror(mc)); rv = -1; break; }
        if (watch && (w.revents & CURL_WAIT_POLLIN) && (http_interrupt_check ? http_interrupt_check() : ctrl_c_pending())) { interrupted = true; break; }
        long long t = now_ms();
        /* the idle callback runs when the server has been quiet for 100 ms, as it always has:
         * while text streams in, the spinner is not drawn over it */
        if (t - (x.last_rx > last_tick ? x.last_rx : last_tick) >= 100) { if (http_idle) http_idle(http_idle_ud); last_tick = t; }
        if (t - x.last_rx >= http_idle_timeout_ms) { silent = true; break; }
    }
    if (!running && rv == 0) {
        int left; CURLMsg *msg;
        while ((msg = curl_multi_info_read(m, &left))) if (msg->msg == CURLMSG_DONE && msg->easy_handle == h) cc = msg->data.result;
    }
    long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    res->status = (int)code;
    if (rv != 0) {}
    else if (interrupted || x.sink.abort) res->aborted = true;
    else if (silent) { snprintf(res->err, sizeof res->err, "recv: timed out — the server sent nothing for %d s", http_idle_timeout_ms / 1000); rv = -1; }
    else if (cc != CURLE_OK) {
        const char *why = errbuf[0] ? errbuf : curl_easy_strerror(cc);
        if (cc == CURLE_GOT_NOTHING) snprintf(res->err, sizeof res->err, "empty response from server");
        else if (cc == CURLE_COULDNT_CONNECT || cc == CURLE_COULDNT_RESOLVE_HOST || cc == CURLE_COULDNT_RESOLVE_PROXY || (cc == CURLE_OPERATION_TIMEDOUT && !code))
            snprintf(res->err, sizeof res->err, "%s%s", strcasestr(why, "connect") ? "" : "cannot connect: ", why);
        else if (cc == CURLE_PEER_FAILED_VERIFICATION || cc == CURLE_SSL_CONNECT_ERROR || cc == CURLE_SSL_CACERT_BADFILE || cc == CURLE_SSL_CERTPROBLEM)
            snprintf(res->err, sizeof res->err, "TLS: %s", why);
        else snprintf(res->err, sizeof res->err, "%s", why);
        rv = -1;
    }
    if (rv == 0 && !res->aborted) sink_finish(&x.sink);
    curl_multi_remove_handle(m, h);
    curl_easy_cleanup(h); curl_multi_cleanup(m);
    curl_slist_free_all(hdrs);
    free(url); sb_free(&x.sink.linebuf);
    return rv;
}
