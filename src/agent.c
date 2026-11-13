/*
 * Claude Agent for Miyoo Mini / Mini Plus / Mini Flip (OnionOS and RedOnion)
 *
 * Stage 1: the device tools, usable from the command line (over SSH).
 * Every tool takes JSON arguments and answers with an MCP "CallToolResult"
 * ({"content":[...], "isError":bool}), so the same code is used later by the
 * network part that talks to Claude.
 *
 *   claude-agent tools                       list the tools (MCP schemas)
 *   claude-agent call '<json>'               {"tool":"...","args":{...}}
 *   claude-agent info                        same as the device_info tool
 *   claude-agent shot <file.png> [half]      save a screenshot
 *   claude-agent keys "<sequence>"           press buttons, e.g. "DOWN*2 A"
 *   claude-agent exec "<command>"            run a shell command
 *   claude-agent connect [--pair]            connect to the relay (see net.c)
 *   claude-agent unpair                      remove every Claude connection
 *   claude-agent ui                          the app screen (see ui.c)
 *
 * Environment overrides (for tests): CA_HOME (app folder), CA_FB (/dev/fb0),
 * CA_FB_MODE ("WxHxBPP", only when CA_FB is a plain file), CA_INPUT
 * (/dev/input/event0).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "cJSON.h"
#include "common.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#include "stb_image_write.h"

#define SD "/mnt/SDCARD"

char g_home[PATH_MAX];
int net_main(int argc, char **argv);
int ui_main(void);

/* ------------------------------------------------------------------ util */

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

int mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Reads a small text file into buf; strips the trailing newline. */
static int read_small(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = 0;
    return (int)n;
}

static void agent_log(const char *fmt, ...)
{
    char dir[PATH_MAX], path[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/logs", g_home);
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/agent.log", dir);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 512 * 1024) {
        char old[PATH_MAX];
        snprintf(old, sizeof(old), "%s/agent.old.log", dir);
        rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", tm.tm_year + 1900, tm.tm_mon + 1,
            tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const unsigned char *d, size_t n)
{
    size_t out = 4 * ((n + 2) / 3);
    char *s = malloc(out + 1), *p = s;
    if (!s)
        return NULL;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        *p++ = B64[v >> 18];
        *p++ = B64[(v >> 12) & 63];
        *p++ = B64[(v >> 6) & 63];
        *p++ = B64[v & 63];
    }
    if (i < n) {
        unsigned v = d[i] << 16;
        if (i + 1 < n)
            v |= d[i + 1] << 8;
        *p++ = B64[v >> 18];
        *p++ = B64[(v >> 12) & 63];
        *p++ = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = 0;
    return s;
}

static unsigned char *b64_decode(const char *s, size_t *outlen)
{
    size_t n = strlen(s);
    unsigned char *out = malloc(n / 4 * 3 + 3), *p = out;
    if (!out)
        return NULL;
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        const char *c = strchr(B64, s[i]);
        if (s[i] == '=' || !c || !s[i])
            continue;
        v = (v << 6) | (unsigned)(c - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *p++ = (unsigned char)(v >> bits);
        }
    }
    *outlen = (size_t)(p - out);
    return out;
}

/* ---------------------------------------------------------- MCP results */

cJSON *result_new(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "content", cJSON_CreateArray());
    cJSON_AddBoolToObject(r, "isError", 0);
    return r;
}

void result_text(cJSON *r, const char *text)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", text);
    cJSON_AddItemToArray(cJSON_GetObjectItem(r, "content"), c);
}

void result_textf(cJSON *r, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    result_text(r, buf);
}

cJSON *result_error(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cJSON *r = result_new();
    result_text(r, buf);
    cJSON_ReplaceItemInObject(r, "isError", cJSON_CreateBool(1));
    agent_log("  error: %s", buf);
    return r;
}

const char *arg_str(cJSON *args, const char *name, const char *def)
{
    cJSON *v = cJSON_GetObjectItem(args, name);
    return cJSON_IsString(v) ? v->valuestring : def;
}

double arg_num(cJSON *args, const char *name, double def)
{
    cJSON *v = cJSON_GetObjectItem(args, name);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

int arg_bool(cJSON *args, const char *name, int def)
{
    cJSON *v = cJSON_GetObjectItem(args, name);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

/* ------------------------------------------------------------- safety */

/*
 * Seat belts, not a sandbox: the agent runs as root and Claude can do
 * almost anything. What is refused here is what can brick the device or
 * wipe the SD card in one step: the internal flash (bootloader, kernel,
 * stock firmware) and raw writes to block devices.
 */
static const char *const blocked_cmd[] = {
    "/dev/mtd", "/dev/mmcblk", "/dev/block", "flash_erase", "flashcp",
    "nandwrite", "mtd_debug", "fw_setenv", "mkfs", "fdisk", "parted",
    NULL};

static const char *command_blocked(const char *cmd)
{
    for (int i = 0; blocked_cmd[i]; i++)
        if (strstr(cmd, blocked_cmd[i]))
            return blocked_cmd[i];
    return NULL;
}

int write_path_blocked(const char *path)
{
    return strncmp(path, "/dev/", 5) == 0 || strncmp(path, "/proc/", 6) == 0 ||
           strncmp(path, "/sys/", 5) == 0;
}

/* --------------------------------------------------------- device_info */

static int process_running(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char p[64], comm[64];
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        if (read_small(p, comm, sizeof(comm)) > 0 && strcmp(comm, name) == 0)
            found = 1;
    }
    closedir(d);
    return found;
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[128];
    long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
            v = atol(line + kl + 1);
            break;
        }
    fclose(f);
    return v;
}

/* The real CPU clock. scaling_cur_freq only knows the cpufreq table, and cpuclock (used by OnionOS,
   ports and run_app) sets the PLL directly, so read the PLL like cpuclock does. Falls back to cpufreq. */
int cpu_mhz_real(void)
{
    int mhz = -1;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd >= 0) {
        volatile uint8_t *p8 = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1F000000 + 0x103000 * 2);
        if (p8 != MAP_FAILED) {
            volatile uint16_t *p16 = (volatile uint16_t *)p8;
            uint32_t lpf = p16[0x2A4] + ((uint32_t)p16[0x2A6] << 16);
            if (!lpf)
                lpf = ((uint32_t)p8[0x2C2 << 1] << 16) + ((uint32_t)p8[0x2C1 << 1] << 8) + p8[0x2C0 << 1];
            uint32_t post_div = p16[0x232] + 1;
            if (lpf && post_div) {
                unsigned long long rate = 432000000ULL * 524288 / lpf * 2 / post_div * 16;
                mhz = (int)(rate / 1000000);
                if (mhz < 100 || mhz > 3000)
                    mhz = -1;
            }
            munmap((void *)p8, 0x1000);
        }
        close(fd);
    }
    if (mhz < 0) {
        char b[32];
        if (read_small("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", b, sizeof(b)) > 0)
            mhz = atoi(b) / 1000;
    }
    return mhz;
}


