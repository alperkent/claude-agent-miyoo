/*
 * Claude Agent: plugins, tools added by developers or by Claude itself.
 *
 * A plugin is a folder plugins/<name>/ with plugin.json and a program (usually run.sh). Each one
 * appears to Claude as the tool "plugin_<name>", with its own setting in the app
 * (Settings > Developer options > Plugins): Off, Ask every time, Ask once, Don't ask.
 * Claude can write one with plugin_create; it is installed only if the person says yes on the Miyoo.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "common.h"

extern int g_is_net;

static const char *DOCS =
    "CLAUDE AGENT PLUGINS\n"
    "A plugin is a new tool for you (Claude) on this Miyoo: a folder /mnt/SDCARD/App/ClaudeAgent/plugins/<name>/ "
    "with plugin.json and a program. It appears as the tool plugin_<name> in NEW chats, after the person "
    "accepts it on the Miyoo. Make one when a task needs something the other tools do not do well, or that "
    "will be repeated (reading a game's own stats, a special test, a fix the user will want again). Suggest it "
    "to the user first, in plain words.\n\n"
    "plugin.json fields:\n"
    "  name: letters, digits and _ (the tool becomes plugin_<name>)\n"
    "  title: short name shown on the Miyoo, like \"FPS counter\"\n"
    "  summary: one plain sentence for the person: what it does\n"
    "  description: for you: what it does, when to use it, what it returns\n"
    "  permission: what it is allowed to do, shown to the person: screen, read, write, control or command. "
    "Choose the closest; anything that changes files is write, anything that runs other programs is command.\n"
    "  input_schema: JSON Schema of its arguments (type object)\n"
    "  run: the program to start, default run.sh (sh runs it; other files are executed directly)\n"
    "  timeout_s: default 60, max 600\n"
    "  version, author: optional\n\n"
    "How it runs: in its own folder, as root, with BusyBox sh (ARMv7, glibc 2.28: no bash, no python). "
    "The arguments arrive as JSON on standard input, in the file named by CA_ARGS_FILE and, for simple values, "
    "as environment variables ARG_<name> (for example ARG_path). Also set: CA_HOME (the app folder), "
    "CA_PLUGIN_DIR. /mnt/SDCARD/.tmp_update/bin has jq, curl and 7z. Write the answer on standard output: "
    "plain text, or one JSON object {\"text\":\"...\",\"images\":[\"/path/a.png\"],\"error\":false} to also "
    "show pictures. Exit code not 0 = error. Output is limited to 256 KB.\n\n"
    "Create it with plugin_create (it shows the person what it is and asks). To change one, create it again "
    "with the same name: the old version goes to the backups. Keep plugins small, readable and safe: never "
    "touch /dev/mtd*, /dev/mmcblk*, or the boot partitions.";

typedef struct {
    char name[48], title[64], summary[300], perm[16], run[64];
    int timeout;
    char *description;
    cJSON *schema;
} plugin_t;

static void plugins_dir(char *out, size_t size)
{
    snprintf(out, size, "%s/plugins", g_home);
}

static int name_ok(const char *n)
{
    if (!n || !*n || strlen(n) > 40)
        return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    return 1;
}

static char *slurp_file(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *b = malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (!b)
        return NULL;
    b[n] = 0;
    return b;
}

static int plugin_load(const char *name, plugin_t *p)
{
    memset(p, 0, sizeof(*p));
    char dir[PATH_MAX], path[PATH_MAX + 20];
    plugins_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s/plugin.json", dir, name);
    char *s = slurp_file(path, 65536);
    cJSON *j = cJSON_Parse(s ? s : "");
    free(s);
    if (!j)
        return -1;
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->title, sizeof(p->title), "%s", arg_str(j, "title", name));
    snprintf(p->summary, sizeof(p->summary), "%s", arg_str(j, "summary", ""));
    snprintf(p->perm, sizeof(p->perm), "%s", arg_str(j, "permission", "command"));
    snprintf(p->run, sizeof(p->run), "%s", arg_str(j, "run", "run.sh"));
    p->timeout = (int)arg_num(j, "timeout_s", 60);
    if (p->timeout < 1 || p->timeout > 600)
        p->timeout = 60;
    p->description = strdup(arg_str(j, "description", p->summary));
    cJSON *sc = cJSON_GetObjectItem(j, "input_schema");
    p->schema = sc ? cJSON_Duplicate(sc, 1) : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}");
    cJSON_Delete(j);
    int known = 0;
    for (int i = 0; i < PERM_COUNT; i++)
        known |= !strcmp(p->perm, perm_keys[i]);
    if (!known)
        snprintf(p->perm, sizeof(p->perm), "command");
    return 0;
}

static void plugin_free(plugin_t *p)
{
    free(p->description);
    cJSON_Delete(p->schema);
}

/* 0 off, 1 ask every time, 2 ask once (default), 3 don't ask */
int plugin_mode(const char *name)
{
    char key[80];
    snprintf(key, sizeof(key), "plugin_%s", name);
    int m = setting_num(key, 2);
    return m < 0 || m > 3 ? 2 : m;
}

