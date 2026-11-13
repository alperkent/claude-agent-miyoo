/*
 * Claude Agent: more tools for developers (Settings > Developer options > Developer tools).
 *
 *   run_app         opens a port, app or command the way OnionOS does, keeps everything it prints,
 *                   watches for words in its output, takes screenshots on a schedule, keeps the last
 *                   frame and a core dump when it crashes, can fix the CPU clock for the run
 *   perf_monitor    samples CPU use and clock, memory, one program's CPU and memory, battery
 *   screen_capture  a series of screenshots: every N ms, from a moment, until a count or a time
 *   watch_file      follows a log file and reports the lines that match
 *   snapshot        saves a folder (a port, saves) to restore it later exactly as it was
 *
 * All of them run as background jobs (see job_status), with adjustable timing.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"

#define JOB_WAIT_MS 100000
#define RUN_PID "/tmp/claude_agent_run.pid"
#define RUN_RC "/tmp/claude_agent_run.rc"

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

/* first process whose name (comm) is name, or 0 */
static pid_t pid_of(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    pid_t found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char p[64], comm[64];
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        if (read_text(p, comm, sizeof(comm)) > 0) {
            comm[strcspn(comm, "\n")] = 0;
            if (!strcmp(comm, name))
                found = atoi(e->d_name);
        }
    }
    closedir(d);
    return found;
}

/* children of pid (all levels), for stopping a program and what it started */
static void kill_tree(pid_t pid, int sig)
{
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            char p[64], st[512];
            snprintf(p, sizeof(p), "/proc/%s/stat", e->d_name);
            if (read_text(p, st, sizeof(st)) <= 0)
                continue;
            char *r = strrchr(st, ')');
            int ppid = 0;
            if (r && sscanf(r + 2, "%*c %d", &ppid) == 1 && ppid == pid)
                kill_tree(atoi(e->d_name), sig);
        }
        closedir(d);
    }
    kill(pid, sig);
}

/* screenshot to a PNG file, in its own thread so the caller keeps its timing */
typedef struct {
    unsigned char *raw;
    screen_raw_t sr;
    int scale;
    char path[1300];
} pending_shot_t;

static void *save_shot(void *arg)
{
    pending_shot_t *s = arg;
    size_t len = 0;
    unsigned char *png = screen_png(s->raw, &s->sr, s->scale, &len);
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

static int shot_async(const char *path, int scale)
{
    pending_shot_t *s = calloc(1, sizeof(*s));
    char err[200];
    if (!s || !(s->raw = screen_grab(&s->sr, err, sizeof(err)))) {
        free(s);
        return -1;
    }
    s->scale = scale;
    snprintf(s->path, sizeof(s->path), "%s", path);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, save_shot, s) != 0) {
        free(s->raw);
        free(s);
        pthread_attr_destroy(&at);
        return -1;
    }
    pthread_attr_destroy(&at);
    return 0;
}

