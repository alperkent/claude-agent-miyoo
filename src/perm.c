/*
 * Claude Agent: permissions, settings and the logs shown on the app screen.
 *
 * Each tool belongs to a permission category. A category is allowed when:
 *   - the settings say "always allow" (config/settings.json), or
 *   - the person allowed it on the app screen since the app was opened
 *     (a file /tmp/claude_agent_ok_<category>, cleared when the app closes
 *     and when the device restarts).
 * Otherwise the agent writes the question to /tmp/claude_agent_ask, the app
 * screen shows it, and the answer comes back in /tmp/claude_agent_answer.
 * With the app closed (background mode) nobody can answer: the call is denied.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

const char *const perm_keys[PERM_COUNT] = {"screen", "read", "write", "control", "command"};
const char *const perm_titles[PERM_COUNT] = {"See the screen", "Read files", "Change and delete files",
                                             "Press buttons", "Run commands"};
const char *const perm_texts[PERM_COUNT] = {
    "Claude wants to take pictures of what is on the screen.",
    "Claude wants to read files and folders on the SD card.",
    "Claude wants to create, change or delete files. A copy of every changed or deleted file is kept in "
    "the app's backups folder.",
    "Claude wants to press the buttons of this Miyoo, as if you did, or record the buttons you press to "
    "replay them later.",
    "Claude wants to run system commands. Commands can do anything on this Miyoo, including what the other "
    "permissions control.",
};

/* tool -> category (-1: always allowed) */
static int tool_category(const char *tool)
{
    if (!strcmp(tool, "screenshot") || !strcmp(tool, "screen_capture"))
        return 0;
    if (!strcmp(tool, "read_file") || !strcmp(tool, "list_dir") || !strcmp(tool, "view_image") ||
        !strcmp(tool, "watch_file") || !strcmp(tool, "perf_monitor"))
        return 1;
    if (!strcmp(tool, "write_file") || !strcmp(tool, "delete_path") || !strcmp(tool, "download_file") ||
        !strcmp(tool, "snapshot"))
        return 2;
    if (!strcmp(tool, "press_buttons") || !strcmp(tool, "input_record") || !strcmp(tool, "input_play"))
        return 3;
    if (!strcmp(tool, "run_command") || !strcmp(tool, "run_app"))
        return 4;
    return -1;
}

/* ------------------------------------------------------------ settings */

static pthread_mutex_t set_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *settings_load(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/config/settings.json", g_home);
    FILE *f = fopen(path, "rb");
    cJSON *j = NULL;
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        j = cJSON_Parse(buf);
    }
    return j ? j : cJSON_CreateObject();
}

int setting_get(const char *key)
{
    pthread_mutex_lock(&set_lock);
    cJSON *j = settings_load();
    int v = cJSON_IsTrue(cJSON_GetObjectItem(j, key));
    cJSON_Delete(j);
    pthread_mutex_unlock(&set_lock);
    return v;
}

/* numeric settings (modes 0/1/2); def when missing */
int setting_num(const char *key, int def)
{
    pthread_mutex_lock(&set_lock);
    cJSON *j = settings_load();
    cJSON *v = cJSON_GetObjectItem(j, key);
    int r = cJSON_IsNumber(v) ? v->valueint : def;
    cJSON_Delete(j);
    pthread_mutex_unlock(&set_lock);
    return r;
}

static void setting_put(const char *key, cJSON *val);

void setting_set_num(const char *key, int value)
{
    setting_put(key, cJSON_CreateNumber(value));
}

/* how a permission asks: 0 every time, 1 once after the app opens (default), 2 never */
int perm_mode(int cat)
{
    char key[32];
    snprintf(key, sizeof(key), "perm_%s", perm_keys[cat]);
    int v = setting_num(key, -1);
    if (v < 0) { /* settings from 0.4/0.5 */
        snprintf(key, sizeof(key), "always_%s", perm_keys[cat]);
        v = setting_get(key) ? 2 : 1;
    }
    return v < 0 || v > 2 ? 1 : v;
}

void setting_set(const char *key, int value)
{
    setting_put(key, cJSON_CreateBool(value));
}

