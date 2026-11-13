/*
 * Claude Agent: tools for developers (turned on in Settings > Developer options).
 *
 *   download_file   the Miyoo downloads a file from the internet (https), checks it, saves it
 *   input_record    records the buttons the person presses, with exact timing
 *   input_play      replays a recording, or a timeline written by Claude, with timed screenshots
 *   view_image      shows Claude a picture saved on the SD card (for example those screenshots)
 *   job_status      long downloads and replays run in the background: progress, result, cancel
 *
 * Jobs run in their own threads inside the "connect" process, so they keep going while
 * Claude does other things, and are not limited by the time a single tool call may take.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"
#include "mbedtls/sha256.h"

#define JOB_WAIT_MS 100000 /* a call waits this long for its job, then returns the job id */

/* ------------------------------------------------------------------ jobs */



pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static job_t *jobs = NULL;
static int next_job_id = 1;

void job_progress(job_t *j, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&jobs_lock);
    snprintf(j->progress, sizeof(j->progress), "%s", buf);
    pthread_mutex_unlock(&jobs_lock);
}

void job_finish(job_t *j, cJSON *result)
{
    pthread_mutex_lock(&jobs_lock);
    j->result = result;
    j->ended = now_ms();
    j->done = 1;
    pthread_mutex_unlock(&jobs_lock);
}

/* starts fn(job) in a thread; keeps the 20 newest jobs */
job_t *job_start(const char *kind, const char *what, void *(*fn)(void *), void *arg)
{
    job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    snprintf(j->kind, sizeof(j->kind), "%s", kind);
    snprintf(j->what, sizeof(j->what), "%s", what);
    snprintf(j->progress, sizeof(j->progress), "starting");
    j->started = now_ms();
    j->arg = arg;
    pthread_mutex_lock(&jobs_lock);
    j->id = next_job_id++;
    j->next = jobs;
    jobs = j;
    /* forget old finished jobs */
    int n = 0;
    for (job_t **pp = &jobs; *pp;) {
        if (++n > 20 && (*pp)->done) {
            job_t *old = *pp;
            *pp = old->next;
            cJSON_Delete(old->result);
            free(old);
        }
        else
            pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&jobs_lock);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, fn, j) != 0) {
        job_finish(j, result_error("could not start the job"));
    }
    pthread_attr_destroy(&at);
    return j;
}

static job_t *job_find(int id)
{
    for (job_t *j = jobs; j; j = j->next)
        if (j->id == id)
            return j;
    return NULL;
}

/* a running job of this kind, if any */
job_t *job_running(const char *kind)
{
    for (job_t *j = jobs; j; j = j->next)
        if (!j->done && !strcmp(j->kind, kind))
            return j;
    return NULL;
}

/* copy of the job's result, or a "still running" note */
static cJSON *job_report(job_t *j)
{
    pthread_mutex_lock(&jobs_lock);
    cJSON *r;
    if (j->done)
        r = cJSON_Duplicate(j->result, 1);
    else {
        r = result_new();
        result_textf(r, "Job %d is still running: %s. Now: %s. Call job_status with id %d to follow it "
                        "(add wait_s to wait for it, or cancel true to stop it).",
                     j->id, j->what, j->progress, j->id);
    }
    pthread_mutex_unlock(&jobs_lock);
    return r;
}

/* waits for the job up to ms, then reports */
cJSON *job_wait(job_t *j, int ms)
{
    long long until = now_ms() + ms;
    while (!j->done && now_ms() < until)
        sleep_ms(100);
    return job_report(j);
}

cJSON *tool_job_status(cJSON *args)
{
    int id = (int)arg_num(args, "id", 0);
    int cancel = arg_bool(args, "cancel", 0);
    int wait_s = (int)arg_num(args, "wait_s", 0);
    if (wait_s > 90)
        wait_s = 90;
    if (!id) {
        cJSON *r = result_new();
        char buf[4000] = "";
        size_t n = 0;
        pthread_mutex_lock(&jobs_lock);
        for (job_t *j = jobs; j && n < sizeof(buf) - 400; j = j->next)
            n += snprintf(buf + n, sizeof(buf) - n, "job %d [%s] %s: %s\n", j->id,
                          j->done ? "finished" : "running", j->what, j->progress);
        pthread_mutex_unlock(&jobs_lock);
        result_text(r, n ? buf : "No jobs since the agent started.");
        return r;
    }
    pthread_mutex_lock(&jobs_lock);
    job_t *j = job_find(id);
    pthread_mutex_unlock(&jobs_lock);
    if (!j)
        return result_error("no job %d (jobs are forgotten when the agent restarts)", id);
    if (cancel && !j->done) {
        j->cancel = 1;
        activity("Cancelling job %d", id);
        wait_s = wait_s < 10 ? 10 : wait_s;
    }
    return job_wait(j, wait_s * 1000);
}

