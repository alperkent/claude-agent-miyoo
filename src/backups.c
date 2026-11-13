/*
 * Claude Agent: the backups folder's housekeeping (its size, emptying it, deleting old copies).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"


static long long sum_kb;
static int add_size(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)path, (void)flag, (void)ftw;
    sum_kb += (long long)st->st_blocks / 2;
    return 0;
}

/* what the backups folder takes on the SD card, in KB */
long long backups_kb(void)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    char d[PATH_MAX];
    snprintf(d, sizeof(d), "%s/backups", g_home);
    pthread_mutex_lock(&m);
    sum_kb = 0;
    nftw(d, add_size, 16, FTW_PHYS);
    long long kb = sum_kb;
    pthread_mutex_unlock(&m);
    return kb;
}

static int rm_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)ftw;
    if (flag == FTW_DP)
        rmdir(path);
    else
        unlink(path);
    return 0;
}

static void rm_tree(const char *path)
{
    nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

/* the undo list keeps only changes that can still be undone */
static void prune_changes(void)
{
    char lp[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(lp, sizeof(lp), "%s/backups/changes.log", g_home);
    snprintf(tmp, sizeof(tmp), "%s.tmp", lp);
    FILE *in = fopen(lp, "r");
    if (!in)
        return;
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return;
    }
    char line[2 * PATH_MAX + 128];
    while (fgets(line, sizeof(line), in)) {
        /* id time action path backup undone: field 5 is the backup */
        char copy[sizeof(line)];
        snprintf(copy, sizeof(copy), "%s", line);
        char *f = copy;
        for (int k = 0; k < 4 && f; k++) {
            f = strchr(f, '\t');
            if (f)
                f++;
        }
        if (f) {
            char *end = strchr(f, '\t');
            if (end)
                *end = 0;
            if (*f && access(f, F_OK) != 0)
                continue; /* its copy is gone */
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    rename(tmp, lp);
}

/* deletes every copy for good; returns the KB freed */
long long backups_empty(void)
{
    long long before = backups_kb();
    char d[PATH_MAX];
    snprintf(d, sizeof(d), "%s/backups", g_home);
    DIR *dir = opendir(d);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "changes.log"))
            continue;
        char p[PATH_MAX + 300];
        snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
        rm_tree(p);
    }
    if (dir)
        closedir(dir);
    prune_changes();
    sync();
    long long after = backups_kb();
    return before > after ? before - after : 0;
}

/* deletes the copies older than days (by the folder's time); 0 = keep everything */
void backups_prune(int days)
{
    if (days <= 0)
        return;
    time_t limit = time(NULL) - (time_t)days * 86400;
    if (limit < 1700000000) /* the clock is not set: do not guess */
        return;
    int removed = 0;
    char d[PATH_MAX];
    const char *subs[] = {"", "/snapshots"};
    for (int s = 0; s < 2; s++) {
        snprintf(d, sizeof(d), "%s/backups%s", g_home, subs[s]);
        DIR *dir = opendir(d);
        struct dirent *e;
        while (dir && (e = readdir(dir))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "changes.log") || !strcmp(e->d_name, "snapshots"))
                continue;
            char p[PATH_MAX + 300];
            snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
            struct stat st;
            if (lstat(p, &st) == 0 && st.st_mtime < limit) {
                rm_tree(p);
                removed++;
            }
        }
        if (dir)
            closedir(dir);
    }
    if (removed) {
        prune_changes();
        activity("Old backups deleted: %d", removed);
    }
}