static void setting_put(const char *key, cJSON *val)
{
    pthread_mutex_lock(&set_lock);
    cJSON *j = settings_load();
    cJSON_DeleteItemFromObject(j, key);
    cJSON_AddItemToObject(j, key, val);
    char dir[PATH_MAX], path[PATH_MAX + 32], tmp[PATH_MAX + 40];
    snprintf(dir, sizeof(dir), "%s/config", g_home);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char *s = cJSON_Print(j);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
        rename(tmp, path);
    }
    free(s);
    cJSON_Delete(j);
    pthread_mutex_unlock(&set_lock);
}

/* ---------------------------------------------------------------- logs */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* keeps a log file short: when it passes max bytes, only the newer half stays */
static void trim_file(const char *path, long max)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= max)
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    long keep = max / 2;
    fseek(f, -keep, SEEK_END);
    char *buf = malloc(keep + 1);
    size_t n = fread(buf, 1, keep, f);
    fclose(f);
    buf[n] = 0;
    char *start = strchr(buf, '\n');
    start = start ? start + 1 : buf;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f) {
        fwrite(start, 1, n - (start - buf), f);
        fclose(f);
        rename(tmp, path);
    }
    free(buf);
}

static void append_line(const char *name, const char *line)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", g_home, name);
    pthread_mutex_lock(&log_lock);
    FILE *f = fopen(path, "a");
    if (f) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(f, "%02d:%02d:%02d\t%s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
        fclose(f);
    }
    trim_file(path, 48 * 1024);
    pthread_mutex_unlock(&log_lock);
}

/* one line per entry: newlines inside become \v */
static void one_line(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '\n')
            *p = '\v';
        else if ((unsigned char)*p < 32 && *p != '\v')
            *p = ' ';
    }
}

void activity(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), T(fmt), ap); /* shown on the Miyoo: in its language */
    va_end(ap);
    one_line(line);
    append_line("activity.log", line);
}

void messages_add(const char *text)
{
    char *s = strdup(text);
    if (!s)
        return;
    if (strlen(s) > 4000)
        strcpy(s + 3990, "...");
    one_line(s);
    append_line("messages.log", s);
    free(s);
}

static const char *arg(cJSON *args, const char *name)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(args, name));
    return v ? v : "";
}

void activity_call(const char *tool, cJSON *args)
{
    if (!strcmp(tool, "device_info"))
        activity("Checking the device");
    else if (!strcmp(tool, "screenshot"))
        activity("Taking a screenshot");
    else if (!strcmp(tool, "press_buttons"))
        activity("Pressing: %s", arg(args, "sequence"));
    else if (!strcmp(tool, "run_command"))
        activity("$ %.200s", arg(args, "command"));
    else if (!strcmp(tool, "read_file"))
        activity("Reading %s", arg(args, "path"));
    else if (!strcmp(tool, "write_file"))
        activity("Writing %s", arg(args, "path"));
    else if (!strcmp(tool, "delete_path"))
        activity("Deleting %s", arg(args, "path"));
    else if (!strcmp(tool, "list_dir"))
        activity("Listing %s", *arg(args, "path") ? arg(args, "path") : "/mnt/SDCARD");
    else if (!strcmp(tool, "view_image"))
        activity("Looking at %s", arg(args, "path"));
    else if (!strcmp(tool, "input_play"))
        activity("Replaying buttons%s%s", *arg(args, "recording") ? ": " : T(" (timeline)"), arg(args, "recording"));
    else if (!strcmp(tool, "background_mode"))
        activity("Background mode: %s", T(cJSON_IsFalse(cJSON_GetObjectItem(args, "enable")) ? "turn off" : "turn on"));
}

void activity_result(const char *tool, cJSON *result)
{
    if (!strcmp(tool, "show_message"))
        return;
    if (cJSON_IsTrue(cJSON_GetObjectItem(result, "isError"))) {
        cJSON *c = cJSON_GetArrayItem(cJSON_GetObjectItem(result, "content"), 0);
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(c, "text"));
        activity("  failed: %.160s", t ? t : "");
    }
    else if (!strcmp(tool, "run_command")) {
        cJSON *content = cJSON_GetObjectItem(result, "content");
        cJSON *last = cJSON_GetArrayItem(content, cJSON_GetArraySize(content) - 1);
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(last, "text"));
        if (t)
            activity("  %s", t);
    }
}

/* --------------------------------------------------------- permissions */