/* ---------------------------------------------------------- download_file */

typedef struct {
    char url[2048];
    char path[1024];
    char part[1100];
    char sha_want[65];
    int executable;
    FILE *f;
    long long got, total, max;
    long long last_note;
    mbedtls_sha256_context sha;
    job_t *job;
    char err[300];
} dl_t;

static int dl_sink(void *ctx, const unsigned char *data, size_t len)
{
    dl_t *d = ctx;
    if (d->got + (long long)len > d->max) {
        snprintf(d->err, sizeof(d->err), "the file is bigger than the free space on the SD card");
        return -1;
    }
    if (fwrite(data, 1, len, d->f) != len) {
        snprintf(d->err, sizeof(d->err), "could not write to the SD card: %s", strerror(errno));
        return -1;
    }
    mbedtls_sha256_update(&d->sha, data, len);
    d->got += len;
    long long now = now_ms();
    if (now - d->last_note > 1000) {
        d->last_note = now;
        if (d->total > 0)
            job_progress(d->job, "%.1f of %.1f MB (%d%%)", d->got / 1048576.0, d->total / 1048576.0,
                         (int)(d->got * 100 / d->total));
        else
            job_progress(d->job, "%.1f MB", d->got / 1048576.0);
    }
    return 0;
}

static int backup_move(const char *path, char *where, size_t wsize);

static void *download_thread(void *arg)
{
    job_t *j = arg;
    dl_t *d = j->arg;
    d->job = j;
    long long t0 = now_ms();
    mbedtls_sha256_init(&d->sha);
    mbedtls_sha256_starts(&d->sha, 0);
    d->f = fopen(d->part, "wb");
    cJSON *r = NULL;
    if (!d->f)
        r = result_error("cannot create %s: %s", d->part, strerror(errno));
    else {
        char err[300];
        long long total = -1;
        int rc = https_get(d->url, dl_sink, d, &total, &j->cancel, err, sizeof(err));
        d->total = total;
        if (fclose(d->f) != 0 && rc == 0) {
            rc = -1;
            snprintf(err, sizeof(err), "could not finish writing the file: %s", strerror(errno));
        }
        unsigned char sum[32];
        mbedtls_sha256_finish(&d->sha, sum);
        char hex[65];
        for (int i = 0; i < 32; i++)
            sprintf(hex + i * 2, "%02x", sum[i]);
        if (rc != 0) {
            unlink(d->part);
            r = result_error("Download failed: %s", d->err[0] ? d->err : err);
        }
        else if (d->sha_want[0] && strcasecmp(hex, d->sha_want) != 0) {
            unlink(d->part);
            r = result_error("The file arrived different from what was expected (sha256 %s, expected %s). "
                             "Nothing was changed.",
                             hex, d->sha_want);
        }
        else {
            char backup[1200] = "";
            struct stat st;
            if (stat(d->path, &st) == 0 && S_ISREG(st.st_mode) && backup_move(d->path, backup, sizeof(backup)) != 0) {
                unlink(d->part);
                r = result_error("could not back up the old %s, so it was not replaced", d->path);
            }
            else if (rename(d->part, d->path) != 0) {
                unlink(d->part);
                r = result_error("could not save %s: %s", d->path, strerror(errno));
            }
            else {
                if (d->executable)
                    chmod(d->path, 0755);
                sync();
                double secs = (now_ms() - t0) / 1000.0;
                r = result_new();
                result_textf(r, "Downloaded %s (%lld bytes, %.1f s, %.0f KB/s). sha256 %s%s%s%s", d->path, d->got,
                             secs, secs > 0 ? d->got / 1024.0 / secs : 0.0, hex,
                             d->sha_want[0] ? " (checked)" : "", backup[0] ? ". The old file was backed up in " : "",
                             backup);
                activity("  downloaded %s (%.1f MB)", d->path, d->got / 1048576.0);
                change_log(backup[0] ? "changed" : "created", d->path, backup);
            }
        }
    }
    mbedtls_sha256_free(&d->sha);
    job_progress(j, "%s", cJSON_IsTrue(cJSON_GetObjectItem(r, "isError")) ? "failed" : "done");
    free(d);
    job_finish(j, r);
    return NULL;
}

