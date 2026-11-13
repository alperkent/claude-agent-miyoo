/*
 * Claude Agent: connection to the relay (stage 2).
 *
 *   claude-agent connect [--pair]   connect and wait for Claude (Ctrl+C or SIGTERM stops)
 *   claude-agent unpair             disconnect every Claude client paired with this device
 *   claude-agent server             print the relay address
 *
 * Signals (sent by the app screen): SIGUSR1 = show a new pairing code,
 * SIGUSR2 = disconnect every Claude client.
 *
 * Files in the app folder:
 *   config/server.txt     relay address (default below), e.g. https://claude-miyoo.example.workers.dev
 *   config/identity.json  device id and secret, created on first connection
 *   cacert.pem            trusted certificate authorities (Mozilla list)
 *   status.txt            state for the app screen: state=, code=, client=
 *
 * One network thread does all socket and TLS work. Tool calls run in their
 * own threads and hand their results back through a queue, because an
 * mbedTLS context must not be used by two threads at once.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "cJSON.h"
#include "common.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha1.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"

#ifndef DEFAULT_SERVER
/* no built-in relay: set one with server.txt at build time or config/server.txt on the SD card */
#define DEFAULT_SERVER "https://relay-not-set.invalid"
#endif

extern char g_home[];
extern cJSON *agent_call(const char *name, cJSON *args);
extern cJSON *tools_list(void);
extern cJSON *device_summary(void);

#define MAX_MSG (8 * 1024 * 1024)
#define PING_EVERY_MS 25000
#define DEAD_AFTER_MS 90000
#define MAX_WORKERS 4

int g_is_net = 0; /* this process holds the connection (plugins ask it to resend the tools) */
static volatile sig_atomic_t g_stop = 0, g_want_code = 0, g_want_unpair = 0, g_want_tools = 0;

/* ------------------------------------------------------------- logging */

static void nlog(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    printf("%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fflush(stdout);
    char path[1024];
    snprintf(path, sizeof(path), "%s/logs/connection.log", g_home);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d UTC %s\n", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, line);
        fclose(f);
    }
}

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* status.txt is read by the app screen */
static char st_state[32] = "starting", st_code[16] = "", st_client[64] = "", st_last[64] = "";
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;

static void status_write(void)
{
    pthread_mutex_lock(&st_lock);
    char path[1024], tmp[1100];
    snprintf(path, sizeof(path), "%s/status.txt", g_home);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "state=%s\ncode=%s\nclient=%s\nlast=%s\n", st_state, st_code, st_client, st_last);
        fclose(f);
        rename(tmp, path);
    }
    pthread_mutex_unlock(&st_lock);
}

/* config/paired: this device was paired at least once (no code needed at start) */
static void paired_flag(int on)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/config/paired", g_home);
    if (on) {
        FILE *f = fopen(path, "w");
        if (f)
            fclose(f);
    }
    else
        unlink(path);
}

static void set_state(const char *s)
{
    if (strcmp(st_state, s) != 0) {
        snprintf(st_state, sizeof(st_state), "%s", s);
        status_write();
    }
}

/* ---------------------------------------------------------- files/config */

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (s && fread(s, 1, n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    if (s)
        s[n] = 0;
    fclose(f);
    return s;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '/'))
        s[--n] = 0;
}

typedef struct {
    int tls;
    char host[256];
    char port[8];
    char url[512];
} server_t;

/* "https://host[:port]/path" -> sv (and path, if wanted) */
static int url_parse(const char *url, server_t *sv, char *path, size_t psize)
{
    memset(sv, 0, sizeof(*sv));
    snprintf(sv->url, sizeof(sv->url), "%s", url);
    const char *p = url;
    if (!strncmp(p, "https://", 8)) {
        sv->tls = 1;
        p += 8;
        strcpy(sv->port, "443");
    }
    else if (!strncmp(p, "http://", 7)) {
        sv->tls = 0;
        p += 7;
        strcpy(sv->port, "80");
    }
    else
        return -1;
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && i < sizeof(sv->host) - 1)
        sv->host[i++] = *p++;
    sv->host[i] = 0;
    if (*p == ':') {
        p++;
        i = 0;
        while (*p >= '0' && *p <= '9' && i < sizeof(sv->port) - 1)
            sv->port[i++] = *p++;
        sv->port[i] = 0;
    }
    if (path)
        snprintf(path, psize, "%s%s", *p == '/' ? "" : "/", p);
    return sv->host[0] ? 0 : -1;
}

static int server_load(server_t *sv)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/config/server.txt", g_home);
    char *s = slurp(path);
    char url[512];
    snprintf(url, sizeof(url), "%s", s ? s : DEFAULT_SERVER);
    free(s);
    trim(url);
    return url_parse(url, sv, NULL, 0);
}

typedef struct {
    char id[64];
    char secret[128];
    char server[512];
} identity_t;