static cJSON *tool_device_info(cJSON *args)
{
    (void)args;
    char buf[512];
    cJSON *o = cJSON_CreateObject();

    int model = 0;
    if (read_small("/tmp/deviceModel", buf, sizeof(buf)) > 0)
        model = atoi(buf);
    char res[32] = "";
    read_small("/tmp/screen_resolution", res, sizeof(res));
    const char *name = "unknown";
    if (model == 354)
        name = "Miyoo Mini Plus";
    else if (model == 285)
        name = "Miyoo Mini Flip";
    else if (model == 283)
        name = strcmp(res, "752x560") == 0 ? "Miyoo Mini v4" : "Miyoo Mini (v2/v3)";
    cJSON_AddStringToObject(o, "model", name);
    cJSON_AddNumberToObject(o, "model_id", model);
    cJSON_AddStringToObject(o, "panel_resolution", res[0] ? res : "640x480");

    /* what Claude should know about this model before changing settings or testing games */
    cJSON *pf = cJSON_CreateObject();
    int is_plus = model == 354, is_flip = model == 285, is_v4 = model == 283 && !strcmp(res, "752x560");
    cJSON_AddNumberToObject(pf, "screen_inches", is_plus ? 3.5 : 2.8);
    cJSON_AddNumberToObject(pf, "cpu_max_safe_mhz", is_plus ? 1800 : 1600);
    cJSON_AddNumberToObject(pf, "ram_mb", 128);
    cJSON_AddBoolToObject(pf, "wifi", is_plus || is_flip);
    cJSON_AddBoolToObject(pf, "battery_chip", is_plus || is_flip); /* AXP: charging state and % */
    cJSON_AddBoolToObject(pf, "clock_keeps_time_off", access("/dev/rtc0", F_OK) == 0);
    cJSON_AddBoolToObject(pf, "lid", is_flip);
    cJSON_AddStringToObject(
        pf, "notes",
        is_plus ? "Mini Plus: 3.5 inch 640x480 screen refreshing at about 59.7 Hz (measure it, do not assume 60). "
                  "CPU up to 1800 MHz. 128 MB RAM shared by the system, the game and this agent."
        : is_flip ? "Mini Flip: 2.8 inch 640x480 screen with a lid (closing it sleeps the device). CPU up to "
                    "1600 MHz. 128 MB RAM."
        : is_v4 ? "Mini v4: 2.8 inch 752x560 screen (OnionOS can show 640x480 games scaled or centered). CPU up "
                  "to 1600 MHz. No Wi-Fi: this agent normally cannot run on it. 128 MB RAM."
                : "Mini v2/v3: 2.8 inch 640x480 screen. CPU up to 1600 MHz. No Wi-Fi. 128 MB RAM.");
    cJSON_AddItemToObject(o, "profile", pf);

    /* firmware: RedOnion ships its own website.txt */
    char ver[64] = "", site[256] = "";
    read_small(SD "/.tmp_update/onionVersion/version.txt", ver, sizeof(ver));
    read_small(SD "/.tmp_update/onionVersion/website.txt", site, sizeof(site));
    const char *os = "unknown";
    if (access(SD "/.tmp_update", F_OK) == 0)
        os = (strcasestr(site, "redonion") || strcasestr(site, "Amiga500")) ? "RedOnion" : "OnionOS";
    cJSON_AddStringToObject(o, "os", os);
    cJSON_AddStringToObject(o, "os_version", ver);

    cJSON *bat = cJSON_CreateObject();
    if (read_small("/tmp/percBat", buf, sizeof(buf)) > 0)
        cJSON_AddNumberToObject(bat, "percent", atoi(buf));
    if (read_small("/sys/class/power_supply/battery/status", buf, sizeof(buf)) > 0)
        cJSON_AddStringToObject(bat, "status", buf);
    cJSON_AddItemToObject(o, "battery", bat);

    cJSON *cpu = cJSON_CreateObject();
    int mhz_now = cpu_mhz_real();
    if (mhz_now > 0)
        cJSON_AddNumberToObject(cpu, "mhz", mhz_now);
    if (read_small("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", buf, sizeof(buf)) > 0)
        cJSON_AddStringToObject(cpu, "governor", buf);
    if (read_small("/proc/loadavg", buf, sizeof(buf)) > 0)
        cJSON_AddStringToObject(cpu, "loadavg", buf);
    cJSON_AddItemToObject(o, "cpu", cpu);

    cJSON *mem = cJSON_CreateObject();
    cJSON_AddNumberToObject(mem, "total_mb", meminfo_kb("MemTotal") / 1024);
    long avail = meminfo_kb("MemAvailable");
    if (avail < 0)
        avail = meminfo_kb("MemFree") + meminfo_kb("Cached");
    cJSON_AddNumberToObject(mem, "available_mb", avail / 1024);
    cJSON_AddItemToObject(o, "memory", mem);

    struct statvfs vfs;
    if (statvfs(SD, &vfs) == 0) {
        cJSON *sd = cJSON_CreateObject();
        cJSON_AddNumberToObject(sd, "total_mb", (double)vfs.f_blocks * vfs.f_frsize / 1048576.0);
        cJSON_AddNumberToObject(sd, "free_mb", (double)vfs.f_bavail * vfs.f_frsize / 1048576.0);
        cJSON_AddItemToObject(o, "sd_card", sd);
    }

    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo") != 0) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
                cJSON_AddStringToObject(o, "ip", ip);
                break;
            }
        freeifaddrs(ifa);
    }

    /* what is on screen */
    const char *fg = "unknown";
    if (process_running("gameSwitcher"))
        fg = "Game Switcher";
    else if (process_running("retroarch") || process_running("ra32"))
        fg = "RetroArch";
    else if (process_running("MainUI"))
        fg = "main menu (MainUI)";
    else if (access(SD "/.tmp_update/cmd_to_run.sh", F_OK) == 0)
        fg = "app or port (see running_command)";
    cJSON_AddStringToObject(o, "foreground", fg);
    if (read_small(SD "/.tmp_update/cmd_to_run.sh", buf, sizeof(buf)) > 0)
        cJSON_AddStringToObject(o, "running_command", buf);

    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    cJSON_AddStringToObject(o, "clock", buf);
    if (read_small("/proc/uptime", buf, sizeof(buf)) > 0)
        cJSON_AddNumberToObject(o, "uptime_s", (long)atof(buf));
    char lsys[24];
    lang_system(lsys, sizeof(lsys));
    cJSON_AddStringToObject(o, "language", lsys); /* the menus' language: write show_message in it */
    cJSON_AddStringToObject(o, "agent_version", AGENT_VERSION);
    cJSON_AddNumberToObject(o, "backups_mb", (double)(backups_kb() / 102) / 10.0);

    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    cJSON *r = result_new();
    result_text(r, s);
    free(s);
    return r;
}

/* Short description sent to the relay when connecting. */
cJSON *device_summary(void)
{
    cJSON *r = tool_device_info(NULL);
    cJSON *c = cJSON_GetArrayItem(cJSON_GetObjectItem(r, "content"), 0);
    cJSON *full = cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItem(c, "text")));
    cJSON_Delete(r);
    cJSON *o = cJSON_CreateObject();
    const char *keys[] = {"model", "os", "os_version", "agent_version", NULL};
    for (int i = 0; keys[i]; i++) {
        cJSON *v = cJSON_GetObjectItem(full, keys[i]);
        if (v)
            cJSON_AddItemToObject(o, keys[i], cJSON_Duplicate(v, 1));
    }
    cJSON_Delete(full);
    return o;
}

/* ----------------------------------------------------------- screenshot */

typedef struct {
    unsigned char *data;
    size_t len, cap;
} membuf;

static void membuf_write(void *ctx, void *data, int size)
{
    membuf *m = ctx;
    if (m->len + size > m->cap) {
        size_t nc = (m->cap ? m->cap * 2 : 65536);
        while (nc < m->len + size)
            nc *= 2;
        unsigned char *nd = realloc(m->data, nc);
        if (!nd)
            return;
        m->data = nd;
        m->cap = nc;
    }
    memcpy(m->data + m->len, data, size);
    m->len += size;
}

/*
 * Grabs the page that is on screen. The panel is mounted upside down, so
 * the picture is turned 180 degrees, as OnionOS does for its screenshots.
 * Pixels are ARGB8888 (B in the low byte) in 32 bpp, or RGB565 in 16 bpp.
 */