static void stamp_now(char *out, size_t size)
{
    time_t t = time(NULL);
    strftime(out, size, "%Y%m%d-%H%M%S", localtime(&t));
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* shell-quote into out: 'it'\''s' */
static void sq(char *out, size_t size, const char *in)
{
    size_t n = 0;
    if (n < size - 1)
        out[n++] = '\'';
    for (; *in && n < size - 6; in++) {
        if (*in == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        }
        else
            out[n++] = *in;
    }
    out[n++] = '\'';
    out[n] = 0;
}

/* ------------------------------------------------------------------ run_app */

typedef struct {
    char dir[1024];       /* run folder: output.log, shots, core */
    char what[300];
    char watch[8][120];
    int nwatch, stop_on_match;
    int timeout_s;
    int shot_every, shot_start, shot_count;
    int core, reopen;
    char core_pattern_old[256];
} run_t;

/* After a run: open the Claude Agent app again if it was open before, so the person can answer
   questions (permissions, plugin installs) on the Miyoo. Same steps as opening it from the menu. */
static int reopen_app(void)
{
    char launch[PATH_MAX + 16];
    snprintf(launch, sizeof(launch), "%s/launch.sh", g_home);
    if (access(launch, F_OK) != 0 || ui_running())
        return 0;
    pid_t mp = 0;
    for (int i = 0; i < 150 && !(mp = pid_of("MainUI")); i++) /* runtime.sh starts MainUI again */
        sleep_ms(100);
    if (!mp)
        return 0;
    sleep_ms(1500); /* let MainUI finish drawing */
    if (access("/tmp/cmd_to_run.sh", F_OK) == 0) /* something else is already being opened */
        return 0;
    FILE *f = fopen("/tmp/cmd_to_run.sh.tmp", "w");
    if (!f)
        return 0;
    fprintf(f, "cd %s; chmod a+x ./launch.sh; LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so ./launch.sh",
            g_home);
    fclose(f);
    rename("/tmp/cmd_to_run.sh.tmp", "/tmp/cmd_to_run.sh");
    sync();
    if ((mp = pid_of("MainUI")))
        kill(mp, SIGKILL);
    for (int i = 0; i < 50 && !ui_running(); i++)
        sleep_ms(100);
    return 1;
}

static void *run_thread(void *arg)
{
    job_t *j = arg;
    run_t *r = j->arg;
    char out_path[1100], line[1024];
    snprintf(out_path, sizeof(out_path), "%s/output.log", r->dir);

    /* wait for the program to start (MainUI closing and runtime.sh launching it) */
    pid_t pid = 0;
    long long t0 = now_ms();
    while (!j->cancel && now_ms() - t0 < 30000) {
        char b[32];
        if (read_text(RUN_PID, b, sizeof(b)) > 0 && (pid = atoi(b)) > 0)
            break;
        sleep_ms(200);
    }
    cJSON *res = result_new();
    if (!pid) {
        result_textf(res, "The program did not start within 30 s. Is MainUI on screen (not a game or another app)?");
        cJSON_AddBoolToObject(res, "isError", 1);
        job_finish(j, res);
        free(r);
        return NULL;
    }
    activity("Running: %s", r->what);
    long long start = now_ms(), next_shot = start + r->shot_start, last_frame_at = 0;
    int shots = 0, matched = 0;
    FILE *out = NULL;
    char matches[3000] = "";
    size_t ml = 0;
    unsigned char *last_raw = NULL;
    screen_raw_t last_sr;
    int rc = -1, timed_out = 0;
    while (1) {
        /* output: new lines, watched words */
        if (!out)
            out = fopen(out_path, "r");
        if (out) {
            clearerr(out);
            while (fgets(line, sizeof(line), out)) {
                for (int i = 0; i < r->nwatch; i++)
                    if (strstr(line, r->watch[i])) {
                        matched++;
                        line[strcspn(line, "\n")] = 0;
                        if (ml < sizeof(matches) - 200)
                            ml += snprintf(matches + ml, sizeof(matches) - ml, "\n  at %.1f s: %.150s",
                                           (now_ms() - start) / 1000.0, line);
                        break;
                    }
            }
        }
        char b[32];
        if (read_text(RUN_RC, b, sizeof(b)) > 0) {
            rc = atoi(b);
            break;
        }
        if (kill(pid, 0) != 0 && errno == ESRCH) { /* gone without writing the code */
            sleep_ms(300);
            if (read_text(RUN_RC, b, sizeof(b)) > 0)
                rc = atoi(b);
            break;
        }
        long long now = now_ms();
        if (j->cancel || (r->timeout_s && now - start > r->timeout_s * 1000LL) ||
            (r->stop_on_match && matched)) {
            timed_out = !j->cancel && !(r->stop_on_match && matched);
            kill_tree(pid, SIGTERM);
            for (int i = 0; i < 20 && kill(pid, 0) == 0; i++)
                sleep_ms(100);
            if (kill(pid, 0) == 0)
                kill_tree(pid, SIGKILL);
            sleep_ms(300);
            if (read_text(RUN_RC, b, sizeof(b)) > 0)
                rc = atoi(b);
            break;
        }
        if (r->shot_every && shots < r->shot_count && now >= next_shot) {
            char p[1300];
            snprintf(p, sizeof(p), "%s/shot_%06lld.png", r->dir, now - start);
            if (shot_async(p, 1) == 0)
                shots++;
            next_shot += r->shot_every;
        }
        /* keep the latest frame in memory: it becomes last_frame.png if the program crashes */
        if (now - last_frame_at >= 1000) {
            char err[100];
            screen_raw_t sr;
            unsigned char *raw = screen_grab(&sr, err, sizeof(err));
            if (raw) {
                free(last_raw);
                last_raw = raw;
                last_sr = sr;
            }
            last_frame_at = now;
        }
        job_progress(j, "running for %lld s, %d screenshots, %d matches", (now - start) / 1000, shots, matched);
        sleep_ms(50);
    }
    if (out)
        fclose(out);
    double secs = (now_ms() - start) / 1000.0;
    int sig = rc > 128 && rc < 160 ? rc - 128 : 0;
    int crashed = sig && sig != SIGTERM && sig != SIGKILL && sig != SIGINT;
    if (crashed && last_raw) {
        char p[1300];
        snprintf(p, sizeof(p), "%s/last_frame.png", r->dir);
        pending_shot_t *s = calloc(1, sizeof(*s));
        if (s) {
            s->raw = last_raw;
            s->sr = last_sr;
            s->scale = 1;
            snprintf(s->path, sizeof(s->path), "%s", p);
            save_shot(s);
            last_raw = NULL;
        }
    }
    free(last_raw);
    if (r->core_pattern_old[0]) { /* put the system setting back */
        FILE *f = fopen("/proc/sys/kernel/core_pattern", "w");
        if (f) {
            fputs(r->core_pattern_old, f);
            fclose(f);
        }
    }
    /* the end of the output */
    char tail[3000] = "", cmd[1300];
    snprintf(cmd, sizeof(cmd), "tail -n 25 '%s' 2>/dev/null", out_path);
    FILE *p = popen(cmd, "r");
    if (p) {
        size_t n = fread(tail, 1, sizeof(tail) - 1, p);
        tail[n] = 0;
        pclose(p);
    }
    char cores[600] = "";
    if (r->core) {
        snprintf(cmd, sizeof(cmd), "ls -1 '%s' 2>/dev/null | grep '^core' | head -3", r->dir);
        p = popen(cmd, "r");
        if (p) {
            size_t n = fread(cores, 1, sizeof(cores) - 1, p);
            cores[n] = 0;
            pclose(p);
        }
    }
    static const char *names[] = {"", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL (illegal instruction)", "SIGTRAP",
                                  "SIGABRT (abort, often a failed assert)", "SIGBUS (bad memory access)",
                                  "SIGFPE (math error, like division by zero)", "SIGKILL", "SIGUSR1",
                                  "SIGSEGV (segmentation fault: bad memory access)", "SIGUSR2", "SIGPIPE",
                                  "SIGALRM", "SIGTERM"};
    char how[200];
    if (j->cancel)
        snprintf(how, sizeof(how), "stopped (cancelled)");
    else if (timed_out)
        snprintf(how, sizeof(how), "stopped after the time limit of %d s", r->timeout_s);
    else if (r->stop_on_match && matched)
        snprintf(how, sizeof(how), "stopped when its output matched");
    else if (crashed)
        snprintf(how, sizeof(how), "CRASHED with %s", sig < 16 ? names[sig] : "a signal");
    else if (rc >= 0)
        snprintf(how, sizeof(how), "exited with code %d", rc);
    else
        snprintf(how, sizeof(how), "ended (exit code unknown)");
    result_textf(res, "%s: %s after %.1f s. Output saved in %s/output.log.%s%s%s%s%s%s\nLast lines:\n%s", r->what,
                 how, secs, r->dir, shots ? " Screenshots in that folder." : "",
                 crashed ? " last_frame.png shows the screen about a second before the crash." : "",
                 matched ? "\nMatches:" : "", matches, cores[0] ? "\nCore dump (open it with gdb and the program): " : "",
                 cores, tail[0] ? tail : "(no output)");
    activity("Run ended: %s, %s", r->what, how);
    unlink(RUN_PID);
    unlink(RUN_RC);
    if (r->reopen && !getenv("CA_TEST_RUN")) {
        if (reopen_app()) {
            activity("Reopened Claude Agent");
            result_textf(res, "The Claude Agent app is open again on the Miyoo, so the person can answer questions there.");
        }
        else
            result_textf(res, "Note: the Claude Agent app could not be reopened. If you need the person to confirm "
                              "something on the Miyoo, ask them to open it (Apps > Claude Agent).");
    }
    free(r);
    job_finish(j, res);
    return NULL;
}

cJSON *tool_run_app(cJSON *args)
{
    pthread_mutex_lock(&jobs_lock);
    job_t *busy = job_running("run");
    pthread_mutex_unlock(&jobs_lock);
    if (busy)
        return result_error("a program started by run_app is still running (job %d)", busy->id);
    const char *launch = arg_str(args, "launch", NULL), *command = arg_str(args, "command", NULL);
    const char *rom = arg_str(args, "rom", NULL);
    if (!launch == !command)
        return result_error("give either 'launch' (a launch script, like /mnt/SDCARD/Roms/PORTS/Game.sh) or "
                            "'command' (a shell command)");
    if (access(BG_FLAG, F_OK) != 0)
        return result_error("background_mode must be on first: the Claude Agent app has to close so the program "
                            "can use the screen");
    if (launch && access(launch, F_OK) != 0)
        return result_error("%s does not exist", launch);
    if (pid_of("MainUI") == 0 && !ui_running() && !getenv("CA_TEST_RUN"))
        return result_error("something else is on screen (a game or an app): run_app starts programs from the "
                            "main menu. Ask the user to go back to the main menu.");

    run_t *r = calloc(1, sizeof(*r));
    char stamp[32];
    stamp_now(stamp, sizeof(stamp));
    const char *name = launch ? strrchr(launch, '/') + 1 : "command";
    snprintf(r->dir, sizeof(r->dir), "%s/runs/%s-%.40s", g_home, stamp, name);
    for (char *c = r->dir + strlen(g_home) + 6; *c; c++)
        if (*c == ' ' || *c == '\'' || *c == '"')
            *c = '_';
    if (mkdirs(r->dir) != 0) {
        cJSON *e = result_error("cannot create %s", r->dir);
        free(r);
        return e;
    }
    snprintf(r->what, sizeof(r->what), "%s%s%s", launch ? launch : command, rom ? " " : "", rom ? rom : "");
    r->timeout_s = clampi((int)arg_num(args, "timeout_s", 0), 0, 7200);
    r->shot_every = clampi((int)arg_num(args, "shots_every_ms", 0), 0, 600000);
    if (r->shot_every && r->shot_every < 200)
        r->shot_every = 200;
    r->shot_start = clampi((int)arg_num(args, "shots_start_ms", 2000), 0, 3600000);
    r->shot_count = clampi((int)arg_num(args, "shots_count", 10), 0, 500);
    r->stop_on_match = arg_bool(args, "stop_on_match", 0);
    r->core = arg_bool(args, "core_dump", 1);
    cJSON *w = cJSON_GetObjectItem(args, "watch_for"), *it;
    cJSON_ArrayForEach(it, w)
    {
        if (cJSON_IsString(it) && r->nwatch < 8)
            snprintf(r->watch[r->nwatch++], 120, "%s", it->valuestring);
    }
    int mhz = (int)arg_num(args, "cpu_mhz", 0);

    /* the wrapper: same environment as OnionOS, output kept, exit code noted */
    char script[1100];
    snprintf(script, sizeof(script), "%s/run.sh", r->dir);
    FILE *f = fopen(script, "w");
    if (!f) {
        free(r);
        return result_error("cannot write %s", script);
    }
    char q1[1200], q2[1200];
    fprintf(f, "#!/bin/sh\n# written by Claude Agent run_app\nunlink %s 2>/dev/null\necho $$ > %s\n", RUN_RC, RUN_PID);
    if (r->core)
        fprintf(f, "ulimit -c unlimited\n");
    cJSON *env = cJSON_GetObjectItem(args, "env");
    cJSON_ArrayForEach(it, env)
    {
        if (cJSON_IsString(it) && it->string && strspn(it->string, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == strlen(it->string)) {
            sq(q1, sizeof(q1), it->valuestring);
            fprintf(f, "export %s=%s\n", it->string, q1);
        }
    }
    if (mhz >= 400 && mhz <= 1800)
        fprintf(f, "/mnt/SDCARD/.tmp_update/bin/cpuclock %d >/dev/null 2>&1\n", mhz);
    sq(q1, sizeof(q1), r->dir);
    if (launch) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", launch);
        *strrchr(dir, '/') = 0;
        sq(q2, sizeof(q2), dir);
        fprintf(f, "cd %s\n", q2);
        sq(q2, sizeof(q2), launch);
        if (rom) {
            char q3[1200];
            sq(q3, sizeof(q3), rom);
            fprintf(f, "sh %s %s > %s/output.log 2>&1\n", q2, q3, q1);
        }
        else
            fprintf(f, "sh %s > %s/output.log 2>&1\n", q2, q1);
    }
    else {
        sq(q2, sizeof(q2), command);
        fprintf(f, "cd /mnt/SDCARD\nsh -c %s > %s/output.log 2>&1\n", q2, q1);
    }
    fprintf(f, "echo $? > %s.tmp && mv %s.tmp %s\n", RUN_RC, RUN_RC, RUN_RC);
    fclose(f);
    chmod(script, 0755);

    if (r->core) { /* cores go to the run folder, for this run only */
        FILE *cp = fopen("/proc/sys/kernel/core_pattern", "r");
        if (cp) {
            if (!fgets(r->core_pattern_old, sizeof(r->core_pattern_old), cp))
                r->core_pattern_old[0] = 0;
            fclose(cp);
        }
        cp = fopen("/proc/sys/kernel/core_pattern", "w");
        if (cp) {
            fprintf(cp, "%s/core.%%e.%%p", r->dir);
            fclose(cp);
        }
    }

    /* the Claude Agent app goes away first, then MainUI hands over to the program, like keymon does */
    unlink(RUN_PID);
    unlink(RUN_RC);
    r->reopen = ui_running();
    if (r->reopen) {
        /* the app is "claude-agent ui": find it by its command line */
        DIR *d = opendir("/proc");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char p[64], c[256];
            snprintf(p, sizeof(p), "/proc/%s/cmdline", e->d_name);
            int n = read_text(p, c, sizeof(c));
            if (n > 0 && strstr(c, "claude-agent") && n > (int)strlen(c) + 1 && !strcmp(c + strlen(c) + 1, "ui"))
                kill(atoi(e->d_name), SIGTERM);
        }
        if (d)
            closedir(d);
        for (int i = 0; i < 100 && !pid_of("MainUI"); i++) /* back in the main menu */
            sleep_ms(100);
        sleep_ms(1500); /* let MainUI finish drawing */
    }
    char cmd[1300];
    snprintf(cmd, sizeof(cmd), "LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so sh %s", script);
    f = fopen("/tmp/cmd_to_run.sh.tmp", "w");
    if (f) {
        fputs(cmd, f);
        fclose(f);
        rename("/tmp/cmd_to_run.sh.tmp", "/tmp/cmd_to_run.sh");
    }
    sync();
    pid_t mp = pid_of("MainUI");
    if (mp)
        kill(mp, SIGKILL); /* what keymon does to start an app from the menu */
    else if (getenv("CA_TEST_RUN")) /* tests: start the wrapper directly */
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", script, (char *)NULL);
            _exit(127);
        }
    job_t *j = job_start("run", r->what, run_thread, r);
    if (!j)
        return result_error("out of memory");
    return job_wait(j, arg_bool(args, "wait", 1) ? JOB_WAIT_MS : 0);
}