static int identity_load(identity_t *idn)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/config/identity.json", g_home);
    char *s = slurp(path);
    if (!s)
        return -1;
    cJSON *j = cJSON_Parse(s);
    free(s);
    if (!j)
        return -1;
    cJSON *a = cJSON_GetObjectItem(j, "device_id"), *b = cJSON_GetObjectItem(j, "device_secret"),
          *c = cJSON_GetObjectItem(j, "server");
    int ok = cJSON_IsString(a) && cJSON_IsString(b) && cJSON_IsString(c);
    if (ok) {
        snprintf(idn->id, sizeof(idn->id), "%s", a->valuestring);
        snprintf(idn->secret, sizeof(idn->secret), "%s", b->valuestring);
        snprintf(idn->server, sizeof(idn->server), "%s", c->valuestring);
    }
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

static int identity_save(const identity_t *idn)
{
    char dir[1024], path[1100], tmp[1200];
    snprintf(dir, sizeof(dir), "%s/config", g_home);
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/identity.json", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "server", idn->server);
    cJSON_AddStringToObject(j, "device_id", idn->id);
    cJSON_AddStringToObject(j, "device_secret", idn->secret);
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        free(s);
        return -1;
    }
    ssize_t w = write(fd, s, strlen(s));
    fsync(fd);
    close(fd);
    free(s);
    if (w <= 0)
        return -1;
    return rename(tmp, path);
}

/* --------------------------------------------------------------- clock */

/* TLS certificates are checked against the clock. Many Miyoos have no
   hardware clock and boot with an old date, so ask an NTP server first. */
static void sync_clock(void)
{
    const char *servers[] = {"pool.ntp.org", "time.cloudflare.com", "time.google.com", NULL};
    for (int s = 0; servers[s]; s++) {
        struct addrinfo hints = {0}, *ai = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(servers[s], "123", &hints, &ai) != 0 || !ai)
            continue;
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        unsigned char pkt[48] = {0x1b};
        struct timeval tv = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        long long t0 = mono_ms();
        int ok = sendto(fd, pkt, 48, 0, ai->ai_addr, ai->ai_addrlen) == 48 &&
                 recv(fd, pkt, 48, 0) == 48;
        long long rtt = mono_ms() - t0;
        close(fd);
        freeaddrinfo(ai);
        if (!ok)
            continue;
        unsigned long secs = ((unsigned long)pkt[40] << 24) | (pkt[41] << 16) | (pkt[42] << 8) | pkt[43];
        unsigned long frac = ((unsigned long)pkt[44] << 24) | (pkt[45] << 16) | (pkt[46] << 8) | pkt[47];
        if (secs == 0)
            continue;
        double ntp = (double)(secs - 2208988800UL) + frac / 4294967296.0 + rtt / 2000.0;
        struct timeval now;
        gettimeofday(&now, NULL);
        double diff = ntp - (now.tv_sec + now.tv_usec / 1e6);
        if (diff > 60 || diff < -60) {
            struct timeval nt = {(time_t)ntp, (suseconds_t)((ntp - (time_t)ntp) * 1e6)};
            if (settimeofday(&nt, NULL) == 0)
                nlog("clock corrected by %.0f s (NTP %s)", diff, servers[s]);
            else
                nlog("clock is %.0f s off, but it could not be set: %s", diff, strerror(errno));
        }
        return;
    }
    nlog("NTP not reachable; keeping the device clock");
}

/* ---------------------------------------------------------- connection */

typedef struct {
    int fd;
    int tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca; /* only during the handshake: the list takes ~350 KB of memory */
} conn_t;

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_tls_ready = 0;

static int tls_global_init(void)
{
    if (g_tls_ready)
        return 0;
    psa_crypto_init();
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)"claude-agent", 12) != 0) {
        nlog("random generator failed");
        return -1;
    }
    g_tls_ready = 1;
    return 0;
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *ai = NULL, *p;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, port, &hints, &ai);
    if (e != 0) {
        nlog("cannot find %s: %s", host, gai_strerror(e));
        return -1;
    }
    int fd = -1;
    for (p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        struct timeval tv = {15, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0)
        nlog("cannot connect to %s:%s: %s", host, port, strerror(errno));
    else {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    }
    return fd;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                                            : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_READ
                                                                            : MBEDTLS_ERR_NET_RECV_FAILED;
    if (n == 0)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return (int)n;
}

static void conn_close(conn_t *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
    }
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static int conn_open(conn_t *c, const server_t *sv)
{
    memset(c, 0, sizeof(*c));
    c->fd = tcp_connect(sv->host, sv->port);
    if (c->fd < 0)
        return -1;
    c->tls = sv->tls;
    if (!c->tls)
        return 0;
    if (tls_global_init() != 0) {
        close(c->fd);
        return -1;
    }
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    char path[1024];
    snprintf(path, sizeof(path), "%s/cacert.pem", g_home);
    if (mbedtls_x509_crt_parse_file(&c->ca, path) < 0) {
        nlog("cannot read %s (certificate list)", path);
        conn_close(c);
        return -1;
    }
    mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_setup(&c->ssl, &c->conf);
    mbedtls_ssl_set_hostname(&c->ssl, sv->host);
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        char err[200];
        mbedtls_strerror(r, err, sizeof(err));
        unsigned flags = mbedtls_ssl_get_verify_result(&c->ssl);
        char vinfo[300] = "";
        if (flags && flags != (unsigned)-1)
            mbedtls_x509_crt_verify_info(vinfo, sizeof(vinfo), "", flags);
        trim(vinfo);
        nlog("secure connection failed: %s %s", err, vinfo);
        conn_close(c);
        return -1;
    }
    /* the certificate list is only needed to check the server: give the memory back */
    mbedtls_ssl_conf_ca_chain(&c->conf, NULL, NULL);
    mbedtls_x509_crt_free(&c->ca);
    malloc_trim(0);
    return 0;
}