/* the old file goes to the backups folder: moved (instant, even when big), or copied */
static int backup_move(const char *path, char *where, size_t wsize)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    if (!strncmp(path, "/tmp/", 5))
        return 0;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32], dst[2200], dir[2200];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(dst, sizeof(dst), "%s/backups/%s%s", g_home, stamp, path);
    for (int k = 2; access(dst, F_OK) == 0 && k < 100; k++)
        snprintf(dst, sizeof(dst), "%s/backups/%s-%d%s", g_home, stamp, k, path);
    snprintf(dir, sizeof(dir), "%s", dst);
    *strrchr(dir, '/') = 0;
    if (mkdirs(dir) == 0 && rename(path, dst) == 0) {
        snprintf(where, wsize, "%s", dst);
        return 0;
    }
    return backup_file(path, where, wsize);
}

/* "a/b/c" -> makes a/b */
static int make_parent(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = 0;
    return mkdirs(dir);
}

cJSON *tool_download_file(cJSON *args)
{
    const char *url = arg_str(args, "url", NULL);
    const char *path = arg_str(args, "path", NULL);
    const char *sha = arg_str(args, "sha256", "");
    if (!url || !path)
        return result_error("missing 'url' or 'path'");
    if (strncmp(url, "https://", 8) != 0)
        return result_error("only https:// addresses are allowed");
    if (path[0] != '/' || strstr(path, "/../"))
        return result_error("'path' must be a full path, like /mnt/SDCARD/Roms/PORTS/game.zip");
    if (write_path_blocked(path))
        return result_error("writing to %s is not allowed", path);
    if (sha[0] && strlen(sha) != 64)
        return result_error("'sha256' must have 64 hex characters");
    if (arg_bool(args, "make_dirs", 1) && make_parent(path) != 0)
        return result_error("cannot create the folder for %s", path);

    dl_t *d = calloc(1, sizeof(*d));
    if (!d)
        return result_error("out of memory");
    snprintf(d->url, sizeof(d->url), "%s", url);
    snprintf(d->path, sizeof(d->path), "%s", path);
    snprintf(d->part, sizeof(d->part), "%s.part", path);
    snprintf(d->sha_want, sizeof(d->sha_want), "%s", sha);
    d->executable = arg_bool(args, "executable", 0);
    struct statvfs vs;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    d->max = statvfs(dir[0] ? dir : "/", &vs) == 0 ? (long long)vs.f_bavail * vs.f_frsize - 8 * 1048576LL
                                                   : 4LL * 1024 * 1048576;
    if (d->max <= 0) {
        free(d);
        return result_error("the SD card is full");
    }
    char what[300];
    const char *name = strrchr(url, '/');
    snprintf(what, sizeof(what), "download %.120s to %.150s", name ? name + 1 : url, path);
    job_t *j = job_start("download", what, download_thread, d);
    if (!j) {
        free(d);
        return result_error("out of memory");
    }
    activity("Downloading to %s (job %d)", path, j->id);
    return job_wait(j, arg_bool(args, "wait", 1) ? JOB_WAIT_MS : 0);
}

/* ------------------------------------------------------------- view_image */

cJSON *tool_view_image(cJSON *args)
{
    const char *path = arg_str(args, "path", NULL);
    if (!path)
        return result_error("missing 'path'");
    FILE *f = fopen(path, "rb");
    if (!f)
        return result_error("cannot open %s: %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1048576) {
        fclose(f);
        return result_error("%s is empty or bigger than 4 MB", path);
    }
    unsigned char *buf = malloc(n);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return result_error("cannot read %s", path);
    }
    fclose(f);
    const char *mime = NULL;
    if (n > 8 && !memcmp(buf, "\x89PNG", 4))
        mime = "image/png";
    else if (n > 3 && buf[0] == 0xff && buf[1] == 0xd8)
        mime = "image/jpeg";
    else if (n > 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WEBP", 4))
        mime = "image/webp";
    else if (n > 6 && !memcmp(buf, "GIF8", 4))
        mime = "image/gif";
    if (!mime) {
        free(buf);
        return result_error("%s is not a PNG, JPEG, WEBP or GIF picture", path);
    }
    cJSON *r = result_new();
    result_image(r, buf, n, mime);
    result_textf(r, "%s (%ld bytes)", path, n);
    free(buf);
    return r;
}