/* ------------------------------------------------------------- perf_monitor */

typedef struct {
    int interval, duration_ms, start_ms;
    char proc[64];
    char csv[1100];
} perf_t;

typedef struct {
    double min, max, sum;
    int n;
} stat_t;

static void st_add(stat_t *s, double v)
{
    if (!s->n || v < s->min)
        s->min = v;
    if (!s->n || v > s->max)
        s->max = v;
    s->sum += v;
    s->n++;
}

static int cpu_times(unsigned long long *busy, unsigned long long *total)
{
    char b[512];
    if (read_text("/proc/stat", b, sizeof(b)) <= 0)
        return -1;
    unsigned long long u, n, s, i, w, q, sq2, st;
    if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &q, &sq2, &st) < 4)
        return -1;
    *total = u + n + s + i + w + q + sq2 + st;
    *busy = *total - i - w;
    return 0;
}

static int proc_sample(pid_t pid, unsigned long long *ticks, long *rss_kb)
{
    char p[64], b[1024];
    snprintf(p, sizeof(p), "/proc/%d/stat", pid);
    if (read_text(p, b, sizeof(b)) <= 0)
        return -1;
    char *r = strrchr(b, ')');
    unsigned long ut = 0, stt = 0;
    long rss = 0;
    if (!r || sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                     &ut, &stt, &rss) != 3)
        return -1;
    *ticks = ut + stt;
    *rss_kb = rss * (sysconf(_SC_PAGESIZE) / 1024);
    return 0;
}