/* writes everything (blocking style; the socket may be non-blocking) */
static int conn_write(conn_t *c, const unsigned char *buf, size_t len)
{
    size_t done = 0;
    long long until = mono_ms() + 30000;
    while (done < len) {
        int n;
        if (c->tls)
            n = mbedtls_ssl_write(&c->ssl, buf + done, len - done);
        else {
            n = (int)send(c->fd, buf + done, len - done, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                n = MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            if (mono_ms() > until)
                return -1;
            struct pollfd p = {c->fd, n == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, 0};
            poll(&p, 1, 1000);
            continue;
        }
        if (n <= 0)
            return -1;
        done += n;
    }
    return 0;
}

/* reads what is available; returns bytes, 0 if nothing yet, -1 on close/error */
static int conn_read(conn_t *c, unsigned char *buf, size_t len)
{
    int n;
    if (c->tls) {
        n = mbedtls_ssl_read(&c->ssl, buf, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            return 0;
#endif
        if (n <= 0)
            return -1;
        return n;
    }
    n = (int)recv(c->fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    return n <= 0 ? -1 : n;
}

static int conn_pending(conn_t *c)
{
    return c->tls ? (int)mbedtls_ssl_get_bytes_avail(&c->ssl) : 0;
}

/* reads an HTTP response head (blocking); returns status code and leftover body */
static int http_head(conn_t *c, char *head, size_t hsize, size_t *hlen)
{
    size_t n = 0;
    long long until = mono_ms() + 20000;
    while (n < hsize - 1) {
        int r = conn_read(c, (unsigned char *)head + n, hsize - 1 - n);
        if (r < 0)
            return -1;
        if (r == 0) {
            if (mono_ms() > until)
                return -1;
            struct pollfd p = {c->fd, POLLIN, 0};
            poll(&p, 1, 500);
            continue;
        }
        n += r;
        head[n] = 0;
        if (strstr(head, "\r\n\r\n"))
            break;
    }
    *hlen = n;
    int status = 0;
    if (sscanf(head, "HTTP/%*s %d", &status) != 1)
        return -1;
    return status;
}

/* --------------------------------------------- HTTPS downloads (download_file) */

/* value of a response header (case-insensitive name), or "" */
static void header_get(const char *head, const char *name, char *out, size_t size)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            l += nl + 1;
            while (*l == ' ' || *l == '\t')
                l++;
            size_t n = 0;
            while (l[n] && l[n] != '\r' && n < size - 1) {
                out[n] = l[n];
                n++;
            }
            out[n] = 0;
            return;
        }
    }
}

/* waits for data; 0 = more data, -1 = closed/error, -2 = stalled too long or cancelled */
static int dl_read(conn_t *c, unsigned char *buf, size_t len, volatile int *cancel)
{
    long long until = mono_ms() + 45000;
    for (;;) {
        if (cancel && *cancel)
            return -2;
        int r = conn_read(c, buf, len);
        if (r != 0)
            return r;
        if (conn_pending(c))
            continue;
        if (mono_ms() > until)
            return -2;
        struct pollfd p = {c->fd, POLLIN, 0};
        poll(&p, 1, 500);
    }
}