/* ------------------------------------------------- input recording / replay */

#define MAX_EVENTS 50000

typedef struct {
    int t;    /* ms from the start */
    int code; /* key code, or 0 for a screenshot */
    int value; /* 1 down, 0 up */
    int idx;  /* order given, for events at the same moment */
    char label[48];
} ev_t;

static char rec_dir[1024];

static const char *recordings_dir(void)
{
    snprintf(rec_dir, sizeof(rec_dir), "%s/recordings", g_home);
    mkdirs(rec_dir);
    return rec_dir;
}

/* names: letters, digits, - and _ */
static int name_ok(const char *n)
{
    if (!n || !*n || strlen(n) > 60)
        return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_'))
            return 0;
    return 1;
}

static const char *input_dev(void)
{
    return getenv("CA_INPUT") ? getenv("CA_INPUT") : "/dev/input/event0";
}

/* --- recording */

typedef struct {
    char name[64];
    char path[1200];
    int max_s;
    int count;
} rec_t;

static void *record_thread(void *arg)
{
    job_t *j = arg;
    rec_t *rc = j->arg;
    int fd = open(input_dev(), O_RDONLY | O_NONBLOCK);
    FILE *f = fopen(rc->path, "w");
    if (fd < 0 || !f) {
        if (fd >= 0)
            close(fd);
        if (f)
            fclose(f);
        job_finish(j, result_error("cannot start recording: %s", strerror(errno)));
        free(rc);
        return NULL;
    }
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "# Claude Agent input recording v1, %s\n# ms button down|up\n", when);
    long long t0 = now_ms(), last_note = 0;
    int first = -1, last = 0;
    while (!j->cancel && now_ms() - t0 < (long long)rc->max_s * 1000) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) {
            if (now_ms() - last_note > 1000) {
                last_note = now_ms();
                job_progress(j, "recording, %d presses, %lld s", rc->count / 2, (now_ms() - t0) / 1000);
            }
            continue;
        }
        struct input_event ev;
        while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type != EV_KEY || ev.value == 2) /* repeats come back by themselves on replay */
                continue;
            const char *b = button_name(ev.code);
            if (!b)
                continue;
            int t = (int)(now_ms() - t0);
            if (first < 0)
                first = t;
            last = t;
            fprintf(f, "%d %s %s\n", t, b, ev.value ? "down" : "up");
            rc->count++;
        }
        fflush(f);
    }
    close(fd);
    fclose(f);
    int secs = (int)((now_ms() - t0) / 1000);
    cJSON *r = result_new();
    if (rc->count)
        result_textf(r,
                     "Recording '%s' saved: %d button events in %d s (first press at %d ms, last at %d ms). File: %s. "
                     "Replay it with input_play {\"recording\":\"%s\"}.",
                     rc->name, rc->count, secs, first, last, rc->path, rc->name);
    else
        result_textf(r, "Recording '%s' stopped after %d s, but no button was pressed. File: %s", rc->name, secs,
                     rc->path);
    activity("Recording '%s' stopped: %d button events", rc->name, rc->count);
    job_progress(j, "finished, %d events", rc->count);
    free(rc);
    job_finish(j, r);
    return NULL;
}