static void *perf_thread(void *arg)
{
    job_t *j = arg;
    perf_t *pf = j->arg;
    FILE *csv = fopen(pf->csv, "w");
    if (csv)
        fprintf(csv, "ms,cpu_percent,cpu_mhz,mem_free_mb,battery,proc_cpu_percent,proc_mem_mb\n");
    for (long long until = now_ms() + pf->start_ms; now_ms() < until && !j->cancel;)
        sleep_ms(100);
    stat_t cpu = {0}, mhz = {0}, mem = {0}, pcpu = {0}, pmem = {0};
    double bat0 = -1, bat1 = -1;
    unsigned long long b0, t0, pt0 = 0;
    cpu_times(&b0, &t0);
    long long start = now_ms(), last = start;
    pid_t pid = pf->proc[0] ? pid_of(pf->proc) : 0;
    long rss;
    if (pid)
        proc_sample(pid, &pt0, &rss);
    long hz = sysconf(_SC_CLK_TCK);
    while (!j->cancel && now_ms() - start < pf->duration_ms) {
        sleep_ms(pf->interval);
        long long now = now_ms();
        unsigned long long b1, t1;
        char b[256];
        double c = -1, f = -1, m = -1, bt = -1, pc = -1, pm = -1;
        if (cpu_times(&b1, &t1) == 0 && t1 > t0) {
            c = 100.0 * (b1 - b0) / (t1 - t0);
            b0 = b1;
            t0 = t1;
            st_add(&cpu, c);
        }
        int cm = cpu_mhz_real();
        if (cm > 0) {
            f = cm;
            st_add(&mhz, f);
        }
        if (read_text("/proc/meminfo", b, sizeof(b)) > 0) {
            char *a = strstr(b, "MemAvailable:");
            if (!a)
                a = strstr(b, "MemFree:");
            if (a) {
                m = atol(strchr(a, ':') + 1) / 1024.0;
                st_add(&mem, m);
            }
        }
        if (read_text("/tmp/percBat", b, sizeof(b)) > 0) {
            bt = atoi(b);
            if (bat0 < 0)
                bat0 = bt;
            bat1 = bt;
        }
        if (pf->proc[0]) {
            if (!pid && (pid = pid_of(pf->proc)))
                proc_sample(pid, &pt0, &rss);
            unsigned long long pt;
            if (pid && proc_sample(pid, &pt, &rss) == 0) {
                pc = 100.0 * (pt - pt0) / hz / ((now - last) / 1000.0);
                pt0 = pt;
                pm = rss / 1024.0;
                st_add(&pcpu, pc);
                st_add(&pmem, pm);
            }
            else
                pid = 0;
        }
        last = now;
        if (csv)
            fprintf(csv, "%lld,%.1f,%.0f,%.1f,%.0f,%.1f,%.1f\n", now - start, c, f, m, bt, pc, pm);
        job_progress(j, "%lld of %d s, CPU %.0f%% at %.0f MHz", (now - start) / 1000, pf->duration_ms / 1000, c, f);
    }
    if (csv)
        fclose(csv);
    cJSON *r = result_new();
#define AVG(s) ((s).n ? (s).sum / (s).n : 0)
    char procline[300] = "";
    if (pf->proc[0] && pcpu.n)
        snprintf(procline, sizeof(procline),
                 "\n%s: CPU %.0f%% average, %.0f%% max (100%% = the whole CPU); memory %.1f-%.1f MB%s", pf->proc,
                 AVG(pcpu), pcpu.max, pmem.min, pmem.max,
                 pmem.max - pmem.min > 5 ? " (it grew: a memory leak?)" : "");
    else if (pf->proc[0])
        snprintf(procline, sizeof(procline), "\n%s: not running while measuring", pf->proc);
    result_textf(r,
                 "Measured %d samples over %.0f s (every %d ms).\nCPU use: %.0f%% average, %.0f%% max. "
                 "Clock: %.0f-%.0f MHz (average %.0f).\nFree memory: %.0f-%.0f MB.%s\nAll samples (CSV): %s",
                 cpu.n, (now_ms() - start) / 1000.0, pf->interval, AVG(cpu), cpu.max, mhz.min, mhz.max, AVG(mhz),
                 mem.min, mem.max, procline, pf->csv);
    if (bat0 >= 0) {
        char bl[100];
        snprintf(bl, sizeof(bl), "\nBattery: %.0f%% -> %.0f%%", bat0, bat1);
        result_text(r, bl);
    }
    free(pf);
    job_finish(j, r);
    return NULL;
}