/* is the app screen ("claude-agent ui") open? */
int ui_running(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char p[64], cmd[256];
        snprintf(p, sizeof(p), "/proc/%s/cmdline", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0)
            continue;
        cmd[n] = 0;
        const char *base = strrchr(cmd, '/');
        base = base ? base + 1 : cmd;
        size_t bl = strlen(cmd);
        if (!strcmp(base, "claude-agent") && (size_t)n > bl + 1 && !strcmp(cmd + bl + 1, "ui"))
            found = 1;
    }
    closedir(d);
    return found;
}

void perm_clear_session(void)
{
    char path[64];
    for (int i = 0; i < PERM_COUNT; i++) {
        snprintf(path, sizeof(path), "%s%s", PERM_GRANT, perm_keys[i]);
        unlink(path);
    }
    unlink(DEV_SESSION); /* background "until restart" (BG_AUTO_SESSION) stays: /tmp empties at restart */
}

static pthread_mutex_t ask_lock = PTHREAD_MUTEX_INITIALIZER;

/* Asks the person on the app screen (one question at a time). key: what a "yes" is remembered as
 * (PERM_GRANT + key) when remember is set. title/text: the question. 1 = yes. */
int perm_ask(const char *key, const char *title, const char *text, const char *detail, int remember, char *why,
             size_t why_size)
{
    char grant[160];
    snprintf(grant, sizeof(grant), "%s%s", PERM_GRANT, key);
    if (!ui_running()) {
        snprintf(why, why_size,
                 "\"%s\" needs the person's OK, and nobody can answer now because the Claude Agent app is not on "
                 "screen. Ask the user to open the app, or to set this to \"Don't ask\" in the app's settings (START).",
                 title);
        return 0;
    }
    pthread_mutex_lock(&ask_lock);
    int ok = remember && access(grant, F_OK) == 0; /* allowed while we waited? */
    if (!ok) {
        unlink(PERM_ANSWER);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s.tmp", PERM_ASK);
        FILE *f = fopen(tmp, "w");
        if (f) {
            char d[300], t[1100];
            snprintf(d, sizeof(d), "%s", detail ? detail : "");
            one_line(d);
            snprintf(t, sizeof(t), "%s", text ? text : "");
            one_line(t);
            fprintf(f, "cat=%s\ntitle=%s\ntext=%s\ndetail=%s\nremember=%d\n", key, title, t, d, remember);
            fclose(f);
            rename(tmp, PERM_ASK);
        }
        char ans[16] = "";
        for (int i = 0; i < 900 && !ans[0]; i++) { /* 90 s */
            usleep(100000);
            FILE *a = fopen(PERM_ANSWER, "r");
            if (a) {
                if (!fgets(ans, sizeof(ans), a))
                    ans[0] = 0;
                fclose(a);
            }
            if (i % 20 == 19 && !ui_running())
                break;
        }
        unlink(PERM_ASK);
        unlink(PERM_ANSWER);
        ok = !strncmp(ans, "allow", 5);
        if (ok && remember) {
            FILE *g = fopen(grant, "w");
            if (g)
                fclose(g);
        }
        activity("%s: %s", title, T(ok ? "allowed" : (ans[0] ? "denied" : "no answer")));
        if (!ok)
            snprintf(why, why_size,
                     ans[0] ? "The user said no to \"%s\" on the device. Do not try again unless the user asks."
                            : "Nobody answered \"%s\" on the device within 90 s.",
                     title);
    }
    pthread_mutex_unlock(&ask_lock);
    return ok;
}

int perm_check(const char *tool, cJSON *args, char *why, size_t why_size)
{
    int cat = tool_category(tool);
    if (cat < 0)
        return 1;
    char grant[64];
    snprintf(grant, sizeof(grant), "%s%s", PERM_GRANT, perm_keys[cat]);
    int mode = perm_mode(cat);
    if (mode == 2 || (mode == 1 && access(grant, F_OK) == 0))
        return 1;
    const char *detail = arg(args, "path");
    if (!*detail)
        detail = arg(args, "command");
    if (!*detail)
        detail = arg(args, "sequence");
    if (!*detail)
        detail = arg(args, "launch");
    char title[80], text[600];
    snprintf(title, sizeof(title), T("Allow: %s?"), T(perm_titles[cat]));
    snprintf(text, sizeof(text), T("%s %s If you did not ask Claude for anything, press B."), T(perm_texts[cat]),
             T(mode == 1 ? "If you allow it, it stays allowed until you close this app."
                         : "You will be asked again next time."));
    return perm_ask(perm_keys[cat], title, text, detail, mode == 1, why, why_size);
}