/* Copies the page on screen (raw pixels). Caller frees. NULL + message on error. */
unsigned char *screen_grab(screen_raw_t *sr, char *err, size_t errsize)
{
    const char *fbpath = getenv("CA_FB") ? getenv("CA_FB") : "/dev/fb0";
    int fd = open(fbpath, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsize, "cannot open %s: %s", fbpath, strerror(errno));
        return NULL;
    }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    memset(&vi, 0, sizeof(vi));
    memset(&fi, 0, sizeof(fi));
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        const char *mode = getenv("CA_FB_MODE"); /* tests with a plain file */
        unsigned w, h, b;
        if (!mode || sscanf(mode, "%ux%ux%u", &w, &h, &b) != 3) {
            close(fd);
            snprintf(err, errsize, "%s is not a framebuffer", fbpath);
            return NULL;
        }
        vi.xres = vi.xres_virtual = w;
        vi.yres = h;
        vi.yres_virtual = h * 2;
        vi.bits_per_pixel = b;
        fi.line_length = w * b / 8;
        const char *yo = getenv("CA_FB_YOFFSET");
        vi.yoffset = yo ? atoi(yo) : 0;
    }
    sr->bpp = vi.bits_per_pixel;
    if (sr->bpp != 32 && sr->bpp != 16) {
        close(fd);
        snprintf(err, errsize, "unsupported framebuffer format: %d bits per pixel", sr->bpp);
        return NULL;
    }
    sr->w = vi.xres;
    sr->h = vi.yres;
    sr->pitch = fi.line_length;
    sr->page = sr->h ? vi.yoffset / sr->h : 0;
    sr->pages = sr->h ? vi.yres_virtual / sr->h : 0;
    size_t page = (size_t)sr->pitch * sr->h;
    unsigned char *src = malloc(page);
    if (!src) {
        close(fd);
        snprintf(err, errsize, "out of memory");
        return NULL;
    }
    ssize_t got = pread(fd, src, page, (off_t)vi.yoffset * sr->pitch);
    close(fd);
    if (got != (ssize_t)page) {
        free(src);
        snprintf(err, errsize, "could not read the screen (read %d of %d bytes)", (int)got, (int)page);
        return NULL;
    }
    return src;
}

/* Raw pixels -> upright PNG in memory. Returns the PNG (caller frees) or NULL. */
unsigned char *screen_png(const unsigned char *src, const screen_raw_t *sr, int scale, size_t *png_len)
{
    int w = sr->w, h = sr->h, pitch = sr->pitch, bpp = sr->bpp;
    int ow = w / scale, oh = h / scale;
    unsigned char *rgb = malloc((size_t)ow * oh * 3);
    if (!rgb)
        return NULL;
    for (int y = 0; y < oh; y++) {
        int sy = h - 1 - y * scale; /* 180 degrees */
        unsigned char *o = rgb + (size_t)y * ow * 3;
        for (int x = 0; x < ow; x++) {
            int sx = w - 1 - x * scale;
            if (bpp == 32) {
                uint32_t p = *(const uint32_t *)(src + (size_t)sy * pitch + sx * 4);
                o[0] = p >> 16;
                o[1] = p >> 8;
                o[2] = p;
            }
            else {
                uint16_t p = *(const uint16_t *)(src + (size_t)sy * pitch + sx * 2);
                o[0] = ((p >> 11) & 31) * 255 / 31;
                o[1] = ((p >> 5) & 63) * 255 / 63;
                o[2] = (p & 31) * 255 / 31;
            }
            o += 3;
        }
    }
    membuf png = {0};
    stbi_write_png_compression_level = 6;
    int ok = stbi_write_png_to_func(membuf_write, &png, ow, oh, 3, rgb, ow * 3);
    free(rgb);
    if (!ok || !png.data) {
        free(png.data);
        return NULL;
    }
    *png_len = png.len;
    return png.data;
}

/* an MCP image block */
void result_image(cJSON *r, const unsigned char *png, size_t len, const char *mime)
{
    char *b64 = b64_encode(png, len);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "image");
    cJSON_AddStringToObject(c, "mimeType", mime);
    cJSON_AddStringToObject(c, "data", b64 ? b64 : "");
    cJSON_AddItemToArray(cJSON_GetObjectItem(r, "content"), c);
    free(b64);
}

/*
 * Grabs the page that is on screen. The panel is mounted upside down, so
 * the picture is turned 180 degrees, as OnionOS does for its screenshots.
 * Pixels are ARGB8888 (B in the low byte) in 32 bpp, or RGB565 in 16 bpp.
 */
static cJSON *tool_screenshot(cJSON *args)
{
    int scale = (int)arg_num(args, "scale", 1);
    if (scale != 1 && scale != 2)
        scale = 1;
    const char *save = arg_str(args, "save_path", NULL);
    char err[300];
    screen_raw_t sr;
    unsigned char *src = screen_grab(&sr, err, sizeof(err));
    if (!src)
        return result_error("%s", err);
    size_t len = 0;
    unsigned char *png = screen_png(src, &sr, scale, &len);
    free(src);
    if (!png)
        return result_error("PNG encoding failed");

    cJSON *r = result_new();
    if (save) {
        FILE *f = fopen(save, "wb");
        if (!f || fwrite(png, 1, len, f) != len) {
            if (f)
                fclose(f);
            free(png);
            return result_error("cannot write %s: %s", save, strerror(errno));
        }
        fclose(f);
        result_textf(r, "Saved %s", save);
    }
    result_image(r, png, len, "image/png");
    result_textf(r, "Screen %dx%d (framebuffer page %d of %d, %d bpp), image %dx%d, %u bytes", sr.w, sr.h,
                 sr.page, sr.pages, sr.bpp, sr.w / scale, sr.h / scale, (unsigned)len);
    free(png);
    return r;
}

/* -------------------------------------------------------- press_buttons */