cJSON *tool_perf_monitor(cJSON *args)
{
    perf_t *pf = calloc(1, sizeof(*pf));
    pf->interval = clampi((int)arg_num(args, "interval_ms", 1000), 100, 60000);
    pf->duration_ms = clampi((int)arg_num(args, "duration_s", 60), 1, 7200) * 1000;
    pf->start_ms = clampi((int)arg_num(args, "start_after_ms", 0), 0, 3600000);
    snprintf(pf->proc, sizeof(pf->proc), "%s", arg_str(args, "process", ""));
    char stamp[32], dir[1100];
    stamp_now(stamp, sizeof(stamp));
    snprintf(dir, sizeof(dir), "%s/perf", g_home);
    mkdirs(dir);
    snprintf(pf->csv, sizeof(pf->csv), "%s/%s.csv", dir, stamp);
    char what[200];
    snprintf(what, sizeof(what), "performance for %d s%s%s", pf->duration_ms / 1000, pf->proc[0] ? " of " : "",
             pf->proc);
    job_t *j = job_start("perf", what, perf_thread, pf);
    if (!j)
        return result_error("out of memory");
    return job_wait(j, arg_bool(args, "wait", 1) ? JOB_WAIT_MS : 0);
}

/* ---------------------------------------------------------- screen_capture */