cJSON *tool_input_record(cJSON *args)
{
    const char *action = arg_str(args, "action", "start");
    pthread_mutex_lock(&jobs_lock);
    job_t *running = job_running("record");
    pthread_mutex_unlock(&jobs_lock);
    if (!strcmp(action, "stop")) {
        if (!running)
            return result_error("no recording is running");
        running->cancel = 1;
        return job_wait(running, 5000);
    }
    if (!strcmp(action, "list")) {
        char cmd[1200], out[4000] = "";
        snprintf(cmd, sizeof(cmd), "ls -l '%s' 2>/dev/null", recordings_dir());
        FILE *p = popen(cmd, "r");
        size_t n = p ? fread(out, 1, sizeof(out) - 1, p) : 0;
        if (p)
            pclose(p);
        out[n] = 0;
        cJSON *r = result_new();
        result_textf(r, "Recordings in %s:\n%s", recordings_dir(), n ? out : "(none)");
        return r;
    }
    if (strcmp(action, "start") != 0)
        return result_error("'action' must be start, stop or list");
    if (running)
        return result_error("a recording is already running (job %d); stop it first", running->id);
    const char *name = arg_str(args, "name", NULL);
    if (!name_ok(name))
        return result_error("'name' is required: letters, digits, - and _ only (for example level1_run)");
    rec_t *rc = calloc(1, sizeof(*rc));
    snprintf(rc->name, sizeof(rc->name), "%s", name);
    snprintf(rc->path, sizeof(rc->path), "%s/%s.txt", recordings_dir(), name);
    rc->max_s = (int)arg_num(args, "max_s", 600);
    if (rc->max_s < 5)
        rc->max_s = 5;
    if (rc->max_s > 3600)
        rc->max_s = 3600;
    char what[200];
    snprintf(what, sizeof(what), "record buttons as '%s' (up to %d s)", name, rc->max_s);
    job_t *j = job_start("record", what, record_thread, rc);
    if (!j)
        return result_error("out of memory");
    activity("Recording buttons as '%s'", name);
    cJSON *r = result_new();
    result_textf(r,
                 "Recording started (job %d), saving to %s. Every button the person presses is recorded with its "
                 "time, for up to %d s. Call input_record with action stop when they are done.",
                 j->id, rc->path, rc->max_s);
    return r;
}

/* --- replay */

typedef struct {
    ev_t *ev;
    int n;
    char source[100];
    char shots_dir[1100];
    int allow_power;
} play_t;

typedef struct {
    unsigned char *raw;
    screen_raw_t sr;
    char path[1300];
} shot_t;

/* PNG encoding takes a few hundred ms on the Miyoo: done here, so the buttons keep their timing */
static void *shot_thread(void *arg)
{
    shot_t *s = arg;
    size_t len = 0;
    unsigned char *png = screen_png(s->raw, &s->sr, 1, &len);
    if (png) {
        FILE *f = fopen(s->path, "wb");
        if (f) {
            fwrite(png, 1, len, f);
            fclose(f);
        }
        free(png);
    }
    free(s->raw);
    free(s);
    return NULL;
}

static int ev_cmp(const void *a, const void *b)
{
    const ev_t *x = a, *y = b;
    if (x->t != y->t)
        return x->t - y->t;
    return x->idx - y->idx;
}

static void sleep_until(const struct timespec *t0, int ms)
{
    struct timespec t = *t0;
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {
    }
}

static int ms_since(const struct timespec *t0)
{
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return (int)((n.tv_sec - t0->tv_sec) * 1000 + (n.tv_nsec - t0->tv_nsec) / 1000000L);
}

static void emit_key(int fd, int code, int value)
{
    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY;
    ev[0].code = code;
    ev[0].value = value;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    if (write(fd, ev, sizeof(ev)) != sizeof(ev)) {
    }
}

