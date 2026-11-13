/*
 * Claude Agent: texts in the language set on the Miyoo (Settings > Language in OnionOS / RedOnion).
 *
 * The texts are written in English in the code. lang/<code>.txt next to the program has the
 * translations, as pairs of lines separated by a blank line:
 *
 *     # comment
 *     English text
 *     Translated text
 *
 * <code> is the name of the system language file without ".lang" (pt-BR, es, fr...). If there is no
 * file for it, the part before "-" is tried (pt-BR -> pt), then another variety of the same language
 * (pt -> pt-BR), then English is used. A translation whose
 * %s / %d placeholders do not match the English text exactly is ignored, so a typo cannot crash the app.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define SLOTS 2048 /* power of two, well above the number of texts */

static struct {
    char *en, *tr;
} table[SLOTS];
static char lang[24] = "en";
static pthread_once_t once = PTHREAD_ONCE_INIT;

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* the printf conversions of a text, in order ("%s%d"), to compare English and translation */
static void conversions(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 8 < size; s++) {
        if (*s != '%')
            continue;
        s++;
        if (*s == '%')
            continue;
        out[n++] = '%';
        while (*s && strchr("-+ #0123456789.*lhzjt", *s))
            out[n++] = *s++;
        if (!*s)
            break;
        out[n++] = *s;
    }
    out[n] = 0;
}

/* "\n" inside a text is written as \n in the file */
static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *d++ = '\n';
            s++;
        }
        else
            *d++ = *s;
    }
    *d = 0;
}

static void add(const char *en, const char *tr)
{
    char a[64], b[64];
    conversions(en, a, sizeof(a));
    conversions(tr, b, sizeof(b));
    if (strcmp(a, b)) {
        fprintf(stderr, "lang: placeholders differ, kept in English: %s\n", en);
        return;
    }
    uint32_t i = hash(en) & (SLOTS - 1);
    for (int k = 0; k < SLOTS; k++, i = (i + 1) & (SLOTS - 1)) {
        if (!table[i].en) {
            table[i].en = strdup(en);
            table[i].tr = strdup(tr);
            return;
        }
        if (!strcmp(table[i].en, en))
            return;
    }
}

static int load_file(const char *code)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/lang/%s.txt", g_home, code);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[2048], en[2048] = "";
    int have_en = 0, lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = 0;
        if (lineno == 1 && !strncmp(line, "\xef\xbb\xbf", 3)) /* saved by Notepad with a BOM */
            memmove(line, line + 3, strlen(line + 2));
        if (line[0] == '#')
            continue;
        if (!line[0]) {
            have_en = 0;
            continue;
        }
        unescape(line);
        if (!have_en) {
            snprintf(en, sizeof(en), "%s", line);
            have_en = 1;
        }
        else {
            add(en, line);
            have_en = 0;
        }
    }
    fclose(f);
    return 0;
}

static void load(void)
{
    char code[24] = "";
    const char *env = getenv("CA_LANG"); /* tests */
    if (env)
        snprintf(code, sizeof(code), "%s", env);
    else {
        FILE *f = fopen("/appconfigs/system.json", "r");
        if (f) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            char *k = strstr(buf, "\"language\"");
            if (k && (k = strchr(k + 10, '"'))) {
                k++;
                size_t l = strcspn(k, "\".");
                if (l < sizeof(code))
                    snprintf(code, sizeof(code), "%.*s", (int)l, k);
            }
        }
    }
    if (!code[0] || !strcmp(code, "en"))
        return;
    if (load_file(code) == 0) {
        snprintf(lang, sizeof(lang), "%s", code);
        return;
    }
    char base[24];
    snprintf(base, sizeof(base), "%.*s", (int)strcspn(code, "-_"), code);
    if (strcmp(base, code) && load_file(base) == 0) {
        snprintf(lang, sizeof(lang), "%s", base);
        return;
    }
    /* another variety of the same language (pt -> pt-BR) */
    char dir[PATH_MAX + 8];
    snprintf(dir, sizeof(dir), "%s/lang", g_home);
    DIR *d = opendir(dir);
    struct dirent *e;
    size_t bl = strlen(base);
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > bl + 5 && !strncmp(e->d_name, base, bl) && e->d_name[bl] == '-' && !strcmp(e->d_name + n - 4, ".txt")) {
            char other[24];
            snprintf(other, sizeof(other), "%.*s", (int)(n - 4), e->d_name);
            if (load_file(other) == 0) {
                snprintf(lang, sizeof(lang), "%s", other);
                break;
            }
        }
    }
    if (d)
        closedir(d);
}

const char *T(const char *en)
{
    if (!en || !*en)
        return en;
    pthread_once(&once, load);
    uint32_t i = hash(en) & (SLOTS - 1);
    for (int k = 0; k < SLOTS && table[i].en; k++, i = (i + 1) & (SLOTS - 1))
        if (!strcmp(table[i].en, en))
            return table[i].tr;
    return en;
}

const char *lang_code(void)
{
    pthread_once(&once, load);
    return lang;
}

/* the system language, even when the app has no translation for it (for Claude) */
void lang_system(char *out, size_t size)
{
    out[0] = 0;
    FILE *f = fopen("/appconfigs/system.json", "r");
    if (!f) {
        snprintf(out, size, "en");
        return;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    char *k = strstr(buf, "\"language\"");
    if (k && (k = strchr(k + 10, '"'))) {
        k++;
        snprintf(out, size, "%.*s", (int)strcspn(k, "\"."), k);
    }
    if (!out[0])
        snprintf(out, size, "en");
}
