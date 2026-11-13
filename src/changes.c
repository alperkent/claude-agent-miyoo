/*
 * Claude Agent: the list of changes Claude made, and putting them back ("Undo changes").
 *
 * backups/changes.log, one change per line, tab separated:
 *   id  time  action  path  backup  undone
 * action: changed (backup = copy of the old file), created (no backup: undo removes the new file),
 *         deleted (backup = where the file or folder was moved).
 * Undoing keeps what is there now in the backups folder too, so an undo can be undone.
 */
#define _GNU_SOURCE
#include <errno.h>
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

static pthread_mutex_t ch_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_path(char *out, size_t size)
{
    snprintf(out, size, "%s/backups/changes.log", g_home);
}

/* the app's own files (messages, logs) are not changes worth undoing */
static int ignored(const char *path)
{
    size_t n = strlen(g_home);
    return !strncmp(path, "/tmp/", 5) || (!strncmp(path, g_home, n) && (path[n] == '/' || !path[n]));
}

void change_log(const char *action, const char *path, const char *backup)
{
    if (!path || ignored(path))
        return;
    char lp[PATH_MAX], dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/backups", g_home);
    mkdirs(dir);
    log_path(lp, sizeof(lp));
    pthread_mutex_lock(&ch_lock);
    int id = 0;
    FILE *f = fopen(lp, "r");
    if (f) {
        char line[2 * PATH_MAX + 128];
        while (fgets(line, sizeof(line), f))
            id = atoi(line) > id ? atoi(line) : id;
        fclose(f);
    }
    f = fopen(lp, "a");
    if (f) {
        time_t t = time(NULL);
        char when[32];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(f, "%d\t%s\t%s\t%s\t%s\t0\n", id + 1, when, action, path, backup ? backup : "");
        fclose(f);
    }
    pthread_mutex_unlock(&ch_lock);
}

/* newest first; caller frees */
int changes_load(change_t **out)
{
    *out = NULL;
    char lp[PATH_MAX];
    log_path(lp, sizeof(lp));
    pthread_mutex_lock(&ch_lock);
    FILE *f = fopen(lp, "r");
    int n = 0, cap = 0;
    change_t *v = NULL;
    if (f) {
        char line[2 * PATH_MAX + 128];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *fld[6] = {0};
            int k = 0;
            for (char *p = line, *tab; k < 6; k++) {
                fld[k] = p;
                tab = strchr(p, '\t');
                if (!tab) {
                    k++;
                    break;
                }
                *tab = 0;
                p = tab + 1;
            }
            if (k < 6 || !fld[3][0])
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                v = realloc(v, cap * sizeof(change_t));
            }
            change_t *c = &v[n++];
            memset(c, 0, sizeof(*c));
            c->id = atoi(fld[0]);
            snprintf(c->when, sizeof(c->when), "%s", fld[1]);
            snprintf(c->action, sizeof(c->action), "%s", fld[2]);
            snprintf(c->path, sizeof(c->path), "%s", fld[3]);
            snprintf(c->backup, sizeof(c->backup), "%s", fld[4]);
            c->undone = atoi(fld[5]);
        }
        fclose(f);
    }
    pthread_mutex_unlock(&ch_lock);
    for (int i = 0; i < n / 2; i++) { /* newest first */
        change_t t = v[i];
        v[i] = v[n - 1 - i];
        v[n - 1 - i] = t;
    }
    *out = v;
    return n;
}

static void mark_undone(int id)
{
    char lp[PATH_MAX], tmp[PATH_MAX + 8];
    log_path(lp, sizeof(lp));
    snprintf(tmp, sizeof(tmp), "%s.tmp", lp);
    pthread_mutex_lock(&ch_lock);
    FILE *in = fopen(lp, "r"), *out = fopen(tmp, "w");
    if (in && out) {
        char line[2 * PATH_MAX + 128];
        while (fgets(line, sizeof(line), in)) {
            size_t l = strlen(line);
            if (atoi(line) == id && l >= 2 && line[l - 2] == '0')
                line[l - 2] = '1';
            fputs(line, out);
        }
    }
    if (in)
        fclose(in);
    if (out) {
        fclose(out);
        rename(tmp, lp);
    }
    pthread_mutex_unlock(&ch_lock);
}

/* moves what is at path now into backups/<stamp>-undo/..., so the undo can be reversed */
static int keep_current(const char *path, char *where, size_t wsize, int id)
{
    where[0] = 0;
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    time_t t = time(NULL);
    char stamp[32], dst[PATH_MAX], dir[PATH_MAX];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
    snprintf(dst, sizeof(dst), "%s/backups/%s-undo%d%s", g_home, stamp, id, path);
    snprintf(dir, sizeof(dir), "%s", dst);
    *strrchr(dir, '/') = 0;
    if (mkdirs(dir) != 0 || rename(path, dst) != 0)
        return -1;
    snprintf(where, wsize, "%s", dst);
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return -1;
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.claude-tmp", to);
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[16384];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            rc = -1;
    fclose(in);
    if (fclose(out) != 0)
        rc = -1;
    if (rc == 0 && rename(tmp, to) != 0)
        rc = -1;
    if (rc)
        unlink(tmp);
    return rc;
}

int change_undo(int id, char *msg, size_t msize)
{
    change_t *v;
    int n = changes_load(&v), rc = -1;
    change_t *c = NULL;
    for (int i = 0; i < n; i++)
        if (v[i].id == id)
            c = &v[i];
    if (!c) {
        snprintf(msg, msize, "That change is not in the list any more.");
        free(v);
        return -1;
    }
    if (c->undone) {
        snprintf(msg, msize, "This change was already undone.");
        free(v);
        return -1;
    }
    char kept[PATH_MAX] = "", dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", c->path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        mkdirs(dir);
    }
    struct stat st;
    if (!strcmp(c->action, "created")) {
        if (keep_current(c->path, kept, sizeof(kept), id) != 0)
            snprintf(msg, msize, "Could not remove %s: %s", c->path, strerror(errno));
        else {
            snprintf(msg, msize, "Removed %s, which Claude had created.", c->path);
            rc = 0;
        }
    }
    else if (lstat(c->backup, &st) != 0)
        snprintf(msg, msize, "The saved copy is gone (%s).", c->backup);
    else if (keep_current(c->path, kept, sizeof(kept), id) != 0)
        snprintf(msg, msize, "Could not move the current %s aside: %s", c->path, strerror(errno));
    else if (!strcmp(c->action, "deleted")) {
        if (rename(c->backup, c->path) != 0)
            snprintf(msg, msize, "Could not bring back %s: %s", c->path, strerror(errno));
        else {
            snprintf(msg, msize, "Brought back %s.", c->path);
            rc = 0;
        }
    }
    else { /* changed: copy the old version back, the copy stays in the backups */
        if (copy_file(c->backup, c->path) != 0)
            snprintf(msg, msize, "Could not put back %s: %s", c->path, strerror(errno));
        else {
            chmod(c->path, st.st_mode & 07777);
            snprintf(msg, msize, "Put back %s as it was at %s.", c->path, c->when + 11);
            rc = 0;
        }
    }
    if (rc == 0) {
        sync();
        mark_undone(id);
        /* the undo is a change too: it can be reversed */
        if (!strcmp(c->action, "created"))
            change_log("deleted", c->path, kept);
        else
            change_log(kept[0] ? "changed" : "created", c->path, kept[0] ? kept : NULL);
        activity("Undo: %s", msg);
    }
    free(v);
    return rc;
}
