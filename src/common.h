/* Claude Agent: things shared by agent.c, net.c, perm.c and ui.c */
#ifndef CLAUDE_AGENT_COMMON_H
#define CLAUDE_AGENT_COMMON_H

#include "cJSON.h"

extern char g_home[];

#define AGENT_VERSION "1.0.1"
#define UPDATE_STATUS "/tmp/claude_agent_update" /* state=, version=, note=... written by "update" */
int update_main(int argc, char **argv);

/* files in /tmp: gone when the device restarts */
#define BG_FLAG "/tmp/claude_agent_background"     /* keep running after leaving the app */
#define BG_REQUEST "/tmp/claude_agent_bg_request"  /* Claude asks for background mode */
#define BG_ANSWER "/tmp/claude_agent_bg_answer"
#define PERM_ASK "/tmp/claude_agent_ask"           /* Claude asks for a permission: cat=, tool=, detail= */
#define PERM_ANSWER "/tmp/claude_agent_answer"     /* "allow" or "deny", written by the app screen */
#define PERM_GRANT "/tmp/claude_agent_ok_"         /* + category: allowed until the app closes */
#define BG_AUTO_SESSION "/tmp/claude_agent_bg_auto" /* background without asking, until the Miyoo restarts */
#define DEV_SESSION "/tmp/claude_agent_dev"         /* developer tools on, until the app closes */

/* permission categories, in the order of the settings screen */
#define PERM_COUNT 5
extern const char *const perm_keys[PERM_COUNT];   /* screen read write control command */
extern const char *const perm_titles[PERM_COUNT]; /* "See the screen", ... */
extern const char *const perm_texts[PERM_COUNT];  /* what allowing it means */

/* settings (config/settings.json) */
int setting_get(const char *key);
void setting_set(const char *key, int value);

/* activity.log (what the agent did) and messages.log (what Claude said) */
void activity(const char *fmt, ...);
void messages_add(const char *text);

/* asks the person holding the device; 1 = allowed */
int perm_check(const char *tool, cJSON *args, char *why, size_t why_size);
void perm_clear_session(void);
int ui_running(void);
const char *T(const char *en); /* the text in the Miyoo's language (i18n.c) */
const char *lang_code(void);    /* language of the app's texts: "en", "pt-BR"... */
void lang_system(char *out, size_t size);
int cpu_mhz_real(void); /* the clock the CPU really runs at (PLL), -1 if unknown */

cJSON *tool_show_message(cJSON *args);
cJSON *tool_show_plan(cJSON *args);
/* backups.c: the backups folder */
long long backups_kb(void);
long long backups_empty(void);
void backups_prune(int days);
cJSON *tool_delete_path(cJSON *args);
void activity_call(const char *tool, cJSON *args);
void activity_result(const char *tool, cJSON *result);

/* helpers from agent.c, shared with dev.c */
long long now_ms(void);
void sleep_ms(int ms);
int mkdirs(const char *path);
char *b64_encode(const unsigned char *d, size_t n);
cJSON *result_new(void);
void result_text(cJSON *r, const char *text);
void result_textf(cJSON *r, const char *fmt, ...);
cJSON *result_error(const char *fmt, ...);
void result_image(cJSON *r, const unsigned char *data, size_t len, const char *mime);
const char *arg_str(cJSON *args, const char *name, const char *def);
double arg_num(cJSON *args, const char *name, double def);
int arg_bool(cJSON *args, const char *name, int def);
int write_path_blocked(const char *path);
int backup_file(const char *path, char *where, size_t wsize);
int button_code(const char *name);
const char *button_name(int code);

typedef struct {
    int w, h, pitch, bpp, page, pages;
} screen_raw_t;
unsigned char *screen_grab(screen_raw_t *sr, char *err, size_t errsize);
unsigned char *screen_png(const unsigned char *src, const screen_raw_t *sr, int scale, size_t *png_len);

/* net.c: HTTPS GET for download_file. sink returns 0 to go on, -1 to stop.
 * total is set when the size is known (-1 otherwise). 0 = ok, -1 = error (message in err). */
typedef int (*https_sink_fn)(void *ctx, const unsigned char *data, size_t len);
int https_get(const char *url, https_sink_fn sink, void *ctx, long long *total, volatile int *cancel,
              char *err, size_t errsize);

int dev_tools_on(void);

/* changes.c: what Claude changed, and undo */
typedef struct {
    int id, undone;
    char when[24], action[16];
    char path[1024], backup[1200];
} change_t;
void change_log(const char *action, const char *path, const char *backup);
int changes_load(change_t **out);
int change_undo(int id, char *msg, size_t msize);
int setting_num(const char *key, int def);
void setting_set_num(const char *key, int value);
int perm_mode(int cat);
int perm_ask(const char *key, const char *title, const char *text, const char *detail, int remember, char *why,
             size_t why_size);

/* dev.c: background jobs (long downloads, recordings, replays, runs) */
#include <pthread.h>
typedef struct job {
    int id;
    char kind[24];
    char what[300];       /* one line: what the job does */
    char progress[300];   /* one line: how far it is */
    volatile int done;    /* the thread finished */
    volatile int cancel;  /* asked to stop */
    cJSON *result;        /* the tool result, when done */
    long long started, ended;
    void *arg;
    struct job *next;
} job_t;
extern pthread_mutex_t jobs_lock;
job_t *job_start(const char *kind, const char *what, void *(*fn)(void *), void *arg);
job_t *job_running(const char *kind);
void job_progress(job_t *j, const char *fmt, ...);
void job_finish(job_t *j, cJSON *result);
cJSON *job_wait(job_t *j, int ms);
int ui_running(void);

/* devrun.c: run and watch a program, performance, screenshots, log watch, snapshots */
cJSON *tool_run_app(cJSON *args);
cJSON *tool_perf_monitor(cJSON *args);
cJSON *tool_screen_capture(cJSON *args);
cJSON *tool_watch_file(cJSON *args);
cJSON *tool_snapshot(cJSON *args);

/* plugins.c */
int plugin_mode(const char *name);
int plugins_list(char (**names)[48]);
int plugin_info(const char *name, char *title, size_t ts, char *summary, size_t ss, char *perm, size_t ps);
void plugins_add_tools(cJSON *arr);
cJSON *plugin_call(const char *tool, cJSON *args);
cJSON *tool_plugin_create(cJSON *args);
cJSON *tool_plugins(cJSON *args);

/* dev.c: tools for developers (downloads, input recording and replay, background jobs) */
cJSON *tool_download_file(cJSON *args);
cJSON *tool_job_status(cJSON *args);
cJSON *tool_view_image(cJSON *args);
cJSON *tool_input_record(cJSON *args);
cJSON *tool_input_play(cJSON *args);

#endif