int https_get(const char *url_in, https_sink_fn sink, void *ctx, long long *total, volatile int *cancel,
              char *err, size_t errsize)
{
    char url[2048];
    snprintf(url, sizeof(url), "%s", url_in);
    *total = -1;
    err[0] = 0;
    if (tls_global_init() != 0) {
        snprintf(err, errsize, "secure connections are not available (cacert.pem missing?)");
        return -1;
    }
    for (int hop = 0; hop < 8; hop++) {
        server_t sv;
        char path[2048];
        if (url_parse(url, &sv, path, sizeof(path)) != 0) {
            snprintf(err, errsize, "not a web address: %.200s", url);
            return -1;
        }
        conn_t c;
        if (conn_open(&c, &sv) != 0) {
            snprintf(err, errsize, "cannot connect to %s (no internet, or the site refused a secure connection)",
                     sv.host);
            return -1;
        }
        char req[2600];
        int rl = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: claude-agent-miyoo\r\nAccept: */*\r\n"
                          "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                          path, sv.host);
        if (rl >= (int)sizeof(req) || conn_write(&c, (unsigned char *)req, rl) != 0) {
            conn_close(&c);
            snprintf(err, errsize, "could not send the request to %s", sv.host);
            return -1;
        }
        static __thread char head[32768];
        size_t hlen = 0;
        int status = http_head(&c, head, sizeof(head), &hlen);
        if (status < 0) {
            conn_close(&c);
            snprintf(err, errsize, "no answer from %s", sv.host);
            return -1;
        }
        char *body = strstr(head, "\r\n\r\n");
        if (!body) {
            conn_close(&c);
            snprintf(err, errsize, "answer headers too long from %s", sv.host);
            return -1;
        }
        body[2] = 0; /* headers end with one \r\n, for header_get */
        body += 4;
        size_t have = hlen - (size_t)(body - head);

        if (status >= 300 && status < 400) {
            char loc[2048];
            header_get(head, "Location", loc, sizeof(loc));
            conn_close(&c);
            if (!loc[0]) {
                snprintf(err, errsize, "redirect without an address (HTTP %d)", status);
                return -1;
            }
            if (loc[0] == '/') { /* same site */
                char base[600];
                snprintf(base, sizeof(base), "%s://%s%s%s", sv.tls ? "https" : "http", sv.host,
                         (!strcmp(sv.port, sv.tls ? "443" : "80")) ? "" : ":", (!strcmp(sv.port, sv.tls ? "443" : "80")) ? "" : sv.port);
                snprintf(url, sizeof(url), "%s%s", base, loc);
            }
            else
                snprintf(url, sizeof(url), "%s", loc);
            continue;
        }
        if (status != 200) {
            conn_close(&c);
            snprintf(err, errsize, "the site answered HTTP %d%s", status,
                     status == 404 ? " (not found: check the address)" : "");
            return -1;
        }

        char v[64];
        header_get(head, "Content-Length", v, sizeof(v));
        long long left = v[0] ? atoll(v) : -1;
        *total = left;
        header_get(head, "Transfer-Encoding", v, sizeof(v));
        int chunked = strcasestr(v, "chunked") != NULL;

        static __thread unsigned char buf[65536];
        int rc = 0;
        if (!chunked) {
            if (have && sink(ctx, (unsigned char *)body, have) != 0)
                rc = -3;
            if (left >= 0)
                left -= have;
            while (!rc && left != 0) {
                int n = dl_read(&c, buf, left > 0 && left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf), cancel);
                if (n == -1) {
                    if (left > 0)
                        rc = -1;
                    break;
                }
                if (n == -2) {
                    rc = -2;
                    break;
                }
                if (sink(ctx, buf, n) != 0)
                    rc = -3;
                if (left > 0)
                    left -= n;
            }
        }
        else {
            /* chunked: "<hex size>\r\n<data>\r\n" ... "0\r\n\r\n" */
            static __thread unsigned char cb[65536];
            size_t cl = have < sizeof(cb) ? have : 0;
            memcpy(cb, body, cl);
            int state = 0; /* 0: size line, 1: data, 2: \r\n after the data */
            long long left_in_chunk = 0;
            for (;;) {
                unsigned char *nl = state == 0 ? memmem(cb, cl, "\r\n", 2) : NULL;
                if (cl == 0 || (state == 0 && !nl)) {
                    if (cl >= sizeof(cb)) {
                        rc = -1;
                        break;
                    }
                    int n = dl_read(&c, cb + cl, sizeof(cb) - cl, cancel);
                    if (n < 0) {
                        rc = n == -2 ? -2 : -1;
                        break;
                    }
                    cl += n;
                    continue;
                }
                size_t used;
                if (state == 0) {
                    left_in_chunk = strtoll((char *)cb, NULL, 16);
                    used = (size_t)(nl + 2 - cb);
                    state = left_in_chunk > 0 ? 1 : 3;
                }
                else if (state == 1) {
                    used = (long long)cl < left_in_chunk ? cl : (size_t)left_in_chunk;
                    if (sink(ctx, cb, used) != 0) {
                        rc = -3;
                        break;
                    }
                    left_in_chunk -= used;
                    if (left_in_chunk == 0) {
                        state = 2;
                        left_in_chunk = 2;
                    }
                }
                else {
                    used = (long long)cl < left_in_chunk ? cl : (size_t)left_in_chunk;
                    left_in_chunk -= used;
                    if (left_in_chunk == 0)
                        state = 0;
                }
                memmove(cb, cb + used, cl - used);
                cl -= used;
                if (state == 3)
                    break; /* last chunk */
            }
        }
        conn_close(&c);
        if (rc == -1)
            snprintf(err, errsize, "the connection was cut before the end of the file");
        else if (rc == -2)
            snprintf(err, errsize, (cancel && *cancel) ? "cancelled" : "the download stopped (no data for 45 s)");
        else if (rc == -3 && !err[0])
            snprintf(err, errsize, "could not save the file");
        return rc ? -1 : 0;
    }
    snprintf(err, errsize, "too many redirects");
    return -1;
}

/* ------------------------------------------------------- registration */