typedef struct {
    int every, start, count, duration_ms, scale;
    char dir[1100];
} cap_t;

static void *cap_thread(void *arg)
{
    job_t *j = arg;
    cap_t *c = j->arg;
    long long t0 = now_ms(), next = t0 + c->start;
    int n = 0;
    while (!j->cancel && n < c->count && (!c->duration_ms || now_ms() - t0 < c->start + c->duration_ms)) {
        long long now = now_ms();
        if (now >= next) {
            char p[1300];
            snprintf(p, sizeof(p), "%s/%06lld.png", c->dir, now - t0);
            if (shot_async(p, c->scale) == 0)
                n++;
            next += c->every;
            job_progress(j, "%d of %d screenshots", n, c->count);
        }
        sleep_ms(next - now_ms() > 20 ? 20 : 1);
    }
    sleep_ms(800);
    cJSON *r = result_new();
    result_textf(r, "%d screenshots in %s (file names are the ms from the start). Look at them with view_image.", n,
                 c->dir);
    free(c);
    job_finish(j, r);
    return NULL;
}

cJSON *tool_screen_capture(cJSON *args)
{
    cap_t *c = calloc(1, sizeof(*c));
    c->every = clampi((int)arg_num(args, "every_ms", 1000), 100, 600000);
    c->start = clampi((int)arg_num(args, "start_after_ms", 0), 0, 3600000);
    c->count = clampi((int)arg_num(args, "count", 10), 1, 500);
    c->duration_ms = clampi((int)arg_num(args, "duration_s", 0), 0, 7200) * 1000;
    c->scale = arg_num(args, "scale", 1) == 2 ? 2 : 1;
    const char *dir = arg_str(args, "dir", NULL);
    char stamp[32];
    stamp_now(stamp, sizeof(stamp));
    if (dir)
        snprintf(c->dir, sizeof(c->dir), "%s", dir);
    else
        snprintf(c->dir, sizeof(c->dir), "%s/shots/capture-%s", g_home, stamp);
    if (mkdirs(c->dir) != 0) {
        free(c);
        return result_error("cannot create %s", dir);
    }
    char what[200];
    snprintf(what, sizeof(what), "%d screenshots every %d ms", c->count, c->every);
    job_t *j = job_start("capture", what, cap_thread, c);
    if (!j)
        return result_error("out of memory");
    return job_wait(j, arg_bool(args, "wait", 1) ? JOB_WAIT_MS : 0);
}