/* names of the installed plugins, sorted; caller frees */
int plugins_list(char (**names)[48])
{
    char dir[PATH_MAX];
    plugins_dir(dir, sizeof(dir));
    DIR *d = opendir(dir);
    int n = 0;
    *names = NULL;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !name_ok(e->d_name))
            continue;
        char p[PATH_MAX + 80];
        snprintf(p, sizeof(p), "%s/%s/plugin.json", dir, e->d_name);
        if (access(p, F_OK) != 0)
            continue;
        *names = realloc(*names, (n + 1) * sizeof(**names));
        snprintf((*names)[n++], 48, "%s", e->d_name);
    }
    closedir(d);
    qsort(*names, n, 48, (int (*)(const void *, const void *))strcmp);
    return n;
}

/* title and summary for the app's list */
int plugin_info(const char *name, char *title, size_t ts, char *summary, size_t ss, char *perm, size_t ps)
{
    plugin_t p;
    if (plugin_load(name, &p) != 0)
        return -1;
    snprintf(title, ts, "%s", p.title);
    snprintf(summary, ss, "%s", p.summary);
    snprintf(perm, ps, "%s", p.perm);
    plugin_free(&p);
    return 0;
}

void plugins_add_tools(cJSON *arr)
{
    char(*names)[48];
    int n = plugins_list(&names);
    for (int i = 0; i < n; i++) {
        if (!plugin_mode(names[i]))
            continue;
        plugin_t p;
        if (plugin_load(names[i], &p) != 0)
            continue;
        cJSON *t = cJSON_CreateObject();
        char tn[64];
        snprintf(tn, sizeof(tn), "plugin_%s", p.name);
        cJSON_AddStringToObject(t, "name", tn);
        char desc[2200];
        snprintf(desc, sizeof(desc), "Plugin \"%s\". %s", p.title, p.description);
        cJSON_AddStringToObject(t, "description", desc);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Duplicate(p.schema, 1));
        cJSON_AddItemToArray(arr, t);
        plugin_free(&p);
    }
    free(names);
}

static const char *perm_title_of(const char *key)
{
    for (int i = 0; i < PERM_COUNT; i++)
        if (!strcmp(key, perm_keys[i]))
            return perm_titles[i];
    return perm_titles[4];
}