static const struct {
    const char *name;
    int code;
} buttons[] = {
    {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
    {"A", KEY_SPACE}, {"B", KEY_LEFTCTRL}, {"X", KEY_LEFTSHIFT}, {"Y", KEY_LEFTALT},
    {"L1", KEY_E}, {"R1", KEY_T}, {"L2", KEY_TAB}, {"R2", KEY_BACKSPACE},
    {"SELECT", KEY_RIGHTCTRL}, {"START", KEY_ENTER}, {"MENU", KEY_ESC},
    {"POWER", KEY_POWER}, {"VOLUP", KEY_VOLUMEUP}, {"VOLDOWN", KEY_VOLUMEDOWN},
    {NULL, 0}};

const char *button_name(int code)
{
    for (int i = 0; buttons[i].name; i++)
        if (buttons[i].code == code)
            return buttons[i].name;
    return NULL;
}

int button_code(const char *name)
{
    for (int i = 0; buttons[i].name; i++)
        if (strcasecmp(buttons[i].name, name) == 0)
            return buttons[i].code;
    return -1;
}

static int emit(int fd, int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

/*
 * Sequence: tokens separated by spaces.
 *   A            press and release A
 *   MENU+POWER   press both together
 *   DOWN*3       three times
 *   A@1500       hold for 1500 ms
 *   WAIT:500     pause 500 ms
 */
static cJSON *tool_press_buttons(cJSON *args)
{
    const char *seq = arg_str(args, "sequence", NULL);
    if (!seq || !*seq)
        return result_error("missing 'sequence'");
    int hold = (int)arg_num(args, "hold_ms", 80);
    int gap = (int)arg_num(args, "gap_ms", 150);
    if (hold < 20)
        hold = 20;
    if (gap < 20)
        gap = 20;

    const char *dev = getenv("CA_INPUT") ? getenv("CA_INPUT") : "/dev/input/event0";
    int fd = open(dev, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return result_error("cannot open %s: %s", dev, strerror(errno));

    char *copy = strdup(seq), *save = NULL;
    int presses = 0;
    long long t0 = now_ms();
    cJSON *err = NULL;
    for (char *tok = strtok_r(copy, " \t\n,", &save); tok && !err;
         tok = strtok_r(NULL, " \t\n,", &save)) {
        if (strncasecmp(tok, "WAIT:", 5) == 0) {
            int ms = atoi(tok + 5);
            sleep_ms(ms > 60000 ? 60000 : ms);
            continue;
        }
        int times = 1, this_hold = hold;
        char *at = strchr(tok, '@');
        if (at) {
            *at = 0;
            this_hold = atoi(at + 1);
            if (this_hold > 30000)
                this_hold = 30000;
        }
        char *star = strchr(tok, '*');
        if (star) {
            *star = 0;
            times = atoi(star + 1);
            if (times < 1)
                times = 1;
            if (times > 100)
                times = 100;
        }
        int codes[8], n = 0;
        char *s2 = NULL;
        for (char *b = strtok_r(tok, "+", &s2); b; b = strtok_r(NULL, "+", &s2)) {
            int c = button_code(b);
            if (c < 0) {
                err = result_error("unknown button '%s' (use UP DOWN LEFT RIGHT A B X Y L1 R1 "
                                   "L2 R2 SELECT START MENU POWER VOLUP VOLDOWN)", b);
                break;
            }
            if (n < 8)
                codes[n++] = c;
        }
        for (int k = 0; k < times && !err; k++) {
            for (int i = 0; i < n; i++)
                emit(fd, EV_KEY, codes[i], 1);
            emit(fd, EV_SYN, SYN_REPORT, 0);
            sleep_ms(this_hold);
            for (int i = n - 1; i >= 0; i--)
                emit(fd, EV_KEY, codes[i], 0);
            emit(fd, EV_SYN, SYN_REPORT, 0);
            presses++;
            sleep_ms(gap);
        }
    }
    free(copy);
    close(fd);
    if (err)
        return err;
    cJSON *r = result_new();
    result_textf(r, "Pressed %d time(s) in %lld ms: %s", presses, now_ms() - t0, seq);
    return r;
}

/* ---------------------------------------------------------- run_command */

#define OUT_MAX (48 * 1024)

static cJSON *tool_run_command(cJSON *args)
{
    const char *cmd = arg_str(args, "command", NULL);
    if (!cmd || !*cmd)
        return result_error("missing 'command'");
    const char *bad = command_blocked(cmd);
    if (bad)
        return result_error("refused: the command mentions '%s'. Writing to the internal flash "
                            "or to raw disks can brick the device or wipe the SD card.", bad);
    int timeout = (int)arg_num(args, "timeout_s", 30);
    if (timeout < 1)
        timeout = 1;
    if (timeout > 600)
        timeout = 600;
    const char *cwd = arg_str(args, "cwd", SD);

    int pfd[2];
    if (pipe(pfd) != 0)
        return result_error("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0)
        return result_error("fork: %s", strerror(errno));
    if (pid == 0) {
        setsid();
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        if (chdir(cwd) != 0) {
            if (chdir("/") != 0) {
            }
        }
        setenv("PATH", SD "/.tmp_update/bin:/usr/bin:/bin:/usr/sbin:/sbin:/customer/app", 1);
        setenv("LD_LIBRARY_PATH", "/lib:/config/lib:" SD "/miyoo/lib:" SD "/.tmp_update/lib", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    /* keep the start and the end of a long output */
    char *head = malloc(OUT_MAX / 2 + 1), *ring = malloc(OUT_MAX / 2);
    size_t hlen = 0, rlen = 0, rpos = 0, total = 0;
    long long deadline = now_ms() + (long long)timeout * 1000;
    int timed_out = 0;
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) {
            timed_out = 1;
            kill(-pid, SIGKILL);
            break;
        }
        struct pollfd p = {pfd[0], POLLIN, 0};
        int pr = poll(&p, 1, left > 1000 ? 1000 : (int)left);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        char buf[4096];
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        if (n <= 0)
            break;
        total += n;
        for (ssize_t i = 0; i < n; i++) {
            if (hlen < OUT_MAX / 2)
                head[hlen++] = buf[i];
            else {
                ring[rpos] = buf[i];
                rpos = (rpos + 1) % (OUT_MAX / 2);
                if (rlen < OUT_MAX / 2)
                    rlen++;
            }
        }
    }
    close(pfd[0]);
    int status = 0;
    if (!timed_out) {
        /* the pipe closed; give the process the rest of the time to exit */
        while (waitpid(pid, &status, WNOHANG) == 0) {
            if (now_ms() > deadline) {
                timed_out = 1;
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            sleep_ms(20);
        }
    }
    else
        waitpid(pid, &status, 0);

    size_t outsz = hlen + rlen + 128;
    char *out = malloc(outsz + 1), *o = out;
    memcpy(o, head, hlen);
    o += hlen;
    if (total > hlen + rlen)
        o += sprintf(o, "\n[... %u bytes cut ...]\n", (unsigned)(total - hlen - rlen));
    size_t start = (rlen < OUT_MAX / 2) ? 0 : rpos;
    for (size_t i = 0; i < rlen; i++)
        *o++ = ring[(start + i) % (OUT_MAX / 2)];
    *o = 0;
    /* JSON needs valid text: replace bytes that are not printable ASCII/UTF-8-ish */
    for (char *q = out; *q; q++)
        if ((unsigned char)*q < 32 && *q != '\n' && *q != '\t' && *q != '\r')
            *q = '?';
    free(head);
    free(ring);

    cJSON *r = result_new();
    result_text(r, out[0] ? out : "(no output)");
    if (timed_out)
        result_textf(r, "Timed out after %d s; the command was killed.", timeout);
    else if (WIFEXITED(status))
        result_textf(r, "Exit code %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        result_textf(r, "Killed by signal %d", WTERMSIG(status));
    if (timed_out || (WIFEXITED(status) && WEXITSTATUS(status) != 0))
        cJSON_ReplaceItemInObject(r, "isError", cJSON_CreateBool(1));
    free(out);
    return r;
}

/* ---------------------------------------------------------------- files */

static cJSON *tool_read_file(cJSON *args)
{
    const char *path = arg_str(args, "path", NULL);
    if (!path)
        return result_error("missing 'path'");
    long offset = (long)arg_num(args, "offset", 0);
    long maxb = (long)arg_num(args, "max_bytes", 64 * 1024);
    if (maxb > 512 * 1024)
        maxb = 512 * 1024;
    if (maxb < 1)
        maxb = 1;
    const char *enc = arg_str(args, "encoding", "text");

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return result_error("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return result_error("%s is a folder; use list_dir", path);
    }
    unsigned char *buf = malloc(maxb + 1);
    ssize_t n = pread(fd, buf, maxb, offset);
    close(fd);
    if (n < 0) {
        free(buf);
        return result_error("read %s: %s", path, strerror(errno));
    }
    buf[n] = 0;
    cJSON *r = result_new();
    if (strcmp(enc, "base64") == 0) {
        char *b = b64_encode(buf, n);
        result_text(r, b ? b : "");
        free(b);
    }
    else {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == 0 || (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\t' && buf[i] != '\r'))
                buf[i] = '?';
        result_text(r, (char *)buf);
    }
    long long size = S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
    result_textf(r, "[%s: bytes %ld to %ld of %lld]", path, offset, offset + (long)n, size);
    free(buf);
    return r;
}

/* Copies an existing file to backups/<date-time>/<full path> before it changes. */
int backup_file(const char *path, char *where, size_t wsize)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0; /* nothing to keep */
    if (strncmp(path, "/tmp/", 5) == 0)
        return 0;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32], dst[PATH_MAX], dir[PATH_MAX];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(dst, sizeof(dst), "%s/backups/%s%s", g_home, stamp, path);
    for (int k = 2; access(dst, F_OK) == 0 && k < 100; k++) /* two changes in the same second */
        snprintf(dst, sizeof(dst), "%s/backups/%s-%d%s", g_home, stamp, k, path);
    snprintf(dir, sizeof(dir), "%s", dst);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    if (mkdirs(dir) != 0)
        return -1;
    int in = open(path, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) {
        if (in >= 0)
            close(in);
        if (out >= 0)
            close(out);
        return -1;
    }
    char buf[16384];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n) {
            rc = -1;
            break;
        }
    close(in);
    close(out);
    snprintf(where, wsize, "%s", dst);
    return rc;
}

static cJSON *tool_write_file(cJSON *args)
{
    const char *path = arg_str(args, "path", NULL);
    const char *content = arg_str(args, "content", NULL);
    if (!path || !content)
        return result_error("missing 'path' or 'content'");
    if (path[0] != '/')
        return result_error("use a full path starting with /");
    if (write_path_blocked(path))
        return result_error("refused: writing to %s is not allowed (system or device file)", path);
    const char *enc = arg_str(args, "encoding", "text");
    int append = arg_bool(args, "append", 0);

    unsigned char *data;
    size_t len;
    if (strcmp(enc, "base64") == 0) {
        data = b64_decode(content, &len);
        if (!data)
            return result_error("bad base64");
    }
    else {
        len = strlen(content);
        data = (unsigned char *)strdup(content);
    }

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (arg_bool(args, "make_dirs", 1))
            mkdirs(dir);
    }

    char bak[PATH_MAX] = "";
    if (backup_file(path, bak, sizeof(bak)) != 0) {
        free(data);
        return result_error("could not back up %s first; nothing was written", path);
    }

    /* write to a temporary file and rename, so a cut never leaves half a file */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.claude-tmp", path);
    int fd;
    if (append) {
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    else
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(data);
        return result_error("cannot write %s: %s", path, strerror(errno));
    }
    ssize_t w = write(fd, data, len);
    fsync(fd);
    close(fd);
    free(data);
    if (w != (ssize_t)len) {
        if (!append)
            unlink(tmp);
        return result_error("short write on %s", path);
    }
    if (!append) {
        struct stat st;
        if (stat(path, &st) == 0)
            chmod(tmp, st.st_mode & 07777); /* keep it executable if it was */
        if (rename(tmp, path) != 0) {
            unlink(tmp);
            return result_error("rename to %s: %s", path, strerror(errno));
        }
    }
    sync();
    cJSON *r = result_new();
    result_textf(r, "%s %u bytes to %s.%s%s", append ? "Appended" : "Wrote", (unsigned)len, path,
                 bak[0] ? " Previous version saved in " : "", bak);
    agent_log("  wrote %s (%u bytes)%s%s", path, (unsigned)len, bak[0] ? ", backup " : "", bak);
    if (!append || bak[0] || len)
        change_log(bak[0] ? "changed" : "created", path, bak);
    return r;
}

static cJSON *tool_list_dir(cJSON *args)
{
    const char *path = arg_str(args, "path", SD);
    DIR *d = opendir(path);
    if (!d)
        return result_error("cannot open %s: %s", path, strerror(errno));
    size_t cap = 8192, len = 0;
    char *out = malloc(cap);
    out[0] = 0;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        char line[PATH_MAX + 64];
        if (lstat(full, &st) != 0)
            snprintf(line, sizeof(line), "?    %s\n", e->d_name);
        else if (S_ISDIR(st.st_mode))
            snprintf(line, sizeof(line), "dir  %s/\n", e->d_name);
        else if (S_ISLNK(st.st_mode))
            snprintf(line, sizeof(line), "link %s\n", e->d_name);
        else
            snprintf(line, sizeof(line), "file %s  (%lld bytes)\n", e->d_name, (long long)st.st_size);
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        memcpy(out + len, line, ll + 1);
        len += ll;
        if (++count >= 2000) {
            strcat(out, "[stopped at 2000 entries]\n");
            break;
        }
    }
    closedir(d);
    cJSON *r = result_new();
    result_text(r, len ? out : "(empty folder)");
    free(out);
    return r;
}

/* ------------------------------------------------------ background_mode */

static cJSON *tool_background_mode(cJSON *args)
{
    int enable = arg_bool(args, "enable", 1);
    cJSON *r = result_new();
    if (!enable) {
        unlink(BG_FLAG);
        if (ui_running())
            result_text(r, "Background mode is off: the connection ends when the user leaves the Claude Agent app.");
        else {
            /* the app is closed: the connection ends a few seconds after this answer */
            char pidpath[PATH_MAX], buf[32];
            snprintf(pidpath, sizeof(pidpath), "%s/agent.pid", g_home);
            if (read_small(pidpath, buf, sizeof(buf)) > 0 && fork() == 0) {
                setsid();
                sleep(3);
                kill(atoi(buf), SIGTERM);
                _exit(0);
            }
            result_text(r, "Background mode is off. The Claude Agent app is closed, so the connection ends in a few "
                           "seconds; the user has to open the app to connect again.");
        }
        return r;
    }
    if (access(BG_FLAG, F_OK) == 0) {
        result_text(r, "Background mode is already on.");
        return r;
    }
    if (setting_get("background_auto") || access(BG_AUTO_SESSION, F_OK) == 0) {
        FILE *bf = fopen(BG_FLAG, "w");
        if (bf)
            fclose(bf);
        result_text(r, "Background mode is on (the app's settings allow it without asking): the connection stays "
                       "on after leaving the app, until it is turned off or the device restarts.");
        return r;
    }
    if (!ui_running()) {
        cJSON_Delete(r);
        return result_error("The Claude Agent app is not open on the device, so nobody can confirm. "
                            "Ask the user to open it (Apps menu) and try again.");
    }
    unlink(BG_ANSWER);
    FILE *f = fopen(BG_REQUEST, "w");
    if (f)
        fclose(f);
    char ans[32] = "";
    for (int i = 0; i < 600 && !ans[0]; i++) { /* 60 s */
        usleep(100000);
        read_small(BG_ANSWER, ans, sizeof(ans));
    }
    unlink(BG_REQUEST);
    unlink(BG_ANSWER);
    if (!strcmp(ans, "allow"))
        result_text(r, "The user allowed background mode: the connection stays on after leaving the app, "
                       "until it is turned off in the app or the device restarts.");
    else {
        cJSON_Delete(r);
        r = result_error(ans[0] ? "The user denied background mode." : "Nobody answered on the device within 60 s.");
    }
    return r;
}

/* --------------------------------------------------- show_message / delete */

cJSON *tool_show_message(cJSON *args)
{
    const char *text = arg_str(args, "text", NULL);
    if (!text || !*text)
        return result_error("missing 'text'");
    messages_add(text);
    cJSON *r = result_new();
    result_text(r, "Shown on the device screen.");
    return r;
}

/* Claude's plan before a big change, shown on the Miyoo: the person accepts it (A) or not (B). */
cJSON *tool_show_plan(cJSON *args)
{
    const char *title = arg_str(args, "title", NULL);
    cJSON *steps = cJSON_GetObjectItem(args, "steps");
    if (!title || !*title || !cJSON_IsArray(steps) || cJSON_GetArraySize(steps) == 0)
        return result_error("give a 'title' and a list of 'steps'");
    cJSON *r = result_new();
    if (setting_get("plans_skip")) {
        result_text(r, "The person turned \"Show Claude's plan first\" off in the app's settings: go ahead "
                       "without waiting, and say in the chat what you are doing.");
        return r;
    }
    if (!ui_running()) {
        result_text(r, "The Claude Agent app is not on screen, so the plan cannot be shown on the Miyoo. "
                       "Explain the plan in the chat and ask the person there before you start.");
        return r;
    }
    char text[1100] = "";
    cJSON *st;
    int n = 0;
    cJSON_ArrayForEach(st, steps)
    {
        const char *s = cJSON_GetStringValue(st);
        if (!s || !*s || n == 10)
            continue;
        size_t l = strlen(text);
        snprintf(text + l, sizeof(text) - l, "%s%s", n ? "\v" : "", s);
        n++;
    }
    char why[600];
    const char *note = arg_str(args, "note", "");
    if (perm_ask("plan", title, text, note, 0, why, sizeof(why)))
        result_text(r, "The person pressed A (Go ahead) on the Miyoo: you can start. Keep the Activity and "
                       "show_message updated as you go.");
    else
        result_textf(r, "Not approved: %s Do not start these changes; ask the person in the chat what they "
                        "prefer.",
                     why);
    return r;
}

/* Moves a file or folder into backups/<date-time>/<full path> instead of erasing it. */
cJSON *tool_delete_path(cJSON *args)
{
    const char *path = arg_str(args, "path", NULL);
    if (!path || path[0] != '/')
        return result_error("use a full path starting with /");
    if (write_path_blocked(path))
        return result_error("refused: %s is a system or device path", path);
    int depth = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1] && p[1] != '/')
            depth++;
    if (strncmp(path, SD "/", strlen(SD) + 1) != 0 || depth < 3 || strstr(path, "/..") ||
        !strncmp(path, g_home, strlen(g_home)))
        return result_error("refused: only files and folders inside /mnt/SDCARD (at least two levels deep, "
                            "and not the Claude Agent folder itself) can be deleted");
    struct stat st;
    if (lstat(path, &st) != 0)
        return result_error("%s does not exist", path);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32], dst[PATH_MAX], dir[PATH_MAX];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    snprintf(dst, sizeof(dst), "%s/backups/%s-deleted%s", g_home, stamp, path);
    for (int k = 2; access(dst, F_OK) == 0 && k < 100; k++)
        snprintf(dst, sizeof(dst), "%s/backups/%s-deleted-%d%s", g_home, stamp, k, path);
    snprintf(dir, sizeof(dir), "%s", dst);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    if (mkdirs(dir) != 0 || rename(path, dst) != 0)
        return result_error("could not delete %s: %s", path, strerror(errno));
    sync();
    cJSON *r = result_new();
    result_textf(r, "Deleted %s. It can be brought back from %s", path, dst);
    agent_log("  deleted %s -> %s", path, dst);
    change_log("deleted", path, dst);
    return r;
}