static void *play_thread(void *arg)
{
    job_t *j = arg;
    play_t *p = j->arg;
    int fd = open(input_dev(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        job_finish(j, result_error("cannot open %s: %s", input_dev(), strerror(errno)));
        free(p->ev);
        free(p);
        return NULL;
    }
    char held[KEY_MAX / 8 + 1];
    memset(held, 0, sizeof(held));
    int sent = 0, shots = 0, late_max = 0, late_sum = 0, late_n = 0;
    char shot_list[3000] = "";
    size_t sl = 0;
    int total = p->n ? p->ev[p->n - 1].t : 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long long last_note = 0;
    int i;
    for (i = 0; i < p->n && !j->cancel; i++) {
        ev_t *e = &p->ev[i];
        /* sleep in steps, to notice cancel */
        while (!j->cancel && e->t - ms_since(&t0) > 250)
            sleep_until(&t0, ms_since(&t0) + 200);
        if (j->cancel)
            break;
        sleep_until(&t0, e->t);
        int late = ms_since(&t0) - e->t;
        if (e->code) {
            emit_key(fd, e->code, e->value);
            if (e->code < KEY_MAX) {
                if (e->value)
                    held[e->code / 8] |= 1 << (e->code % 8);
                else
                    held[e->code / 8] &= ~(1 << (e->code % 8));
            }
            sent++;
            if (late > late_max)
                late_max = late;
            late_sum += late;
            late_n++;
        }
        else {
            shot_t *s = calloc(1, sizeof(*s));
            char err[200];
            if (s && (s->raw = screen_grab(&s->sr, err, sizeof(err)))) {
                snprintf(s->path, sizeof(s->path), "%s/%06d%s%s.png", p->shots_dir, e->t, e->label[0] ? "_" : "",
                         e->label);
                if (sl < sizeof(shot_list) - 200)
                    sl += snprintf(shot_list + sl, sizeof(shot_list) - sl, "\n  %s (at %d ms, taken %d ms late)", s->path,
                                   e->t, late);
                pthread_t th;
                pthread_attr_t at;
                pthread_attr_init(&at);
                pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                if (pthread_create(&th, &at, shot_thread, s) != 0) {
                    free(s->raw);
                    free(s);
                }
                pthread_attr_destroy(&at);
                shots++;
            }
            else
                free(s);
        }
        if (now_ms() - last_note > 1000) {
            last_note = now_ms();
            job_progress(j, "%d of %d s, %d button events, %d screenshots", e->t / 1000, total / 1000, sent, shots);
        }
    }
    /* nothing stays pressed */
    for (int c = 0; c < KEY_MAX; c++)
        if (held[c / 8] & (1 << (c % 8)))
            emit_key(fd, c, 0);
    close(fd);
    sleep_ms(600); /* let the last screenshots finish */
    cJSON *r = result_new();
    result_textf(r,
                 "%s %s: %d button events in %.1f s. Timing: events were on average %.1f ms and at most %d ms "
                 "after their planned time.%s%s",
                 j->cancel ? "Cancelled the replay of" : "Replayed", p->source, sent, ms_since(&t0) / 1000.0,
                 late_n ? (double)late_sum / late_n : 0.0, late_max,
                 shots ? "\nScreenshots (look at them with view_image):" : "", shot_list);
    activity("Replay of %s %s: %d button events, %d screenshots", p->source, T(j->cancel ? "cancelled" : "done"), sent,
             shots);
    job_progress(j, "%s", j->cancel ? "cancelled" : "done");
    free(p->ev);
    free(p);
    job_finish(j, r);
    return NULL;
}

/* adds one event; returns -1 when full */
static int ev_add(play_t *p, int cap, int t, int code, int value, const char *label)
{
    if (p->n >= cap)
        return -1;
    ev_t *e = &p->ev[p->n];
    e->idx = p->n++;
    e->t = t < 0 ? 0 : t;
    e->code = code;
    e->value = value;
    snprintf(e->label, sizeof(e->label), "%s", label ? label : "");
    for (char *c = e->label; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-'))
            *c = '_';
    return 0;
}

static cJSON *load_recording(play_t *p, int cap, const char *name, int from_first)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.txt", recordings_dir(), name);
    FILE *f = fopen(path, "r");
    if (!f)
        return result_error("no recording named '%s' (input_record with action list shows them)", name);
    char line[200];
    int offset = -1;
    while (fgets(line, sizeof(line), f)) {
        int t;
        char b[32], v[16];
        if (line[0] == '#' || sscanf(line, "%d %31s %15s", &t, b, v) != 3)
            continue;
        int code = button_code(b);
        if (code < 0)
            continue;
        if (from_first && offset < 0)
            offset = t;
        if (ev_add(p, cap, t - (offset > 0 ? offset : 0), code, !strcmp(v, "down"), NULL) != 0) {
            fclose(f);
            return result_error("recording '%s' is too long (more than %d events)", name, cap);
        }
    }
    fclose(f);
    return NULL;
}

cJSON *tool_input_play(cJSON *args)
{
    pthread_mutex_lock(&jobs_lock);
    job_t *running = job_running("play");
    job_t *recording = job_running("record");
    pthread_mutex_unlock(&jobs_lock);
    if (running)
        return result_error("a replay is already running (job %d)", running->id);
    if (recording)
        return result_error("a recording is running (job %d); stop it first", recording->id);

    const char *rec = arg_str(args, "recording", NULL);
    cJSON *tl = cJSON_GetObjectItem(args, "timeline");
    if (!rec == !tl)
        return result_error("give either 'recording' (a name) or 'timeline' (a list of events)");
    int allow_power = arg_bool(args, "allow_power", 0);

    play_t *p = calloc(1, sizeof(*p));
    p->ev = calloc(MAX_EVENTS, sizeof(ev_t));
    if (!p || !p->ev) {
        if (p)
            free(p->ev);
        free(p);
        return result_error("out of memory");
    }
    cJSON *err = NULL;
    if (rec) {
        if (!name_ok(rec))
            err = result_error("bad recording name");
        else
            err = load_recording(p, MAX_EVENTS, rec, arg_bool(args, "start_at_first_press", 0));
        snprintf(p->source, sizeof(p->source), "recording '%s'", rec);
    }
    else {
        if (!cJSON_IsArray(tl))
            err = result_error("'timeline' must be a list");
        int i = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, tl)
        {
            if (err)
                break;
            i++;
            int t = (int)arg_num(it, "t", -1);
            if (t < 0) {
                err = result_error("timeline item %d has no 't' (milliseconds from the start)", i);
                break;
            }
            const char *down = arg_str(it, "down", NULL), *up = arg_str(it, "up", NULL),
                       *tap = arg_str(it, "tap", NULL);
            cJSON *shot = cJSON_GetObjectItem(it, "shot");
            const char *b = down ? down : up ? up : tap;
            if (b) {
                int code = button_code(b);
                if (code < 0) {
                    err = result_error("timeline item %d: unknown button '%s'", i, b);
                    break;
                }
                int r1, r2 = 0;
                if (tap) {
                    int hold = (int)arg_num(it, "hold", 80);
                    r1 = ev_add(p, MAX_EVENTS, t, code, 1, NULL);
                    r2 = ev_add(p, MAX_EVENTS, t + (hold < 20 ? 20 : hold), code, 0, NULL);
                }
                else
                    r1 = ev_add(p, MAX_EVENTS, t, code, down ? 1 : 0, NULL);
                if (r1 || r2)
                    err = result_error("the timeline is too long (more than %d events)", MAX_EVENTS);
            }
            else if (shot)
                ev_add(p, MAX_EVENTS, t, 0, 0, cJSON_IsString(shot) ? shot->valuestring : NULL);
            else
                err = result_error("timeline item %d needs one of: down, up, tap, shot", i);
        }
        snprintf(p->source, sizeof(p->source), "timeline");
    }
    /* screenshots at given times */
    cJSON *at = cJSON_GetObjectItem(args, "shots_at");
    if (!err && cJSON_IsArray(at)) {
        cJSON *it;
        cJSON_ArrayForEach(it, at)
        {
            if (cJSON_IsNumber(it))
                ev_add(p, MAX_EVENTS, (int)it->valuedouble, 0, 0, NULL);
        }
    }
    if (!err && !allow_power)
        for (int i = 0; i < p->n; i++)
            if (p->ev[i].code == KEY_POWER) {
                err = result_error("the replay presses POWER, which can put the Miyoo to sleep and cut the "
                                   "connection. Add allow_power true only if that is really wanted.");
                break;
            }
    if (!err && p->n == 0)
        err = result_error("nothing to replay");
    if (err) {
        free(p->ev);
        free(p);
        return err;
    }
    qsort(p->ev, p->n, sizeof(ev_t), ev_cmp);
    p->allow_power = allow_power;
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    const char *dir = arg_str(args, "shots_dir", NULL);
    if (dir)
        snprintf(p->shots_dir, sizeof(p->shots_dir), "%s", dir);
    else
        snprintf(p->shots_dir, sizeof(p->shots_dir), "%s/shots/%s-%s", g_home, rec ? rec : "timeline", stamp);
    for (int i = 0; i < p->n; i++)
        if (!p->ev[i].code) {
            if (mkdirs(p->shots_dir) != 0) {
                cJSON *e = result_error("cannot create %s", p->shots_dir);
                free(p->ev);
                free(p);
                return e;
            }
            break;
        }
    char what[200];
    snprintf(what, sizeof(what), "replay %s (%.1f s)", p->source, p->ev[p->n - 1].t / 1000.0);
    job_t *j = job_start("play", what, play_thread, p);
    if (!j)
        return result_error("out of memory");
    activity("Replaying %s (job %d)", p->source, j->id);
    return job_wait(j, arg_bool(args, "wait", 1) ? JOB_WAIT_MS : 0);
}
