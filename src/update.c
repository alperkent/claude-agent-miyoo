/*
 * Claude Agent: updates from GitHub releases.
 *
 *   claude-agent update check [--auto]   looks at the latest release; writes UPDATE_STATUS
 *   claude-agent update install          downloads it, checks it, puts the new files in place
 *
 * --auto (started by the app): at most once per power-on when it worked, and at most once an hour
 * when it did not (no Wi-Fi, GitHub down), like the OoT port.
 * The release must have a .zip asset with the claude-agent program inside (any folder depth);
 * every file of that folder replaces the app's files. config/, logs, backups and recordings stay.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"
#include "mbedtls/sha256.h"

#ifndef UPDATE_REPO
#define UPDATE_REPO "cacuracaptors/claude-agent-miyoo"
#endif
#define UPDATE_OK_FLAG "/tmp/claude_agent_update_ok"     /* checked fine since power-on */
#define UPDATE_TRY_FLAG "/tmp/claude_agent_update_tried" /* time of the last try */

typedef struct {
    char *data;
    size_t len, cap;
} mem_t;

static int mem_sink(void *ctx, const unsigned char *d, size_t n)
{
    mem_t *m = ctx;
    if (m->len + n + 1 > m->cap) {
        size_t cap = (m->len + n + 1) * 2;
        if (cap > 4 * 1048576)
            return -1;
        char *p = realloc(m->data, cap);
        if (!p)
            return -1;
        m->data = p;
        m->cap = cap;
    }
    memcpy(m->data + m->len, d, n);
    m->len += n;
    m->data[m->len] = 0;
    return 0;
}

static void status_write(const char *state, const char *version, const char *url, const char *sha,
                         long long size, const char *note)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", UPDATE_STATUS);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "state=%s\nversion=%s\ncurrent=%s\nurl=%s\nsha=%s\nsize=%lld\nnote=%s\n", state,
            version ? version : "", AGENT_VERSION, url ? url : "", sha ? sha : "", size, note ? note : "");
    fclose(f);
    rename(tmp, UPDATE_STATUS);
}

/* "v1.2.3" > "1.2.0" ? */
static int version_newer(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V')
        a++;
    if (*b == 'v' || *b == 'V')
        b++;
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y)
            return x > y;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    return 0;
}

static void repo_name(char *out, size_t size)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/config/update_repo.txt", g_home);
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) {
        if (fgets(out, size, f))
            out[strcspn(out, " \r\n")] = 0;
        fclose(f);
    }
    if (!out[0])
        snprintf(out, size, "%s", UPDATE_REPO);
}

static int update_check(int automatic)
{
    if (automatic) {
        if (access(UPDATE_OK_FLAG, F_OK) == 0)
            return 0;
        struct stat st;
        if (stat(UPDATE_TRY_FLAG, &st) == 0 && time(NULL) - st.st_mtime < 3600)
            return 0;
    }
    FILE *t = fopen(UPDATE_TRY_FLAG, "w");
    if (t)
        fclose(t);
    status_write("checking", NULL, NULL, NULL, 0, NULL);

    char repo[200], url[400];
    repo_name(repo, sizeof(repo));
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", repo);
    if (getenv("CA_UPDATE_API")) /* tests */
        snprintf(url, sizeof(url), "%s", getenv("CA_UPDATE_API"));
    mem_t m = {0};
    long long total;
    char err[300];
    if (https_get(url, mem_sink, &m, &total, NULL, err, sizeof(err)) != 0) {
        free(m.data);
        status_write("error", NULL, NULL, NULL, 0,
                     strstr(err, "404") ? "No release published yet." : "Could not reach GitHub. Is Wi-Fi on?");
        return 1;
    }
    cJSON *j = cJSON_Parse(m.data ? m.data : "");
    free(m.data);
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tag_name"));
    if (!tag) {
        cJSON_Delete(j);
        status_write("error", NULL, NULL, NULL, 0, "GitHub gave an unexpected answer.");
        return 1;
    }
    FILE *ok = fopen(UPDATE_OK_FLAG, "w");
    if (ok)
        fclose(ok);
    if (!version_newer(tag, AGENT_VERSION)) {
        status_write("latest", tag, NULL, NULL, 0, NULL);
        cJSON_Delete(j);
        return 0;
    }
    const char *dl = NULL, *sha = "";
    long long size = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "assets"))
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
        if (name && strlen(name) > 4 && !strcasecmp(name + strlen(name) - 4, ".zip")) {
            dl = cJSON_GetStringValue(cJSON_GetObjectItem(a, "browser_download_url"));
            const char *dg = cJSON_GetStringValue(cJSON_GetObjectItem(a, "digest"));
            if (dg && !strncmp(dg, "sha256:", 7))
                sha = dg + 7;
            size = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(a, "size"));
            break;
        }
    }
    /* first line of the release notes */
    char note[200] = "";
    const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(j, "body"));
    if (body) {
        while (*body == '#' || *body == ' ' || *body == '\r' || *body == '\n')
            body++;
        snprintf(note, sizeof(note), "%.*s", (int)strcspn(body, "\r\n"), body);
    }
    if (dl)
        status_write("available", tag, dl, sha, size, note);
    else
        status_write("error", tag, NULL, NULL, 0, "The new version has no file to install yet.");
    cJSON_Delete(j);
    return 0;
}