static int do_register(const server_t *sv, identity_t *idn)
{
    conn_t c;
    if (conn_open(&c, sv) != 0)
        return -1;
    char req[600];
    int rl = snprintf(req, sizeof(req),
                      "POST /api/device/register HTTP/1.1\r\nHost: %s\r\nContent-Length: 0\r\n"
                      "User-Agent: claude-agent-miyoo\r\nConnection: close\r\n\r\n",
                      sv->host);
    if (conn_write(&c, (unsigned char *)req, rl) != 0) {
        conn_close(&c);
        return -1;
    }
    /* read the whole answer until the server closes */
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    long long until = mono_ms() + 20000;
    for (;;) {
        if (len + 1024 > cap)
            buf = realloc(buf, cap *= 2);
        int r = conn_read(&c, (unsigned char *)buf + len, cap - len - 1);
        if (r < 0)
            break;
        if (r == 0) {
            if (mono_ms() > until)
                break;
            struct pollfd p = {c.fd, POLLIN, 0};
            poll(&p, 1, 500);
            continue;
        }
        len += r;
    }
    buf[len] = 0;
    conn_close(&c);
    int status = 0;
    sscanf(buf, "HTTP/%*s %d", &status);
    char *body = strstr(buf, "\r\n\r\n");
    char *js = body ? strchr(body, '{') : NULL; /* also skips a chunk size line */
    cJSON *j = js ? cJSON_Parse(js) : NULL;
    int ok = 0;
    if (status == 200 && j) {
        cJSON *a = cJSON_GetObjectItem(j, "device_id"), *b = cJSON_GetObjectItem(j, "device_secret");
        if (cJSON_IsString(a) && cJSON_IsString(b)) {
            snprintf(idn->id, sizeof(idn->id), "%s", a->valuestring);
            snprintf(idn->secret, sizeof(idn->secret), "%s", b->valuestring);
            snprintf(idn->server, sizeof(idn->server), "%s", sv->url);
            ok = identity_save(idn) == 0;
        }
    }
    if (!ok)
        nlog("registration failed (HTTP %d)", status);
    else
        nlog("registered this device on %s", sv->url);
    cJSON_Delete(j);
    free(buf);
    return ok ? 0 : -1;
}

/* ----------------------------------------------------------- websocket */

static int ws_send_frame(conn_t *c, int opcode, const unsigned char *data, size_t len)
{
    size_t hdr = 2 + (len >= 126 ? (len > 65535 ? 8 : 2) : 0) + 4;
    unsigned char *f = malloc(hdr + len);
    if (!f)
        return -1;
    size_t i = 0;
    f[i++] = 0x80 | (opcode & 15);
    if (len < 126)
        f[i++] = 0x80 | len;
    else if (len <= 65535) {
        f[i++] = 0x80 | 126;
        f[i++] = len >> 8;
        f[i++] = len;
    }
    else {
        f[i++] = 0x80 | 127;
        for (int k = 7; k >= 0; k--)
            f[i++] = (unsigned char)((unsigned long long)len >> (8 * k));
    }
    unsigned char mask[4];
    mbedtls_ctr_drbg_random(&g_drbg, mask, 4);
    memcpy(f + i, mask, 4);
    i += 4;
    for (size_t k = 0; k < len; k++)
        f[i + k] = data[k] ^ mask[k & 3];
    int r = conn_write(c, f, i + len);
    free(f);
    return r;
}