cJSON *plugin_call(const char *tool, cJSON *args)
{
    const char *name = tool + 7;
    plugin_t p;
    if (!name_ok(name) || plugin_load(name, &p) != 0)
        return result_error("unknown tool '%s'", tool);
    int mode = plugin_mode(name);
    if (!mode) {
        plugin_free(&p);
        return result_error("The plugin \"%s\" is turned off in the app (START > Developer options > Plugins).",
                            p.title);
    }
    char key[80], grant[160], why[600];
    snprintf(key, sizeof(key), "plugin_%s", name);
    snprintf(grant, sizeof(grant), "%s%s", PERM_GRANT, key);
    if (mode == 1 || (mode == 2 && access(grant, F_OK) != 0)) {
        char title[120], text[700];
        snprintf(title, sizeof(title), T("Allow: %s?"), p.title);
        snprintf(text, sizeof(text), T("Claude wants to use the plugin \"%s\": %s It can do what \"%s\" allows.%s"),
                 p.title, p.summary, T(perm_title_of(p.perm)),
                 mode == 2 ? T(" If you allow it, it stays allowed until you close this app.") : "");
        if (!perm_ask(key, title, text, NULL, mode == 2, why, sizeof(why))) {
            plugin_free(&p);
            return result_error("%s", why);
        }
    }

    char dir[PATH_MAX], pdir[PATH_MAX + 64], argfile[PATH_MAX + 80];
    plugins_dir(dir, sizeof(dir));
    snprintf(pdir, sizeof(pdir), "%s/%s", dir, name);
    snprintf(argfile, sizeof(argfile), "/tmp/claude_agent_plugin_%s.json", name);
    char *aj = cJSON_PrintUnformatted(args ? args : cJSON_CreateObject());
    FILE *af = fopen(argfile, "w");
    if (af) {
        fputs(aj, af);
        fclose(af);
    }
    int in[2], out[2];
    if (pipe(in) || pipe(out)) {
        free(aj);
        plugin_free(&p);
        return result_error("cannot start the plugin");
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[1]);
        close(out[0]);
        if (chdir(pdir) != 0)
            _exit(126);
        setenv("CA_ARGS_FILE", argfile, 1);
        setenv("CA_HOME", g_home, 1);
        setenv("CA_PLUGIN_DIR", pdir, 1);
        setenv("PATH", "/mnt/SDCARD/.tmp_update/bin:/usr/bin:/bin:/usr/sbin:/sbin:/customer/app", 1);
        cJSON *it;
        cJSON_ArrayForEach(it, args)
        {
            char ek[80], ev[1024];
            if (!it->string || strspn(it->string, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") !=
                                   strlen(it->string))
                continue;
            snprintf(ek, sizeof(ek), "ARG_%s", it->string);
            if (cJSON_IsString(it))
                snprintf(ev, sizeof(ev), "%s", it->valuestring);
            else if (cJSON_IsNumber(it))
                snprintf(ev, sizeof(ev), "%g", it->valuedouble);
            else if (cJSON_IsBool(it))
                snprintf(ev, sizeof(ev), "%s", cJSON_IsTrue(it) ? "true" : "false");
            else
                continue;
            setenv(ek, ev, 1);
        }
        size_t rl = strlen(p.run);
        if (rl > 3 && !strcmp(p.run + rl - 3, ".sh"))
            execl("/bin/sh", "sh", p.run, (char *)NULL);
        else {
            char exe[100];
            snprintf(exe, sizeof(exe), "./%s", p.run);
            execl(exe, p.run, (char *)NULL);
        }
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (write(in[1], aj, strlen(aj)) < 0) {
    }
    close(in[1]);
    free(aj);
    size_t cap = 262144, len = 0;
    char *buf = malloc(cap + 1);
    long long until = now_ms() + p.timeout * 1000LL;
    int timed_out = 0;
    for (;;) {
        struct pollfd pf = {out[0], POLLIN, 0};
        int r = poll(&pf, 1, 200);
        if (r > 0) {
            char tmp[4096];
            ssize_t n = read(out[0], tmp, sizeof(tmp));
            if (n <= 0)
                break;
            if (len + n > cap)
                n = cap - len;
            memcpy(buf + len, tmp, n);
            len += n;
        }
        if (now_ms() > until) {
            timed_out = 1;
            kill(-pid, SIGKILL);
            break;
        }
    }
    close(out[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    buf[len] = 0;
    unlink(argfile);
    cJSON *res;
    int failed = timed_out || !WIFEXITED(st) || WEXITSTATUS(st) != 0;
    cJSON *j = len && buf[0] == '{' ? cJSON_Parse(buf) : NULL;
    if (j && cJSON_GetObjectItem(j, "text")) {
        res = result_new();
        result_text(res, arg_str(j, "text", ""));
        cJSON *im;
        cJSON_ArrayForEach(im, cJSON_GetObjectItem(j, "images"))
        {
            if (!cJSON_IsString(im))
                continue;
            size_t n = 0;
            char *data = slurp_file(im->valuestring, 4 * 1048576);
            FILE *f = fopen(im->valuestring, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                n = ftell(f);
                fclose(f);
            }
            if (data && n > 8)
                result_image(res, (unsigned char *)data, n,
                             (unsigned char)data[0] == 0xff ? "image/jpeg" : "image/png");
            free(data);
        }
        failed |= arg_bool(j, "error", 0);
    }
    else {
        res = result_new();
        result_text(res, len ? buf : "(no output)");
    }
    cJSON_Delete(j);
    if (failed) {
        char note[200];
        if (timed_out)
            snprintf(note, sizeof(note), "The plugin was stopped after %d s.", p.timeout);
        else
            snprintf(note, sizeof(note), "The plugin ended with %s %d.", WIFEXITED(st) ? "exit code" : "signal",
                     WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
        result_text(res, note);
        cJSON_DeleteItemFromObject(res, "isError");
        cJSON_AddBoolToObject(res, "isError", 1);
    }
    free(buf);
    plugin_free(&p);
    return res;
}

static void tools_changed(void)
{
    if (g_is_net)
        kill(getpid(), SIGHUP); /* the connection sends the new list of tools */
}

cJSON *tool_plugin_create(cJSON *args)
{
    const char *name = arg_str(args, "name", NULL);
    if (!name_ok(name))
        return result_error("'name' must be letters, digits and _ (up to 40)");
    const char *title = arg_str(args, "title", name), *summary = arg_str(args, "summary", "");
    const char *perm = arg_str(args, "permission", "command");
    cJSON *files = cJSON_GetObjectItem(args, "files");
    if (!cJSON_IsObject(files) || !cJSON_GetArraySize(files))
        return result_error("'files' must have the plugin's files, like {\"run.sh\":\"...\"}");
    const char *run = arg_str(args, "run", "run.sh");
    if (!cJSON_GetObjectItem(files, run))
        return result_error("'files' has no %s (the program to start)", run);
    if (!summary[0])
        return result_error("'summary' is needed: one plain sentence for the person");
    int known = 0;
    for (int i = 0; i < PERM_COUNT; i++)
        known |= !strcmp(perm, perm_keys[i]);
    if (!known)
        return result_error("'permission' must be screen, read, write, control or command");

    char dir[PATH_MAX], pend[PATH_MAX + 64], final[PATH_MAX + 64], cmd[3 * PATH_MAX];
    plugins_dir(dir, sizeof(dir));
    snprintf(pend, sizeof(pend), "%s/.pending/%s", dir, name);
    snprintf(final, sizeof(final), "%s/%s", dir, name);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", pend);
    if (system(cmd)) {
    }
    if (mkdirs(pend) != 0)
        return result_error("cannot create %s", pend);
    size_t total = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, files)
    {
        if (!cJSON_IsString(it) || !it->string || strstr(it->string, "..") || it->string[0] == '/' ||
            !strcmp(it->string, "plugin.json"))
            return result_error("bad file entry '%s'", it->string ? it->string : "?");
        total += strlen(it->valuestring);
        if (total > 1048576)
            return result_error("the plugin is too big (1 MB of files at most)");
        char fp[PATH_MAX * 2];
        snprintf(fp, sizeof(fp), "%s/%s", pend, it->string);
        char d2[PATH_MAX * 2];
        snprintf(d2, sizeof(d2), "%s", fp);
        *strrchr(d2, '/') = 0;
        mkdirs(d2);
        FILE *f = fopen(fp, "w");
        if (!f)
            return result_error("cannot write %s", fp);
        fputs(it->valuestring, f);
        fclose(f);
        chmod(fp, 0755);
    }
    cJSON *pj = cJSON_CreateObject();
    cJSON_AddStringToObject(pj, "name", name);
    cJSON_AddStringToObject(pj, "title", title);
    cJSON_AddStringToObject(pj, "summary", summary);
    cJSON_AddStringToObject(pj, "description", arg_str(args, "description", summary));
    cJSON_AddStringToObject(pj, "permission", perm);
    cJSON_AddStringToObject(pj, "run", run);
    cJSON_AddNumberToObject(pj, "timeout_s", arg_num(args, "timeout_s", 60));
    cJSON_AddStringToObject(pj, "version", arg_str(args, "version", "1.0"));
    cJSON_AddStringToObject(pj, "author", arg_str(args, "author", "Claude"));
    cJSON *sc = cJSON_GetObjectItem(args, "input_schema");
    cJSON_AddItemToObject(pj, "input_schema",
                          sc ? cJSON_Duplicate(sc, 1) : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
    char *js = cJSON_Print(pj);
    cJSON_Delete(pj);
    char jp[PATH_MAX * 2];
    snprintf(jp, sizeof(jp), "%s/plugin.json", pend);
    FILE *f = fopen(jp, "w");
    if (f) {
        fputs(js, f);
        fclose(f);
    }
    free(js);

    /* the person decides */
    char qtitle[120], text[800], why[600];
    int update = access(final, F_OK) == 0;
    snprintf(qtitle, sizeof(qtitle), "%s", T(update ? "Update a tool from Claude?" : "Add a tool from Claude?"));
    snprintf(text, sizeof(text),
             T(update ? "Claude made a new version of the tool \"%s\": %s It can do what \"%s\" allows. You can "
                        "turn it off or remove it in Settings > Developer options > Plugins."
                      : "Claude made a new tool \"%s\": %s It can do what \"%s\" allows. You can turn it off or "
                        "remove it in Settings > Developer options > Plugins."),
             title, summary, T(perm_title_of(perm)));
    if (!perm_ask("install", qtitle, text, NULL, 0, why, sizeof(why))) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", pend);
        if (system(cmd)) {
        }
        return result_error("%s The plugin was not installed.", why);
    }
    if (update) { /* the old version goes to the backups */
        char stamp[32], old[PATH_MAX * 2];
        time_t t = time(NULL);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
        snprintf(old, sizeof(old), "%s/backups/%s-plugin-%s", g_home, stamp, name);
        char bd[PATH_MAX];
        snprintf(bd, sizeof(bd), "%s/backups", g_home);
        mkdirs(bd);
        rename(final, old);
    }
    if (rename(pend, final) != 0)
        return result_error("could not install the plugin: %s", strerror(errno));
    char key[80];
    snprintf(key, sizeof(key), "plugin_%s", name);
    if (!update)
        setting_set_num(key, 2);
    activity("Plugin %s: %s", T(update ? "updated" : "added"), title);
    tools_changed();
    cJSON *r = result_new();
    result_textf(r,
                 "Plugin \"%s\" installed in %s. It is the tool plugin_%s in new chats (the person may need to "
                 "start a new chat). Set to \"Ask once\": the Miyoo asks the first time it is used.",
                 title, final, name);
    return r;
}

cJSON *tool_plugins(cJSON *args)
{
    const char *action = arg_str(args, "action", "list");
    if (!strcmp(action, "docs")) {
        cJSON *r = result_new();
        result_text(r, DOCS);
        return r;
    }
    if (!strcmp(action, "list")) {
        char(*names)[48];
        int n = plugins_list(&names);
        char out[6000] = "";
        size_t l = 0;
        static const char *modes[] = {"off", "asks every time", "asks once", "doesn't ask"};
        for (int i = 0; i < n && l < sizeof(out) - 400; i++) {
            char t[64], s[300], pm[16];
            if (plugin_info(names[i], t, sizeof(t), s, sizeof(s), pm, sizeof(pm)) == 0)
                l += snprintf(out + l, sizeof(out) - l, "plugin_%s: \"%s\" (%s, permission %s) - %s\n", names[i], t,
                              modes[plugin_mode(names[i])], pm, s);
        }
        free(names);
        cJSON *r = result_new();
        result_text(r, n ? out : "No plugins installed. action docs explains how to make one.");
        return r;
    }
    if (!strcmp(action, "remove")) {
        const char *name = arg_str(args, "name", NULL);
        char dir[PATH_MAX], pd[PATH_MAX + 64], old[PATH_MAX * 2], stamp[32];
        plugins_dir(dir, sizeof(dir));
        snprintf(pd, sizeof(pd), "%s/%s", dir, name ? name : "");
        if (!name_ok(name) || access(pd, F_OK) != 0)
            return result_error("no plugin named %s", name ? name : "?");
        time_t t = time(NULL);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
        snprintf(old, sizeof(old), "%s/backups/%s-plugin-%s", g_home, stamp, name);
        char bd[PATH_MAX];
        snprintf(bd, sizeof(bd), "%s/backups", g_home);
        mkdirs(bd);
        if (rename(pd, old) != 0)
            return result_error("could not remove it: %s", strerror(errno));
        activity("Plugin removed: %s", name);
        tools_changed();
        cJSON *r = result_new();
        result_textf(r, "Removed plugin %s (kept in %s).", name, old);
        return r;
    }
    return result_error("'action' must be list, docs or remove");
}