/* -------------------------------------------------------------- watch_file */

typedef struct {
    char path[1024];
    char pat[8][120];
    int npat, duration_ms, stop_on_match, from_start;
} watch_t;

static void *watch_thread(void *arg)
{
    job_t *j = arg;
    watch_t *w = j->arg;
    long long t0 = now_ms();
    FILE *f = NULL;
    char line[1024], hits[4000] = "";
    size_t hl = 0;
    int n = 0;
    long pos = -1;
    while (!j->cancel && now_ms() - t0 < w->duration_ms && !(w->stop_on_match && n)) {
        if (!f) {
            f = fopen(w->path, "r");
            if (f && !w->from_start && pos < 0)
                fseek(f, 0, SEEK_END);
        }
        if (f) {
            struct stat st;
            if (stat(w->path, &st) == 0 && st.st_size < ftell(f)) /* the file started over */
                fseek(f, 0, SEEK_SET);
            clearerr(f);
            while (fgets(line, sizeof(line), f)) {
                int hit = !w->npat;
                for (int i = 0; i < w->npat && !hit; i++)
                    hit = strstr(line, w->pat[i]) != NULL;
                if (hit) {
                    n++;
                    line[strcspn(line, "\n")] = 0;
                    if (hl < sizeof(hits) - 200)
                        hl += snprintf(hits + hl, sizeof(hits) - hl, "\n  %.1f s: %.180s", (now_ms() - t0) / 1000.0,
                                       line);
                }
            }
            pos = ftell(f);
        }
        job_progress(j, "%lld s, %d matching lines", (now_ms() - t0) / 1000, n);
        sleep_ms(200);
    }
    if (f)
        fclose(f);
    cJSON *r = result_new();
    result_textf(r, "Watched %s for %.0f s: %d matching lines.%s", w->path, (now_ms() - t0) / 1000.0, n,
                 n ? hits : "");
    free(w);
    job_finish(j, r);
    return NULL;
}

cJSON *tool_watch_file(cJSON *args)
{
    watch_t *w = calloc(1, sizeof(*w));
    const char *path = arg_str(args, "path", NULL);
    if (!path) {
        free(w);
        return result_error("missing 'path'");
    }
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->duration_ms = clampi((int)arg_num(args, "duration_s", 60), 1, 7200) * 1000;
    w->stop_on_match = arg_bool(args, "stop_on_match", 0);
    w->from_start = arg_bool(args, "from_start", 0);
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(args, "patterns"))
    {
        if (cJSON_IsString(it) && w->npat < 8)
            snprintf(w->pat[w->npat++], 120, "%s", it->valuestring);
    }
    char what[1200];
    snprintf(what, sizeof(what), "watch %.200s", path);
    job_t *j = job_start("watch", what, watch_thread, w);
    if (!j)
        return result_error("out of memory");
    return job_wait(j, arg_bool(args, "wait", 1) ? JOB_WAIT_MS : 0);
}

/* ---------------------------------------------------------------- snapshot */