static int ws_handshake(conn_t *c, const server_t *sv, const identity_t *idn)
{
    unsigned char raw[16], key[32], acc[64];
    size_t klen = 0, alen = 0;
    mbedtls_ctr_drbg_random(&g_drbg, raw, 16);
    mbedtls_base64_encode(key, sizeof(key), &klen, raw, 16);
    key[klen] = 0;
    char req[1024];
    int rl = snprintf(req, sizeof(req),
                      "GET /api/device/ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
                      "X-Device-Id: %s\r\nAuthorization: Bearer %s\r\n"
                      "User-Agent: claude-agent-miyoo\r\n\r\n",
                      sv->host, key, idn->id, idn->secret);
    if (conn_write(c, (unsigned char *)req, rl) != 0)
        return -1;
    char head[4096];
    size_t hlen;
    int status = http_head(c, head, sizeof(head), &hlen);
    if (status != 101) {
        nlog("the relay refused the connection (HTTP %d)", status);
        return status == 403 ? -2 : -1;
    }
    /* check Sec-WebSocket-Accept */
    char src[128];
    snprintf(src, sizeof(src), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char sha[20];
    mbedtls_sha1((unsigned char *)src, strlen(src), sha);
    mbedtls_base64_encode(acc, sizeof(acc), &alen, sha, 20);
    acc[alen] = 0;
    if (!strcasestr(head, (char *)acc)) {
        nlog("bad websocket answer from the relay");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------- outgoing queue + workers */

typedef struct outmsg {
    char *text;
    struct outmsg *next;
} outmsg;

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static outmsg *q_head = NULL, *q_tail = NULL;
static int wake_pipe[2] = {-1, -1};
static int workers_busy = 0;

static void queue_text(char *text) /* takes ownership */
{
    outmsg *m = malloc(sizeof(*m));
    m->text = text;
    m->next = NULL;
    pthread_mutex_lock(&q_lock);
    if (q_tail)
        q_tail->next = m;
    else
        q_head = m;
    q_tail = m;
    pthread_mutex_unlock(&q_lock);
    if (write(wake_pipe[1], "x", 1) < 0) {
    }
}

static void queue_json(cJSON *j)
{
    queue_text(cJSON_PrintUnformatted(j));
}

typedef struct {
    long long id;
    char *tool;
    cJSON *args;
} call_t;

static void *worker(void *arg)
{
    call_t *jb = arg;
    pthread_mutex_lock(&st_lock);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(st_last, sizeof(st_last), "%s (%02d:%02d)", jb->tool, tm.tm_hour, tm.tm_min);
    pthread_mutex_unlock(&st_lock);
    status_write();
    cJSON *res = agent_call(jb->tool, jb->args);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "result");
    cJSON_AddNumberToObject(msg, "id", (double)jb->id);
    cJSON_AddItemToObject(msg, "result", res);
    queue_json(msg);
    cJSON_Delete(msg);
    cJSON_Delete(jb->args);
    free(jb->tool);
    free(jb);
    pthread_mutex_lock(&q_lock);
    workers_busy--;
    pthread_mutex_unlock(&q_lock);
    malloc_trim(0);
    return NULL;
}

static void start_job(cJSON *msg)
{
    cJSON *id = cJSON_GetObjectItem(msg, "id"), *tool = cJSON_GetObjectItem(msg, "tool");
    if (!cJSON_IsNumber(id) || !cJSON_IsString(tool))
        return;
    pthread_mutex_lock(&q_lock);
    int busy = workers_busy;
    if (busy < MAX_WORKERS)
        workers_busy++;
    pthread_mutex_unlock(&q_lock);
    if (busy >= MAX_WORKERS) {
        cJSON *r = cJSON_CreateObject(), *content = cJSON_CreateArray(), *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "text");
        cJSON_AddStringToObject(t, "text", "The device is busy with other requests; try again in a moment.");
        cJSON_AddItemToArray(content, t);
        cJSON_AddItemToObject(r, "content", content);
        cJSON_AddBoolToObject(r, "isError", 1);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "result");
        cJSON_AddNumberToObject(m, "id", id->valuedouble);
        cJSON_AddItemToObject(m, "result", r);
        queue_json(m);
        cJSON_Delete(m);
        return;
    }
    call_t *jb = calloc(1, sizeof(*jb));
    jb->id = (long long)id->valuedouble;
    jb->tool = strdup(tool->valuestring);
    cJSON *a = cJSON_GetObjectItem(msg, "args");
    jb->args = a ? cJSON_Duplicate(a, 1) : cJSON_CreateObject();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, worker, jb) != 0) {
        cJSON *res = agent_call(jb->tool, jb->args); /* run inline */
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "result");
        cJSON_AddNumberToObject(m, "id", (double)jb->id);
        cJSON_AddItemToObject(m, "result", res);
        queue_json(m);
        cJSON_Delete(m);
        cJSON_Delete(jb->args);
        free(jb->tool);
        free(jb);
        pthread_mutex_lock(&q_lock);
        workers_busy--;
        pthread_mutex_unlock(&q_lock);
    }
    pthread_attr_destroy(&at);
}

/* ------------------------------------------------------------ session */

typedef struct {
    int want_pair;
    int unpair_and_exit;
} options_t;

static void on_message(const char *text, const options_t *opt, int *done)
{
    cJSON *m = cJSON_Parse(text);
    if (!m)
        return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(m, "type"));
    if (!type)
        type = "";
    if (!strcmp(type, "call")) {
        start_job(m);
    }
    else if (!strcmp(type, "welcome")) {
        set_state("connected");
        nlog("connected to the relay");
        activity("Online: Claude can reach this Miyoo");
    }
    else if (!strcmp(type, "pair_code")) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(m, "code"));
        if (code) {
            snprintf(st_code, sizeof(st_code), "%s", code);
            status_write();
            nlog("PAIRING CODE: %.3s %.3s  (type it in Claude; valid for 10 minutes)", code, code + 3);
        }
    }
    else if (!strcmp(type, "paired")) {
        const char *cl = cJSON_GetStringValue(cJSON_GetObjectItem(m, "client"));
        snprintf(st_client, sizeof(st_client), "%s", cl ? cl : "Claude");
        st_code[0] = 0;
        status_write();
        paired_flag(1);
        nlog("paired with %s", st_client);
        activity("Connected to %s", st_client);
    }
    else if (!strcmp(type, "unpaired")) {
        st_client[0] = 0;
        status_write();
        paired_flag(0);
        nlog("every Claude connection to this device was removed");
        if (opt->unpair_and_exit)
            *done = 1;
    }
    cJSON_Delete(m);
}