/* --- install */

typedef struct {
    FILE *f;
    mbedtls_sha256_context sha;
    long long got;
} file_sink_t;

static int file_sink(void *ctx, const unsigned char *d, size_t n)
{
    file_sink_t *s = ctx;
    if (fwrite(d, 1, n, s->f) != n)
        return -1;
    mbedtls_sha256_update(&s->sha, d, n);
    s->got += n;
    return 0;
}

/* folder below root that holds a file named claude-agent */
static int find_app_dir(const char *root, char *out, size_t size, int depth)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/claude-agent", root);
    if (access(p, F_OK) == 0) {
        snprintf(out, size, "%s", root);
        return 0;
    }
    if (depth > 4)
        return -1;
    DIR *d = opendir(root);
    if (!d)
        return -1;
    struct dirent *e;
    int rc = -1;
    while (rc && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            rc = find_app_dir(p, out, size, depth + 1);
    }
    closedir(d);
    return rc;
}

static int run(const char *cmd)
{
    int r = system(cmd);
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

static int update_install(void)
{
    char state[32] = "", url[1200] = "", sha[80] = "", version[64] = "";
    FILE *f = fopen(UPDATE_STATUS, "r");
    if (f) {
        char line[1300];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "state=", 6))
                snprintf(state, sizeof(state), "%s", line + 6);
            else if (!strncmp(line, "url=", 4))
                snprintf(url, sizeof(url), "%s", line + 4);
            else if (!strncmp(line, "sha=", 4))
                snprintf(sha, sizeof(sha), "%s", line + 4);
            else if (!strncmp(line, "version=", 8))
                snprintf(version, sizeof(version), "%s", line + 8);
        }
        fclose(f);
    }
    if (strcmp(state, "available") || !url[0]) {
        fprintf(stderr, "no update to install (check first)\n");
        return 1;
    }
    status_write("installing", version, url, sha, 0, "Downloading...");
    char dir[PATH_MAX], zip[PATH_MAX + 16], ex[PATH_MAX + 16], cmd[3 * PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/update", g_home);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    run(cmd);
    mkdirs(dir);
    snprintf(zip, sizeof(zip), "%s/package.zip", dir);
    snprintf(ex, sizeof(ex), "%s/files", dir);
    file_sink_t s = {0};
    s.f = fopen(zip, "wb");
    if (!s.f) {
        status_write("failed", version, NULL, NULL, 0, "Could not write to the SD card.");
        return 1;
    }
    mbedtls_sha256_init(&s.sha);
    mbedtls_sha256_starts(&s.sha, 0);
    long long total;
    char err[300];
    int rc = https_get(url, file_sink, &s, &total, NULL, err, sizeof(err));
    fclose(s.f);
    unsigned char sum[32];
    mbedtls_sha256_finish(&s.sha, sum);
    mbedtls_sha256_free(&s.sha);
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", sum[i]);
    if (rc != 0) {
        status_write("failed", version, NULL, NULL, 0, "The download did not finish. Try again later.");
        return 1;
    }
    if (sha[0] && strcasecmp(sha, hex)) {
        status_write("failed", version, NULL, NULL, 0, "The download arrived damaged. Try again later.");
        return 1;
    }
    status_write("installing", version, url, sha, 0, "Installing...");
    mkdirs(ex);
    snprintf(cmd, sizeof(cmd), "unzip -o -q '%s' -d '%s' 2>/dev/null || /mnt/SDCARD/.tmp_update/bin/7z x -y -o'%s' '%s' >/dev/null", zip, ex, ex, zip);
    char app[PATH_MAX];
    if (run(cmd) != 0 || find_app_dir(ex, app, sizeof(app), 0) != 0) {
        status_write("failed", version, NULL, NULL, 0, "The update package is not valid.");
        return 1;
    }
    /* keep the running version, then copy the new files over the app folder */
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s/old' && cp -f '%s/claude-agent' '%s/old/' 2>/dev/null; "
             "cd '%s' && for f in $(find . -type f); do mkdir -p \"%s/$(dirname \"$f\")\"; "
             "cp -f \"$f\" \"%s/$f.new\" && mv -f \"%s/$f.new\" \"%s/$f\" || exit 1; done; chmod +x '%s/claude-agent'; sync",
             dir, g_home, dir, app, g_home, g_home, g_home, g_home, g_home);
    if (run(cmd) != 0) {
        /* put the old program back */
        snprintf(cmd, sizeof(cmd), "cp -f '%s/old/claude-agent' '%s/claude-agent'; sync", dir, g_home);
        run(cmd);
        status_write("failed", version, NULL, NULL, 0, "Could not copy the new files. Nothing changed.");
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/files' '%s'", dir, zip);
    run(cmd);
    status_write("installed", version, NULL, NULL, 0, NULL);
    activity("Updated Claude Agent to %s", version);
    return 0;
}

int update_main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "check"))
        return update_check(argc >= 4 && !strcmp(argv[3], "--auto"));
    if (argc >= 3 && !strcmp(argv[2], "install"))
        return update_install();
    fprintf(stderr, "claude-agent update check [--auto] | install\n");
    return 2;
}