/* --------------------------------------------------------------- tools */

typedef cJSON *(*tool_fn)(cJSON *);

static const struct {
    const char *name;
    tool_fn fn;
    const char *description;
    const char *schema;
    int dev; /* only with Settings > Developer options > Developer tools */
} tools[] = {
    {"device_info", tool_device_info,
     "Model, firmware (OnionOS or RedOnion), battery, CPU clock, memory, SD card space, "
     "IP address, what is on screen, clock.",
     "{\"type\":\"object\",\"properties\":{}}", 0},
    {"screenshot", tool_screenshot,
     "Picture of what is on the device screen right now (PNG, already turned upright).",
     "{\"type\":\"object\",\"properties\":{"
     "\"scale\":{\"type\":\"integer\",\"enum\":[1,2],\"description\":\"1 = full size, 2 = half size (smaller)\"},"
     "\"save_path\":{\"type\":\"string\",\"description\":\"optional: also save the PNG on the device\"}}}", 0},
    {"press_buttons", tool_press_buttons,
     "Press device buttons as if the user did. Sequence of tokens separated by spaces: "
     "A, MENU+POWER (together), DOWN*3 (repeat), A@1500 (hold 1500 ms), WAIT:500 (pause). "
     "Buttons: UP DOWN LEFT RIGHT A B X Y L1 R1 L2 R2 SELECT START MENU POWER VOLUP VOLDOWN. "
     "POWER may put the device to sleep or turn it off.",
     "{\"type\":\"object\",\"required\":[\"sequence\"],\"properties\":{"
     "\"sequence\":{\"type\":\"string\"},"
     "\"hold_ms\":{\"type\":\"integer\",\"description\":\"default 80\"},"
     "\"gap_ms\":{\"type\":\"integer\",\"description\":\"pause between presses, default 150\"}}}", 0},
    {"run_command", tool_run_command,
     "Run a shell command on the device as root (BusyBox sh). Returns output and exit code. "
     "Commands that touch the internal flash or raw disks are refused.",
     "{\"type\":\"object\",\"required\":[\"command\"],\"properties\":{"
     "\"command\":{\"type\":\"string\"},"
     "\"timeout_s\":{\"type\":\"integer\",\"description\":\"default 30, max 600\"},"
     "\"cwd\":{\"type\":\"string\",\"description\":\"default /mnt/SDCARD\"}}}", 0},
    {"read_file", tool_read_file,
     "Read a file on the device (text, or base64 for binary). Up to 512 KB per call; use offset for more.",
     "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{"
     "\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},"
     "\"max_bytes\":{\"type\":\"integer\",\"description\":\"default 65536\"},"
     "\"encoding\":{\"type\":\"string\",\"enum\":[\"text\",\"base64\"]}}}", 0},
    {"write_file", tool_write_file,
     "Create or replace a file on the device (or append). An existing file is backed up first "
     "in the agent's backups folder.",
     "{\"type\":\"object\",\"required\":[\"path\",\"content\"],\"properties\":{"
     "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},"
     "\"encoding\":{\"type\":\"string\",\"enum\":[\"text\",\"base64\"]},"
     "\"append\":{\"type\":\"boolean\"},\"make_dirs\":{\"type\":\"boolean\"}}}", 0},
    {"background_mode", tool_background_mode,
     "Keep the connection after the user leaves the Claude Agent app (needed to launch and test games), "
     "or turn that off. Turning it on shows a question on the device that the user must accept with A. "
     "It ends by itself when the device restarts.",
     "{\"type\":\"object\",\"properties\":{\"enable\":{\"type\":\"boolean\",\"description\":\"default true\"}}}", 0},
    {"delete_path", tool_delete_path,
     "Delete a file or folder inside /mnt/SDCARD. It is moved to the agent's backups folder, so it can be "
     "brought back. That folder is on the same SD card, so this frees no space: to free space, files must be "
     "deleted for good (run_command), which cannot be undone; say so plainly in the plan (show_plan). The "
     "person can also empty the backups in the app's settings.",
     "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\"}}}", 0},
    {"show_message", tool_show_message,
     "Show a message on the Miyoo screen, in the Claude Agent app. Use it to mirror a short version of every "
     "answer you give the user (what you found, what you did, what they should do), so they can read it on "
     "the device too. Plain text, no Markdown. Write it in the language of the Miyoo's menus (\"language\" in "
     "device_info, like pt-BR or es). The screen shows Latin, Greek and Cyrillic letters only: for Chinese, "
     "Japanese, Korean, Arabic and similar, write in English.",
     "{\"type\":\"object\",\"required\":[\"text\"],\"properties\":{\"text\":{\"type\":\"string\"}}}", 0},
    {"show_plan", tool_show_plan,
     "Before a big or risky change (several files, system settings, installing or removing things, anything "
     "hard to undo), show your plan on the Miyoo screen and wait for the person to press A (go ahead) or B "
     "(not now), up to 90 s. Not needed for small changes or for looking around. Write the title and steps "
     "in the language of the Miyoo's menus (device_info: language), short and plain, for someone who is not "
     "technical. note: what could go wrong and how it can be undone (optional).",
     "{\"type\":\"object\",\"required\":[\"title\",\"steps\"],\"properties\":{\"title\":{\"type\":"
     "\"string\"},\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":8},"
     "\"note\":{\"type\":\"string\"}}}",
     0},
    {"list_dir", tool_list_dir,
     "List a folder on the device (default /mnt/SDCARD).",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}", 0},
    {"download_file", tool_download_file,
     "Developer tool. The Miyoo downloads a file from an https address (for example a GitHub release asset or "
     "raw file) and saves it, checking it against sha256 if given. Use it for anything bigger than a few hundred KB "
     "instead of write_file. An existing file is backed up first. Waits up to 100 s; longer downloads go on in "
     "the background as a job (see job_status).",
     "{\"type\":\"object\",\"required\":[\"url\",\"path\"],\"properties\":{"
     "\"url\":{\"type\":\"string\",\"description\":\"https://...\"},"
     "\"path\":{\"type\":\"string\",\"description\":\"full path on the device, e.g. /mnt/SDCARD/Roms/PORTS/x.zip\"},"
     "\"sha256\":{\"type\":\"string\",\"description\":\"optional: expected checksum (64 hex)\"},"
     "\"executable\":{\"type\":\"boolean\",\"description\":\"make it executable (chmod 755)\"},"
     "\"make_dirs\":{\"type\":\"boolean\",\"description\":\"create missing folders, default true\"},"
     "\"wait\":{\"type\":\"boolean\",\"description\":\"default true; false returns the job id at once\"}}}", 1},
    {"input_record", tool_input_record,
     "Developer tool. Records every button the person presses on the Miyoo, with its time in ms, to replay "
     "it later with input_play exactly the same way (for example to test a game area twice with different "
     "settings). action start (with a name), stop, or list. To record inside a game, background_mode must be on.",
     "{\"type\":\"object\",\"properties\":{"
     "\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\",\"list\"]},"
     "\"name\":{\"type\":\"string\",\"description\":\"letters, digits, - and _\"},"
     "\"max_s\":{\"type\":\"integer\",\"description\":\"stop by itself after this many seconds, default 600\"}}}", 1},
    {"input_play", tool_input_play,
     "Developer tool. Presses buttons with exact timing, from a recording (input_record) or from a timeline: a "
     "list of events with t = ms from the start: {\"t\":0,\"down\":\"RIGHT\"}, {\"t\":1500,\"tap\":\"A\",\"hold\":80}, "
     "{\"t\":3000,\"up\":\"RIGHT\"}, {\"t\":2000,\"shot\":\"label\"} (screenshot). Buttons can overlap freely. "
     "shots_at adds screenshots at given ms. Screenshots are saved as PNG (see them with view_image) without "
     "delaying the buttons. Held buttons are released at the end. Games are not always deterministic: for fair "
     "comparisons start each run from the same save state. Waits up to 100 s, then continues as a job.",
     "{\"type\":\"object\",\"properties\":{"
     "\"recording\":{\"type\":\"string\"},"
     "\"timeline\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},"
     "\"shots_at\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
     "\"shots_dir\":{\"type\":\"string\",\"description\":\"default: the app's shots folder\"},"
     "\"start_at_first_press\":{\"type\":\"boolean\",\"description\":\"recording: skip the wait before the first press\"},"
     "\"allow_power\":{\"type\":\"boolean\"},"
     "\"wait\":{\"type\":\"boolean\",\"description\":\"default true\"}}}", 1},
    {"view_image", tool_view_image,
     "Developer tool. Shows you a picture saved on the device (PNG, JPEG, WEBP or GIF, up to 4 MB), for "
     "example the screenshots taken by input_play.",
     "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\"}}}", 1},
    {"job_status", tool_job_status,
     "Developer tool. Progress or result of a background job (download_file, input_record, input_play, run_app, "
     "perf_monitor, screen_capture, watch_file). "
     "Without id, lists the jobs. cancel true stops it; wait_s waits up to 90 s for it to finish.",
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"cancel\":{\"type\":\"boolean\"},"
     "\"wait_s\":{\"type\":\"integer\"}}}", 1},
    {"run_app", tool_run_app,
     "Developer tool. Opens a port, app or emulator launch script (launch, plus rom for an emulator) or a shell "
     "command the way OnionOS does (the Claude Agent app closes first; background_mode must be on), keeps "
     "everything it prints, and reports how it ended: exit code, or which crash (SIGSEGV...), with the last "
     "output lines, last_frame.png (the screen about 1 s before a crash) and a core dump. Options: timeout_s "
     "(stop it after), watch_for (words to look for in the output) with stop_on_match, shots_every_ms + "
     "shots_start_ms + shots_count (screenshots while it runs), cpu_mhz (fix the CPU clock for the run, e.g. "
     "1600 to test like a Mini or Flip), env (environment variables), core_dump (default true). Combine with "
     "input_play and perf_monitor (wait false) to test the same thing in the same way. Waits up to 100 s, then "
     "continues as a job.",
     "{\"type\":\"object\",\"properties\":{"
     "\"launch\":{\"type\":\"string\"},\"rom\":{\"type\":\"string\"},\"command\":{\"type\":\"string\"},"
     "\"timeout_s\":{\"type\":\"integer\"},\"watch_for\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
     "\"stop_on_match\":{\"type\":\"boolean\"},\"shots_every_ms\":{\"type\":\"integer\"},"
     "\"shots_start_ms\":{\"type\":\"integer\"},\"shots_count\":{\"type\":\"integer\"},"
     "\"cpu_mhz\":{\"type\":\"integer\"},\"env\":{\"type\":\"object\"},\"core_dump\":{\"type\":\"boolean\"},"
     "\"wait\":{\"type\":\"boolean\"}}}", 1},
    {"perf_monitor", tool_perf_monitor,
     "Developer tool. Measures CPU use and clock, free memory, battery and, if 'process' is given (its name, "
     "like retroarch), that program's CPU and memory (shows memory leaks). interval_ms (default 1000), "
     "duration_s (default 60), start_after_ms. Returns min/average/max and a CSV file with every sample.",
     "{\"type\":\"object\",\"properties\":{\"interval_ms\":{\"type\":\"integer\"},\"duration_s\":{\"type\":\"integer\"},"
     "\"start_after_ms\":{\"type\":\"integer\"},\"process\":{\"type\":\"string\"},\"wait\":{\"type\":\"boolean\"}}}", 1},
    {"screen_capture", tool_screen_capture,
     "Developer tool. A series of screenshots without slowing anything down: every_ms (default 1000), "
     "start_after_ms, count (default 10, max 500) and/or duration_s, scale 2 for half size, dir. Look at them "
     "with view_image.",
     "{\"type\":\"object\",\"properties\":{\"every_ms\":{\"type\":\"integer\"},\"start_after_ms\":{\"type\":\"integer\"},"
     "\"count\":{\"type\":\"integer\"},\"duration_s\":{\"type\":\"integer\"},\"scale\":{\"type\":\"integer\"},"
     "\"dir\":{\"type\":\"string\"},\"wait\":{\"type\":\"boolean\"}}}", 1},
    {"watch_file", tool_watch_file,
     "Developer tool. Follows a log file as it grows (for duration_s, default 60) and reports the lines that "
     "contain any of 'patterns' (all new lines if none). stop_on_match ends at the first one; from_start also "
     "reads what is already there.",
     "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"patterns\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"duration_s\":{\"type\":\"integer\"},"
     "\"stop_on_match\":{\"type\":\"boolean\"},\"from_start\":{\"type\":\"boolean\"},\"wait\":{\"type\":\"boolean\"}}}", 1},
    {"snapshot", tool_snapshot,
     "Developer tool. Saves a whole folder or file of the SD card (a port, its saves, a config folder) to put it "
     "back later exactly as it was: action save (path), restore (name), list. Use it before risky experiments.",
     "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"save\",\"restore\",\"list\"]},"
     "\"path\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}}}", 1},
    {"plugin_create", tool_plugin_create,
     "Developer tool. Makes a new tool for yourself on this Miyoo: a plugin (a small sh script and its "
     "description) that appears as plugin_<name> in new chats. Read plugins {\"action\":\"docs\"} first. The "
     "person sees what it does and accepts it on the Miyoo. Use it when a task needs something the other tools "
     "do not do well or will be repeated, and suggest it to the user in plain words first.",
     "{\"type\":\"object\",\"required\":[\"name\",\"title\",\"summary\",\"files\"],\"properties\":{"
     "\"name\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"summary\":{\"type\":\"string\"},"
     "\"description\":{\"type\":\"string\"},\"permission\":{\"type\":\"string\",\"enum\":[\"screen\",\"read\","
     "\"write\",\"control\",\"command\"]},\"input_schema\":{\"type\":\"object\"},\"run\":{\"type\":\"string\"},"
     "\"timeout_s\":{\"type\":\"integer\"},\"files\":{\"type\":\"object\",\"description\":\"file name -> content\"}}}", 1},
    {"plugins", tool_plugins,
     "Developer tool. Plugins on this Miyoo: action list, docs (how to write one), or remove (name; it is kept "
     "in the backups).",
     "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"docs\",\"remove\"]},"
     "\"name\":{\"type\":\"string\"}}}", 1},
    {NULL, NULL, NULL, NULL, 0}};