/* one connection, until it drops; returns -2 if the identity was refused */
static int session(const server_t *sv, const identity_t *idn, const options_t *opt)
{
    conn_t c;
    set_state("connecting");
    if (conn_open(&c, sv) != 0)
        return -1;
    int hs = ws_handshake(&c, sv, idn);
    if (hs != 0) {
        conn_close(&c);
        return hs;
    }
    fcntl(c.fd, F_SETFL, fcntl(c.fd, F_GETFL) | O_NONBLOCK);

    /* hello: the tools and a short description of the device */
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON *tl = tools_list();
    cJSON_AddItemToObject(hello, "tools", cJSON_DetachItemFromObject(tl, "tools"));
    cJSON_Delete(tl);
    cJSON_AddItemToObject(hello, "info", device_summary());
    queue_json(hello);
    cJSON_Delete(hello);
    if ((opt->want_pair && !st_client[0]) || st_code[0])
        queue_text(strdup("{\"type\":\"pair_request\"}"));
    if (opt->unpair_and_exit)
        queue_text(strdup("{\"type\":\"unpair_all\"}"));

    size_t cap = 65536, len = 0;
    unsigned char *in = malloc(cap);
    unsigned char *msg = NULL; /* reassembled message */
    size_t mlen = 0;
    long long last_rx = mono_ms(), last_ping = mono_ms(), last_pair = mono_ms();
    int rc = -1, done = 0;

    while (!g_stop && !done) {
        /* send what the workers queued */
        for (;;) {
            pthread_mutex_lock(&q_lock);
            outmsg *o = q_head;
            if (o) {
                q_head = o->next;
                if (!q_head)
                    q_tail = NULL;
            }
            pthread_mutex_unlock(&q_lock);
            if (!o)
                break;
            size_t olen = o->text ? strlen(o->text) : 0;
            int r = o->text ? ws_send_frame(&c, 1, (unsigned char *)o->text, olen) : 0;
            free(o->text);
            free(o);
            if (olen > 32 * 1024) /* a screenshot or a big file: give the memory back to the system */
                malloc_trim(0);
            if (r != 0)
                goto out;
        }
        long long now = mono_ms();
        if (now - last_ping > PING_EVERY_MS) {
            if (ws_send_frame(&c, 1, (const unsigned char *)"{\"type\":\"ping\"}", 15) != 0)
                break;
            last_ping = now;
        }
        if (now - last_rx > DEAD_AFTER_MS) {
            nlog("no answer from the relay for %d s; reconnecting", DEAD_AFTER_MS / 1000);
            break;
        }
        if (g_want_code) {
            g_want_code = 0;
            st_client[0] = 0;
            queue_text(strdup("{\"type\":\"pair_request\"}"));
            last_pair = now;
        }
        if (g_want_tools) {
            g_want_tools = 0;
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "type", "tools");
            cJSON *tl = tools_list();
            cJSON_AddItemToObject(m, "tools", cJSON_DetachItemFromObject(tl, "tools"));
            cJSON_Delete(tl);
            queue_json(m);
            cJSON_Delete(m);
        }
        if (g_want_unpair) {
            g_want_unpair = 0;
            st_code[0] = 0;
            status_write();
            queue_text(strdup("{\"type\":\"unpair_all\"}"));
        }
        if (st_code[0] && !st_client[0] && now - last_pair > 9 * 60 * 1000) {
            queue_text(strdup("{\"type\":\"pair_request\"}")); /* fresh code before the old one expires */
            last_pair = now;
        }

        if (!conn_pending(&c)) {
            struct pollfd p[2] = {{c.fd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
            poll(p, 2, 1000);
            if (p[1].revents & POLLIN) {
                char junk[64];
                while (read(wake_pipe[0], junk, sizeof(junk)) > 0) {
                }
            }
            if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
        }
        if (len + 16384 > cap) {
            if (cap >= MAX_MSG + 65536) {
                nlog("message too large");
                break;
            }
            in = realloc(in, cap *= 2);
        }
        int r = conn_read(&c, in + len, cap - len);
        if (r < 0) {
            nlog("the relay closed the connection");
            break;
        }
        if (r == 0)
            continue;
        len += r;
        last_rx = mono_ms();

        /* parse complete frames */
        for (;;) {
            if (len < 2)
                break;
            int fin = in[0] & 0x80, op = in[0] & 15, masked = in[1] & 0x80;
            unsigned long long pl = in[1] & 127;
            size_t h = 2;
            if (pl == 126) {
                if (len < 4)
                    break;
                pl = (in[2] << 8) | in[3];
                h = 4;
            }
            else if (pl == 127) {
                if (len < 10)
                    break;
                pl = 0;
                for (int k = 0; k < 8; k++)
                    pl = (pl << 8) | in[2 + k];
                h = 10;
            }
            if (masked)
                h += 4;
            if (pl > MAX_MSG) {
                nlog("message too large");
                goto out;
            }
            if (len < h + pl)
                break;
            unsigned char *data = in + h;
            if (masked)
                for (size_t k = 0; k < pl; k++)
                    data[k] ^= in[h - 4 + (k & 3)];
            if (op == 8) { /* close */
                ws_send_frame(&c, 8, data, pl > 2 ? 2 : pl);
                nlog("the relay closed the connection");
                goto out;
            }
            else if (op == 9) /* ping */
                ws_send_frame(&c, 10, data, pl);
            else if (op == 1 || op == 2 || op == 0) {
                unsigned char *nm = realloc(msg, mlen + pl + 1);
                if (!nm)
                    goto out;
                msg = nm;
                memcpy(msg + mlen, data, pl);
                mlen += pl;
                if (fin) {
                    msg[mlen] = 0;
                    on_message((char *)msg, opt, &done);
                    mlen = 0;
                }
            }
            memmove(in, in + h + pl, len - h - pl);
            len -= h + pl;
        }
    }
    rc = done ? 1 : -1;
out:
    if (!g_stop && !done)
        activity("Connection lost, trying again");
    free(in);
    free(msg);
    if (g_stop)
        ws_send_frame(&c, 8, (const unsigned char *)"\x03\xe8", 2);
    conn_close(&c);
    set_state("offline");
    return rc;
}

static void on_signal(int s)
{
    if (s == SIGUSR1)
        g_want_code = 1;
    else if (s == SIGUSR2)
        g_want_unpair = 1;
    else if (s == SIGHUP)
        g_want_tools = 1; /* the app changed which tools are on */
    else
        g_stop = 1;
    if (wake_pipe[1] >= 0 && write(wake_pipe[1], "s", 1) < 0) {
    }
}

/* the app starts "connect" each time; only one may run */
static void single_instance(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/agent.pid", g_home);
    char *s = slurp(path);
    if (s) {
        pid_t old = atoi(s);
        free(s);
        char exe[64], me[1024] = "", it[1024] = "";
        snprintf(exe, sizeof(exe), "/proc/%d/exe", old);
        ssize_t a = readlink(exe, it, sizeof(it) - 1), b = readlink("/proc/self/exe", me, sizeof(me) - 1);
        if (old > 1 && old != getpid() && a > 0 && b > 0 && (it[a] = 0, me[b] = 0, !strcmp(it, me))) {
            kill(old, SIGTERM);
            for (int i = 0; i < 30 && kill(old, 0) == 0; i++)
                usleep(100000);
        }
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

int net_main(int argc, char **argv)
{
    options_t opt = {0};
    g_is_net = !strcmp(argv[1], "connect");
    if (!strcmp(argv[1], "unpair"))
        opt.unpair_and_exit = 1;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--pair"))
            opt.want_pair = 1;

    char d[1024];
    snprintf(d, sizeof(d), "%s/logs", g_home);
    mkdir(d, 0755);
    server_t sv0;
    if (!strcmp(argv[1], "server")) {
        if (server_load(&sv0) != 0)
            return 1;
        puts(sv0.url);
        return 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal; /* no SA_RESTART: poll wakes up */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (pipe(wake_pipe) != 0)
        return 1;
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    if (!opt.unpair_and_exit)
        single_instance();

    server_t sv;
    if (server_load(&sv) != 0) {
        nlog("bad relay address in config/server.txt");
        return 1;
    }
    if (sv.tls)
        sync_clock();
    /* the random generator is needed for websocket masks even without TLS */
    if (sv.tls ? tls_global_init() : (psa_crypto_init(), mbedtls_entropy_init(&g_entropy),
                                      mbedtls_ctr_drbg_init(&g_drbg),
                                      mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, NULL, 0)))
        return 1;

    st_code[0] = st_client[0] = 0;
    set_state("connecting");
    identity_t idn;
    int backoff = 2, rc = 0;
    while (!g_stop) {
        if (identity_load(&idn) != 0 || strcmp(idn.server, sv.url) != 0) {
            if (do_register(&sv, &idn) != 0)
                goto wait;
        }
        int r = session(&sv, &idn, &opt);
        if (r == 1)
            break; /* unpair done */
        if (r == -2) {
            /* the relay does not know this device (it was reset): start over */
            char path[1024];
            snprintf(path, sizeof(path), "%s/config/identity.json", g_home);
            unlink(path);
            backoff = 2;
            continue;
        }
        if (r == -1 && st_state[0] && !strcmp(st_state, "offline"))
            backoff = 2; /* it was connected; retry soon */
    wait:
        if (g_stop || opt.unpair_and_exit) {
            rc = opt.unpair_and_exit ? 1 : 0;
            if (opt.unpair_and_exit)
                break;
        }
        set_state("offline");
        nlog("trying again in %d s", backoff);
        for (int i = 0; i < backoff * 10 && !g_stop; i++)
            usleep(100000);
        backoff = backoff * 2 > 60 ? 60 : backoff * 2;
    }
    set_state("stopped");
    if (!opt.unpair_and_exit) {
        snprintf(d, sizeof(d), "%s/agent.pid", g_home);
        unlink(d);
    }
    nlog("stopped");
    return rc;
}