cJSON *tool_snapshot(cJSON *args)
{
    const char *action = arg_str(args, "action", "save");
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/backups/snapshots", g_home);
    mkdirs(dir);
    char cmd[3000], q[1300], out[4000] = "";
    if (!strcmp(action, "list")) {
        snprintf(cmd, sizeof(cmd), "cd '%s' && ls -l *.tar.gz 2>/dev/null; cat *.txt 2>/dev/null", dir);
        FILE *p = popen(cmd, "r");
        size_t n = p ? fread(out, 1, sizeof(out) - 1, p) : 0;
        if (p)
            pclose(p);
        out[n] = 0;
        cJSON *r = result_new();
        result_textf(r, "Snapshots in %s:\n%s", dir, n ? out : "(none)");
        return r;
    }
    const char *path = arg_str(args, "path", NULL), *name = arg_str(args, "name", NULL);
    if (!strcmp(action, "save")) {
        if (!path || path[0] != '/' || strncmp(path, "/mnt/SDCARD/", 12))
            return result_error("'path' must be a folder or file inside /mnt/SDCARD");
        struct stat st;
        if (stat(path, &st) != 0)
            return result_error("%s does not exist", path);
        char stamp[32];
        stamp_now(stamp, sizeof(stamp));
        const char *base = strrchr(path, '/') + 1;
        char file[1300];
        snprintf(file, sizeof(file), "%s/%s-%.60s.tar.gz", dir, stamp, base);
        for (char *c = file + strlen(dir) + 1; *c; c++)
            if (*c == ' ' || *c == '\'')
                *c = '_';
        sq(q, sizeof(q), path + 1);
        snprintf(cmd, sizeof(cmd), "tar -czf '%s' -C / %s 2>&1", file, q);
        FILE *p = popen(cmd, "r");
        size_t n = p ? fread(out, 1, sizeof(out) - 1, p) : 0;
        int rc = p ? pclose(p) : -1;
        out[n] = 0;
        if (rc != 0)
            return result_error("could not save the snapshot: %s", out);
        snprintf(cmd, sizeof(cmd), "%s.txt", file);
        FILE *m = fopen(cmd, "w");
        if (m) {
            fprintf(m, "%s  %s\n", strrchr(file, '/') + 1, path);
            fclose(m);
        }
        st.st_size = 0;
        stat(file, &st);
        cJSON *r = result_new();
        result_textf(r, "Saved %s in %s (%.1f MB). Restore it with snapshot {\"action\":\"restore\",\"name\":\"%s\"}.",
                     path, file, st.st_size / 1048576.0, strrchr(file, '/') + 1);
        activity("Snapshot of %s", path);
        return r;
    }
    if (!strcmp(action, "restore")) {
        if (!name || strchr(name, '/') || strchr(name, '\''))
            return result_error("'name' must be the snapshot's file name (snapshot list shows them)");
        char file[1300], meta[1400], orig[1100] = "";
        snprintf(file, sizeof(file), "%s/%s", dir, name);
        snprintf(meta, sizeof(meta), "%s.txt", file);
        FILE *m = fopen(meta, "r");
        if (m) {
            char line[1300];
            if (fgets(line, sizeof(line), m)) {
                char *s2 = strstr(line, "  /");
                if (s2)
                    snprintf(orig, sizeof(orig), "%.*s", (int)strcspn(s2 + 2, "\n"), s2 + 2);
            }
            fclose(m);
        }
        if (!orig[0] || access(file, F_OK) != 0)
            return result_error("no snapshot named %s", name);
        /* what is there now goes to the backups, then the snapshot comes back */
        char stamp[32], aside[1300];
        stamp_now(stamp, sizeof(stamp));
        snprintf(aside, sizeof(aside), "%s/backups/%s-before-restore%s", g_home, stamp, orig);
        char adir[1300];
        snprintf(adir, sizeof(adir), "%s", aside);
        *strrchr(adir, '/') = 0;
        mkdirs(adir);
        if (access(orig, F_OK) == 0 && rename(orig, aside) != 0)
            return result_error("could not move the current %s aside: %s", orig, strerror(errno));
        snprintf(cmd, sizeof(cmd), "tar -xzf '%s' -C / 2>&1 && sync", file);
        FILE *p = popen(cmd, "r");
        size_t n = p ? fread(out, 1, sizeof(out) - 1, p) : 0;
        int rc = p ? pclose(p) : -1;
        out[n] = 0;
        if (rc != 0) {
            rename(aside, orig);
            return result_error("could not restore: %s. Nothing changed.", out);
        }
        cJSON *r = result_new();
        result_textf(r, "Restored %s from %s. What was there before is in %s.",
                     orig, name, aside);
        activity("Restored %s from a snapshot", orig);
        return r;
    }
    return result_error("'action' must be save, restore or list");
}