int dev_tools_on(void)
{
    return setting_get("dev_tools") || access(DEV_SESSION, F_OK) == 0;
}

cJSON *tools_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    int dev = dev_tools_on();
    for (int i = 0; tools[i].name; i++) {
        if (tools[i].dev && !dev)
            continue;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", tools[i].name);
        cJSON_AddStringToObject(t, "description", tools[i].description);
        cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(tools[i].schema));
        cJSON_AddItemToArray(arr, t);
    }
    plugins_add_tools(arr);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "tools", arr);
    return o;
}

/* Runs one tool. Never returns NULL. */
cJSON *agent_call(const char *name, cJSON *args)
{
    cJSON *empty = NULL;
    if (!args)
        args = empty = cJSON_CreateObject();
    char *a = cJSON_PrintUnformatted(args);
    /* the log keeps the call, but not file contents */
    if (a && strlen(a) > 300)
        strcpy(a + 290, "...}");
    agent_log("call %s %s", name, a ? a : "");
    free(a);
    cJSON *r = NULL;
    int found = 0;
    for (int i = 0; tools[i].name; i++)
        if (strcmp(tools[i].name, name) == 0)
            found = tools[i].dev ? 2 : 1;
    if (!found && !strncmp(name, "plugin_", 7) && strcmp(name, "plugin_create")) {
        activity_call(name, args);
        r = plugin_call(name, args); /* asks with the plugin's own setting */
        activity_result(name, r);
    }
    else if (!found)
        r = result_error("unknown tool '%s'", name);
    else if (found == 2 && !dev_tools_on())
        r = result_error("Developer tools are turned off on this Miyoo. If the user wants them, they can turn "
                         "them on in the Claude Agent app: START > Developer options > Developer tools.");
    else {
        activity_call(name, args);
        char why[600];
        if (!perm_check(name, args, why, sizeof(why)))
            r = result_error("%s", why);
        else
            for (int i = 0; tools[i].name; i++)
                if (strcmp(tools[i].name, name) == 0) {
                    r = tools[i].fn(args);
                    break;
                }
        activity_result(name, r);
    }
    cJSON_Delete(empty);
    return r;
}

/* ---------------------------------------------------------------- main */

static void print_result(cJSON *r)
{
    char *s = cJSON_Print(r);
    puts(s);
    free(s);
}

static int usage(void)
{
    fprintf(stderr,
            "Claude Agent " AGENT_VERSION "\n"
            "  claude-agent tools\n"
            "  claude-agent call '{\"tool\":\"...\",\"args\":{...}}'\n"
            "  claude-agent info\n"
            "  claude-agent shot <file.png> [half]\n"
            "  claude-agent keys \"<sequence>\"\n"
            "  claude-agent exec \"<command>\"\n"
            "  claude-agent connect [--pair]\n"
            "  claude-agent unpair\n"
            "  claude-agent ui\n");
    return 2;
}

int main(int argc, char **argv)
{
    /* memory: the Miyoo has 128 MB for everything, and games run next to this agent. Big buffers
       (screenshots, files) go straight back to the system when freed, and threads share one heap. */
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    mallopt(M_ARENA_MAX, 1);
    signal(SIGPIPE, SIG_IGN);
    if (!getenv("TZ")) {
        /* started outside OnionOS's launcher (ssh, scripts): use its time zone, like runtime.sh does */
        char tz[64] = "";
        FILE *f = fopen("/mnt/SDCARD/.tmp_update/config/.tz", "r");
        if (f) {
            if (fgets(tz, sizeof(tz), f))
                tz[strcspn(tz, "\r\n")] = 0;
            fclose(f);
        }
        if (tz[0])
            setenv("TZ", tz, 1);
    }
    tzset();
    if (getenv("CA_HOME"))
        snprintf(g_home, sizeof(g_home), "%s", getenv("CA_HOME"));
    else {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = 0;
            char *s = strrchr(exe, '/');
            if (s)
                *s = 0;
            snprintf(g_home, sizeof(g_home), "%s", exe);
        }
        else
            snprintf(g_home, sizeof(g_home), SD "/App/ClaudeAgent");
    }
    if (argc < 2)
        return usage();

    const char *cmd = argv[1];
    cJSON *r = NULL;
    if (!strcmp(cmd, "tools")) {
        r = tools_list();
    }
    else if (!strcmp(cmd, "call") && argc >= 3) {
        cJSON *req = cJSON_Parse(argv[2]);
        if (!req) {
            fprintf(stderr, "invalid JSON\n");
            return 2;
        }
        r = agent_call(arg_str(req, "tool", ""), cJSON_GetObjectItem(req, "args"));
        cJSON_Delete(req);
    }
    else if (!strcmp(cmd, "info")) {
        r = agent_call("device_info", NULL);
        /* print the JSON text itself, easier to read */
        cJSON *c = cJSON_GetArrayItem(cJSON_GetObjectItem(r, "content"), 0);
        puts(cJSON_GetObjectItem(c, "text")->valuestring);
        cJSON_Delete(r);
        return 0;
    }
    else if (!strcmp(cmd, "shot") && argc >= 3) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "save_path", argv[2]);
        if (argc >= 4 && !strcmp(argv[3], "half"))
            cJSON_AddNumberToObject(a, "scale", 2);
        r = agent_call("screenshot", a);
        cJSON_Delete(a);
        /* drop the image data from the printout */
        cJSON *content = cJSON_GetObjectItem(r, "content");
        for (int i = cJSON_GetArraySize(content) - 1; i >= 0; i--) {
            cJSON *c = cJSON_GetArrayItem(content, i);
            if (!strcmp(arg_str(c, "type", ""), "image"))
                cJSON_DeleteItemFromArray(content, i);
        }
    }
    else if (!strcmp(cmd, "ui")) {
        return ui_main();
    }
    else if (!strcmp(cmd, "update")) {
        return update_main(argc, argv);
    }
    else if (!strcmp(cmd, "connect") || !strcmp(cmd, "unpair") || !strcmp(cmd, "server")) {
        return net_main(argc, argv);
    }
    else if (!strcmp(cmd, "keys") && argc >= 3) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "sequence", argv[2]);
        r = agent_call("press_buttons", a);
        cJSON_Delete(a);
    }
    else if (!strcmp(cmd, "exec") && argc >= 3) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "command", argv[2]);
        r = agent_call("run_command", a);
        cJSON_Delete(a);
    }
    else
        return usage();

    int err = cJSON_IsTrue(cJSON_GetObjectItem(r, "isError"));
    print_result(r);
    cJSON_Delete(r);
    return err ? 1 : 0;
}
