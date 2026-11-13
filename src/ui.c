/*
 * Claude Agent: the app screen.
 *
 *   claude-agent ui      started by launch.sh from the Apps menu
 *
 * The screen starts the connection ("claude-agent connect") as a separate
 * process and shows:
 *   - the pairing code, the first time;
 *   - "Claude": the messages Claude sends with the show_message tool;
 *   - "Activity": what the agent is doing, like a terminal;
 *   - the permission questions, and the settings (START).
 * Buttons on the main screen: UP/DOWN scroll, L1/R1 or LEFT/RIGHT switch
 * between the two areas, START settings, B or MENU leave the app.
 *
 * Drawn straight into /dev/fb0 (the panel is upside down, as in OnionOS)
 * with the system font of the Miyoo firmware.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include "qrcodegen.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include <assert.h>
#include "stb_truetype.h"

#include "common.h"

/* ------------------------------------------------------------- screen */

static int fb_fd = -1;
static uint8_t *fb_mem = NULL;
static size_t fb_len = 0;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint32_t *canvas = NULL; /* upright picture, W x H */
static int W = 640, H = 480;

static int screen_open(void)
{
    const char *dev = getenv("CA_FB") ? getenv("CA_FB") : "/dev/fb0";
    fb_fd = open(dev, O_RDWR);
    if (fb_fd < 0)
        return -1;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi)) {
        /* tests: a plain file and CA_FB_MODE=WxHx32 */
        unsigned w, h, b;
        const char *m = getenv("CA_FB_MODE");
        if (!m || sscanf(m, "%ux%ux%u", &w, &h, &b) != 3)
            return -1;
        memset(&vi, 0, sizeof(vi));
        vi.xres = vi.xres_virtual = w;
        vi.yres = h;
        vi.yres_virtual = h * 2;
        vi.bits_per_pixel = b;
        fi.line_length = w * 4;
    }
    if (vi.bits_per_pixel != 32)
        return -1;
    if (vi.yoffset && !getenv("CA_FB_MODE")) {
        /* the low battery icon is drawn on the first page only: show that page */
        vi.yoffset = 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vi);
    }
    W = vi.xres;
    H = vi.yres;
    fb_len = (size_t)fi.line_length * vi.yres_virtual;
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED)
        return -1;
    canvas = malloc((size_t)W * H * 4);
    return canvas ? 0 : -1;
}

/* copies the canvas to the page on screen, turned 180 degrees */
static void screen_present(void)
{
    if (!getenv("CA_FB_MODE"))
        ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi);
    if ((int)vi.xres != W || (int)vi.yres != H)
        return;
    uint8_t *page = fb_mem + (size_t)vi.yoffset * fi.line_length;
    for (int y = 0; y < H; y++) {
        uint32_t *dst = (uint32_t *)(page + (size_t)(H - 1 - y) * fi.line_length);
        const uint32_t *src = canvas + (size_t)y * W;
        for (int x = 0; x < W; x++)
            dst[W - 1 - x] = src[x];
    }
}

static int clip_y0 = 0, clip_y1 = 1 << 30; /* text is clipped to a pane */

static void fill(int x, int y, int w, int h, uint32_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            canvas[(size_t)j * W + i] = c;
}

static uint32_t blend(uint32_t bg, uint32_t fg, int a)
{
    uint32_t r = (((fg >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((fg >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
    uint32_t b = ((fg & 255) * a + (bg & 255) * (255 - a)) / 255;
    return 0xff000000 | (r << 16) | (g << 8) | b;
}

/* rounded box with soft (anti-aliased) corners */
static void round_box(int x, int y, int w, int h, int r, uint32_t c)
{
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H)
            continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= W)
                continue;
            float dx = i < r ? r - i - 0.5f : (i >= w - r ? i - (w - r) + 0.5f : 0);
            float dy = j < r ? r - j - 0.5f : (j >= h - r ? j - (h - r) + 0.5f : 0);
            float d2 = dx * dx + dy * dy;
            uint32_t *p = &canvas[(size_t)py * W + px];
            if (d2 <= (r - 1) * (r - 1))
                *p = c;
            else if (d2 < r * r) {
                float a = r - __builtin_sqrtf(d2);
                *p = blend(*p, c, (int)(a * 255));
            }
        }
    }
}

static void round_outline(int x, int y, int w, int h, int r, uint32_t c, uint32_t inside)
{
    round_box(x, y, w, h, r, c);
    round_box(x + 2, y + 2, w - 4, h - 4, r - 2, inside);
}

/* --------------------------------------------------------------- text */

static stbtt_fontinfo font;
static unsigned char *font_data = NULL;

static int font_load(void)
{
    const char *paths[] = {"/customer/app/Helvetica-Neue-2.ttf", "/customer/app/Exo-2-Bold-Italic.ttf", NULL};
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        font_data = malloc(n);
        if (font_data && fread(font_data, 1, n, f) == (size_t)n &&
            stbtt_InitFont(&font, font_data, stbtt_GetFontOffsetForIndex(font_data, 0))) {
            fclose(f);
            return 0;
        }
        fclose(f);
        free(font_data);
        font_data = NULL;
    }
    return -1;
}

/* next character of a UTF-8 string (Latin-1 range is enough for the font) */
static int utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    int c = *s++;
    if (c >= 0xc0 && (*s & 0xc0) == 0x80) {
        if (c < 0xe0) {
            c = ((c & 0x1f) << 6) | (*s++ & 0x3f);
        }
        else if (c < 0xf0 && (s[1] & 0xc0) == 0x80) {
            c = ((c & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f);
            s += 2;
        }
        else {
            while ((*s & 0xc0) == 0x80)
                s++;
            c = '?';
        }
    }
    *p = s;
    return c;
}

/* QR code for the help page, drawn black on a white card; made once per address */
static void qr_draw(int x, int y, int size, const char *link)
{
    static char made_for[600];
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    static int ok = 0;
    if (strcmp(made_for, link)) {
        uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
        snprintf(made_for, sizeof(made_for), "%s", link);
        ok = qrcodegen_encodeText(link, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
    }
    round_box(x, y, size, size, 14, 0xffffffff);
    if (!ok)
        return;
    int n = qrcodegen_getSize(qr);
    int m = (size - 16) / n; /* whole pixels per module keep it sharp */
    int ox = x + (size - m * n) / 2, oy = y + (size - m * n) / 2;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            if (qrcodegen_getModule(qr, i, j))
                fill(ox + i * m, oy + j * m, m, m, 0xff1a1a1a);
}

static int text_width_raw(const char *str, float px)
{
    if (!font_data)
        return 0;
    float scale = stbtt_ScaleForPixelHeight(&font, px);
    int w = 0, prev = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        int c = utf8_next(&p);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, c, &adv, &lsb);
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&font, prev, c);
        w += adv;
        prev = c;
    }
    return (int)(w * scale + 0.5f);
}

/* draws text with its top-left at (x, y); returns the width */
static int text_raw(int x, int y, const char *str, float px, uint32_t color)
{
    if (!font_data)
        return 0;
    float scale = stbtt_ScaleForPixelHeight(&font, px);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    int base = y + (int)(ascent * scale);
    float pen = x;
    int prev = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        int c = utf8_next(&p);
        if (prev)
            pen += stbtt_GetCodepointKernAdvance(&font, prev, c) * scale;
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&font, c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&font, c, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *bmp = malloc((size_t)gw * gh);
            stbtt_MakeCodepointBitmap(&font, bmp, gw, gh, gw, scale, scale, c);
            for (int j = 0; j < gh; j++) {
                int py = base + y0 + j;
                if (py < 0 || py >= H || py < clip_y0 || py >= clip_y1)
                    continue;
                for (int i = 0; i < gw; i++) {
                    int a = bmp[j * gw + i];
                    int px_ = (int)pen + x0 + i;
                    if (a && px_ >= 0 && px_ < W) {
                        uint32_t *d = &canvas[(size_t)py * W + px_];
                        *d = blend(*d, color, a);
                    }
                }
            }
            free(bmp);
        }
        pen += adv * scale;
        prev = c;
    }
    return (int)(pen - x);
}

/* the same, for texts of the app: shown in the Miyoo's language */
static int text_width(const char *str, float px)
{
    return text_width_raw(T(str), px);
}

static int text(int x, int y, const char *str, float px, uint32_t color)
{
    return text_raw(x, y, T(str), px, color);
}

static void text_center(int cx, int y, const char *s, float px, uint32_t color)
{
    text(cx - text_width(s, px) / 2, y, s, px, color);
}

/* ----------------------------------------------------- wrapped lines */

typedef struct {
    char s[200];
    uint8_t kind; /* see LK_ */
} line_t;

enum { LK_BODY, LK_HEAD, LK_TIME, LK_FAIL, LK_GAP, LK_USER, LK_UBODY };

typedef struct {
    line_t *v;
    int n, cap;
} lines_t;

static void lines_add(lines_t *L, const char *s, int kind)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 128;
        L->v = realloc(L->v, sizeof(line_t) * L->cap);
    }
    snprintf(L->v[L->n].s, sizeof(L->v[L->n].s), "%s", s);
    L->v[L->n].kind = kind;
    L->n++;
}

/* splits a paragraph into lines that fit width w (breaks at spaces, or anywhere in very long words) */
static void wrap_into(lines_t *L, const char *para, float px, int w, int kind)
{
    char line[200] = "";
    const char *p = T(para);
    if (!*p) {
        lines_add(L, "", kind);
        return;
    }
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        char word[200];
        int n = 0;
        while (p[n] && p[n] != ' ' && n < 190)
            n++;
        memcpy(word, p, n);
        word[n] = 0;
        char trial[400];
        snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
        if (text_width_raw(trial, px) <= w) {
            snprintf(line, sizeof(line), "%s", trial);
            p += n;
            continue;
        }
        if (line[0]) {
            lines_add(L, line, kind);
            line[0] = 0;
            continue;
        }
        /* a single word wider than the line: cut it */
        int cut = n;
        while (cut > 1) {
            word[cut] = 0;
            if (text_width_raw(word, px) <= w)
                break;
            cut--;
        }
        lines_add(L, word, kind);
        p += cut;
    }
    if (line[0])
        lines_add(L, line, kind);
}

/* ------------------------------------------------------------ panes */

typedef struct {
    const char *title;
    const char *file;
    int x, y, w, h;
    float px;
    int line_h;
    lines_t lines;
    int top;    /* first visible line */
    int follow; /* stays on the newest line */
    long size;  /* file size when last read */
    time_t mtime;
} pane_t;

static int pane_rows(const pane_t *p)
{
    return (p->h - 40) / p->line_h;
}

static void pane_clamp(pane_t *p)
{
    int max = p->lines.n - pane_rows(p);
    if (max < 0)
        max = 0;
    if (p->follow || p->top > max)
        p->top = max;
    if (p->top < 0)
        p->top = 0;
    if (p->top >= max)
        p->follow = 1;
}

/* re-reads the log behind a pane when it changed; returns 1 if it did */
static int pane_load(pane_t *p, int messages)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/%s", g_home, p->file);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (p->lines.n == 0 && p->size == 0)
            return 0;
        p->lines.n = 0;
        p->size = 0;
        return 1;
    }
    if (st.st_size == p->size && st.st_mtime == p->mtime)
        return 0;
    p->size = st.st_size;
    p->mtime = st.st_mtime;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    p->lines.n = 0;
    char *buf = NULL;
    size_t cap = 0;
    int text_w = p->w - 32;
    while (getline(&buf, &cap, f) > 0) {
        buf[strcspn(buf, "\n")] = 0;
        char *tab = strchr(buf, '\t');
        if (!tab)
            continue;
        *tab = 0;
        const char *when = buf, *msg = tab + 1;
        if (messages) {
            /* \x01 first = what the person typed on the Miyoo */
            int mine = msg[0] == '\x01';
            char head[64];
            snprintf(head, sizeof(head), "%s  %.5s", mine ? T("You") : "Claude", when);
            if (p->lines.n)
                lines_add(&p->lines, "", LK_GAP);
            lines_add(&p->lines, head, mine ? LK_USER : LK_HEAD);
            char *copy = strdup(msg + mine), *save = NULL;
            for (char *para = strtok_r(copy, "\v", &save); para; para = strtok_r(NULL, "\v", &save))
                wrap_into(&p->lines, para, p->px, text_w, mine ? LK_UBODY : LK_BODY);
            free(copy);
        }
        else {
            char first[260];
            int fail = !strncmp(msg, "  failed", 8);
            snprintf(first, sizeof(first), "%s  %s", when, msg);
            lines_t tmp = {0};
            wrap_into(&tmp, first, p->px, text_w, fail ? LK_FAIL : LK_BODY);
            for (int i = 0; i < tmp.n; i++)
                lines_add(&p->lines, tmp.v[i].s, i == 0 ? (fail ? LK_FAIL : LK_TIME) : tmp.v[i].kind);
            free(tmp.v);
        }
    }
    free(buf);
    fclose(f);
    pane_clamp(p);
    return 1;
}

/* ------------------------------------------------------------ colors */

#define C_BG 0xff181715
#define C_PANEL 0xff262522
#define C_PANEL2 0xff302e2b
#define C_LINE 0xff3a3834
#define C_TEXT 0xfff2efe8
#define C_DIM 0xff9a978e
#define C_ACCENT 0xffd97757
#define C_OK 0xff86b86f
#define C_WARN 0xffe3b450
#define C_BAD 0xffe5705d

static void pane_draw(pane_t *p, int focused)
{
    round_box(p->x, p->y, p->w, p->h, 14, C_PANEL);
    if (focused)
        round_box(p->x, p->y, 4, p->h, 2, C_ACCENT);
    text(p->x + 16, p->y + 8, p->title, 17, focused ? C_ACCENT : C_DIM);
    clip_y0 = p->y + 34;
    clip_y1 = p->y + p->h - 6;
    int rows = pane_rows(p);
    if (p->lines.n == 0) {
        lines_t E = {0};
        if (!strcmp(p->file, "messages.log"))
            wrap_into(&E,
                      "Nothing here yet. To get help, open Claude on your phone or computer, start a chat with the "
                      "Miyoo connector turned on, and ask anything about this Miyoo, like \"my game does not "
                      "open\". Claude's answers will also appear here.",
                      p->px - 1, p->w - 32, LK_BODY);
        else
            wrap_into(&E, "Everything Claude does on this Miyoo will be listed here, step by step.", p->px,
                      p->w - 32, LK_BODY);
        for (int i = 0; i < E.n && i < rows; i++)
            text_raw(p->x + 16, p->y + 38 + i * p->line_h, E.v[i].s, p->px - 1, C_DIM);
        free(E.v);
    }
    for (int i = 0; i < rows && p->top + i < p->lines.n; i++) {
        line_t *l = &p->lines.v[p->top + i];
        int y = p->y + 36 + i * p->line_h;
        uint32_t c = C_TEXT;
        if (l->kind == LK_HEAD)
            c = C_ACCENT;
        else if (l->kind == LK_USER || l->kind == LK_UBODY)
            c = C_DIM;
        else if (l->kind == LK_FAIL)
            c = C_BAD;
        if (l->kind == LK_TIME && strlen(l->s) > 9) {
            /* "12:34:56  text": the time in grey */
            char t[16];
            snprintf(t, sizeof(t), "%.8s", l->s);
            int w = text_raw(p->x + 16, y, t, p->px, C_DIM);
            text_raw(p->x + 16 + w, y, l->s + 8, p->px, C_TEXT);
        }
        else
            text_raw(p->x + 16, y, l->s, p->px, c);
    }
    clip_y0 = 0;
    clip_y1 = 1 << 30;
    /* scroll bar */
    if (p->lines.n > rows) {
        int track = p->h - 48;
        int bar = track * rows / p->lines.n;
        if (bar < 16)
            bar = 16;
        int pos = (track - bar) * p->top / (p->lines.n - rows);
        round_box(p->x + p->w - 10, p->y + 38, 4, track, 2, C_LINE);
        round_box(p->x + p->w - 10, p->y + 38 + pos, 4, bar, 2, focused ? C_ACCENT : C_DIM);
    }
    if (!p->follow) {
        const char *more = "newer below";
        int w = text_width(more, 14) + 16;
        round_box(p->x + p->w - w - 18, p->y + 8, w, 20, 10, C_PANEL2);
        text(p->x + p->w - w - 10, p->y + 10, more, 14, C_DIM);
    }
}

/* ----------------------------------------------------------- helpers */

static int read_small(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = 0;
    return (int)n;
}

static void kv_get(const char *text_, const char *key, char *out, size_t size)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = text_; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t n = 0;
            while (*p && *p != '\n' && n < size - 1)
                out[n++] = *p++;
            out[n] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
}

static pid_t agent_pid(void)
{
    char path[1100], buf[32];
    snprintf(path, sizeof(path), "%s/agent.pid", g_home);
    if (read_small(path, buf, sizeof(buf)) <= 0)
        return 0;
    pid_t p = atoi(buf);
    return (p > 1 && kill(p, 0) == 0) ? p : 0;
}

/* starts "claude-agent connect" in its own session, so it can outlive the app */
/* runs "claude-agent a1 a2" detached, output to logs/<log> */
static void spawn_self(const char *log_name, const char *a1, const char *a2, const char *a3)
{
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);
        char log[1100];
        snprintf(log, sizeof(log), "%s/logs/%s", g_home, log_name);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        execl(exe, "claude-agent", a1, a2, a3, (char *)NULL);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

/* UPDATE_STATUS value of key */
static void update_get(const char *key, char *out, size_t size)
{
    char buf[2048];
    out[0] = 0;
    if (read_small(UPDATE_STATUS, buf, sizeof(buf)) < 0)
        return;
    size_t kl = strlen(key);
    for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            snprintf(out, size, "%.*s", (int)strcspn(l + kl + 1, "\n"), l + kl + 1);
            return;
        }
}

static void agent_start(int want_pair)
{
    if (agent_pid())
        return;
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);
        char log[1100];
        snprintf(log, sizeof(log), "%s/logs/agent.out", g_home);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        if (want_pair)
            execl(exe, "claude-agent", "connect", "--pair", (char *)NULL);
        else
            execl(exe, "claude-agent", "connect", (char *)NULL);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

static void agent_stop(void)
{
    pid_t p = agent_pid();
    if (!p)
        return;
    kill(p, SIGTERM);
    for (int i = 0; i < 30 && kill(p, 0) == 0; i++)
        usleep(100000);
}

static int has_ip(char *ip, size_t size)
{
    if (getenv("CA_TEST_IP")) {
        snprintf(ip, size, "%s", getenv("CA_TEST_IP"));
        return 1;
    }
    FILE *f = popen("ip route get 1 2>/dev/null | awk '{print $NF;exit}'", "r");
    if (!f)
        return 0;
    ip[0] = 0;
    if (fgets(ip, size, f))
        ip[strcspn(ip, "\r\n ")] = 0;
    pclose(f);
    return ip[0] != 0;
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

/* ------------------------------------------------------------- input */

static int input_fd = -1;

/* returns a key code that was pressed (or repeated, for UP/DOWN), or 0 */
/* Keys shared with the system (keymon): MENU+POWER takes a screenshot, MENU+VOL sets the
 * brightness, VOL shows the volume bar. MENU leaves the app only when it is released alone. */
static long long sys_redraw_at = 0;
static int menu_down = 0, menu_combo = 0;

static int input_poll(int timeout_ms)
{
    struct pollfd p = {input_fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
        return 0;
    struct input_event ev;
    int got = 0;
    while (!got && read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type != EV_KEY)
            continue;
        /* the volume and brightness bars put back what was behind them after 2 s: draw again then */
        sys_redraw_at = now_ms() + 2300;
        if (ev.code == KEY_ESC) {
            if (ev.value == 1) {
                menu_down = 1;
                menu_combo = 0;
            }
            else if (ev.value == 0) {
                if (menu_down && !menu_combo)
                    got = KEY_ESC;
                menu_down = 0;
            }
            continue;
        }
        if (ev.value == 1 && menu_down) {
            menu_combo = 1; /* a system shortcut, not for the app */
            continue;
        }
        if (ev.code == KEY_POWER || ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)
            continue;
        if (ev.value == 1 || (ev.value == 2 && (ev.code == KEY_UP || ev.code == KEY_DOWN)))
            got = ev.code;
    }
    return got;
}

/* --------------------------------------------------------------- app */

typedef enum { DLG_NONE, DLG_PERM, DLG_BACKGROUND, DLG_WARN, DLG_UNPAIR, DLG_UPDATE, DLG_UPDINFO, DLG_UNDO, DLG_PLWARN, DLG_PLREMOVE, DLG_WELCOME, DLG_EMPTY } dialog_t;

static volatile sig_atomic_t ui_stop = 0;
static void on_term(int s)
{
    (void)s;
    ui_stop = 1;
}

static void hint(int *x, int y, const char *button, const char *label)
{
    int bw = text_width(button, 15) + 14;
    round_box(*x, y, bw, 22, 11, C_PANEL2);
    text(*x + 7, y + 3, button, 15, C_ACCENT);
    *x += bw + 6;
    *x += text(*x, y + 3, label, 15, C_DIM) + 14;
}

/* settings screen
 * items: 0-4 permissions, 5 background without asking, 6 keep working now, 7 new code,
 * 8 disconnect, 9 developer options (submenu), 10 developer tools, 11 check for updates, 12 undo changes,
 * 13 plugins (submenu), 14 show Claude's plan first, 15 delete old backups, 16 empty backups */
#define SET_ITEMS 17
static const int menu_main[] = {0, 1, 2, 3, 4, 14, 6, 12, 16, 15, 11, 7, 8, 9};
static const int menu_dev[] = {10, 13};
#define MAIN_COUNT (int)(sizeof(menu_main) / sizeof(menu_main[0]))
#define DEV_COUNT (int)(sizeof(menu_dev) / sizeof(menu_dev[0]))
#define MENU_ROWS 9 /* rows that fit above the help box */

static int menu_index(const int *menu, int count, int item)
{
    for (int k = 0; k < count; k++)
        if (menu[k] == item)
            return k;
    return 0;
}

static const char *set_label(int i)
{
    if (i < PERM_COUNT)
        return perm_titles[i];
    switch (i) {
    case 5:
        return "Keep working in the background";
    case 6:
        return "Keep working after leaving the app";
    case 7:
        return "Show a new pairing code";
    case 9:
        return "Developer options";
    case 10:
        return "Developer tools";
    case 11:
        return "Check for updates";
    case 12:
        return "Undo changes made by Claude";
    case 13:
        return "Plugins";
    case 14:
        return "Show Claude's plan first";
    case 15:
        return "Delete old backups";
    case 16:
        return "Empty backups";
    default:
        return "Disconnect every Claude";
    }
}

static const char *set_help(int i)
{
    static const char *h[SET_ITEMS] = {
        "Claude looking at your screen (screenshots). Ask once: asks the first time after you open this app. "
        "Ask every time: asks before each one. Don't ask: never asks. Recommended: Ask once.",
        "Claude opening and reading files on the SD card (settings, logs, saves) to find problems. "
        "Ask once: asks the first time after you open this app. Recommended: Ask once.",
        "Claude creating, changing and deleting files. A copy of everything changed is kept, so it can be undone "
        "(\"Undo changes made by Claude\"). Recommended: Ask once.",
        "Claude pressing the buttons for you, as if you did (and recording the buttons you press, for "
        "developers). Recommended: Ask once.",
        "Claude running system commands. This is the most powerful permission: it can do anything on this "
        "Miyoo. Recommended: Ask once, or Ask every time.",
        "Claude working after you leave this app (for example to open and test a game). Ask every time: you "
        "confirm each time. Until restart: allowed until the Miyoo is turned off. Recommended: Ask every time.",
        "Claude working after you leave this app (for example to open and test a game). Off: Claude asks here "
        "when it needs it. Until restart: on until the Miyoo is turned off. Always: always on, for developers. "
        "Recommended: Off.",
        "Shows a new connection code. Use it to connect another Claude account, or if Claude says the "
        "connection was turned off.",
        "Removes the access of every Claude account connected to this Miyoo. To use the app again, you will "
        "connect with a new code.",
        "Options for people who make or test games and apps. Most people do not need them.",
        "Lets Claude download files from the internet to the SD card, record the buttons you press and replay "
        "them with exact timing, and take timed screenshots. Works in new Claude chats. Recommended: Off.",
        "Looks for a new version of Claude Agent on the internet (Wi-Fi needed). The app also checks by "
        "itself once each time the Miyoo is turned on.",
        "Everything Claude changed, created or deleted, newest first. Pick one and press A to put it back as "
        "it was before.",
        "Extra tools made by developers or by Claude. Each one can be turned off, or set to ask every time, ask "
        "once, or not ask.",
        "Before a big change (several files, system settings, installing or removing something), Claude shows "
        "its plan here and waits for your OK. Recommended: On.",
        "The copies Claude keeps of what it changed take space on the SD card. Old ones can be deleted by "
        "themselves; after that, those changes can no longer be undone. Recommended: After 30 days.",
        "Deletes every copy kept in the backups folder now, to free space on the SD card. The changes Claude "
        "made until now can no longer be undone.",
    };
    return h[i];
}

/* number of choices of an item (0 = it is an action) */
static int set_choices(int i)
{
    if (i < PERM_COUNT || i == 5 || i == 10)
        return 3;
    if (i == 6)
        return 3;
    if (i == 14)
        return 2;
    if (i == 15)
        return 3;
    return 0;
}

/* the choice that is safe and recommended; going above it lowers security */
static int set_default(int i)
{
    return i < PERM_COUNT || i == 15 ? 1 : 0;
}

static const char *choice_label(int i, int v)
{
    if (i < PERM_COUNT)
        return v == 0 ? "Ask every time" : v == 1 ? "Ask once" : "Don't ask";
    if (i == 5)
        return v == 0 ? "Ask every time" : v == 1 ? "Until restart" : "Always";
    if (i == 10)
        return v == 0 ? "Off" : v == 1 ? "Until exit" : "Always";
    if (i == 6)
        return v == 0 ? "Off" : v == 1 ? "Until restart" : "Always";
    if (i == 14)
        return v ? "Off" : "On"; /* 0 = shows the plan (the safe choice) */
    if (i == 15)
        return v == 0 ? "Never" : v == 1 ? "After 30 days" : "After 7 days";
    return "";
}

static int set_value(int i)
{
    if (i < PERM_COUNT)
        return perm_mode(i);
    if (i == 5)
        return setting_get("background_auto") ? 2 : access(BG_AUTO_SESSION, F_OK) == 0 ? 1 : 0;
    if (i == 6)
        return setting_get("background_auto") ? 2 : access(BG_FLAG, F_OK) == 0 ? 1 : 0;
    if (i == 10)
        return setting_get("dev_tools") ? 2 : access(DEV_SESSION, F_OK) == 0 ? 1 : 0;
    if (i == 14)
        return setting_get("plans_skip");
    if (i == 15)
        return setting_num("backup_clean", 1);
    return -1; /* action */
}

static const char *warn_text(int i)
{
    static const char *t[SET_ITEMS] = {
        "Claude will see your screen without asking you first.",
        "Claude will read your files without asking you first.",
        "Claude will change and delete files without asking you first. "
        "Copies are kept, so changes can be undone.",
        "Claude will press buttons without asking you first.",
        "Claude will run system commands without asking you first. This can change anything on this Miyoo.",
        "Claude will keep working after you leave the app without asking you first. "
        "Meant for developers testing their games.",
        "Claude keeps control of this Miyoo after you leave the app, and turns it on by itself when needed. "
        "Permissions set to ask cannot be asked while the app is closed.",
        NULL, NULL, NULL,
        "Claude will be able to download files from the internet to your SD card and to replay button "
        "presses by itself. Meant for developers.",
        NULL, NULL, NULL,
        "Claude will start big changes without showing you its plan first.",
        "Copies older than 7 days will be deleted for good, and those changes can no longer be undone.",
        NULL,
    };
    return t[i] ? t[i] : "";
}

typedef struct {
    char status[512];
    char server[512];
    char ip[64];
    char note[128];
    int wifi_hw;
    int screen; /* 0 main, 1 settings, 2 developer options, 3 undo changes, 4 plugins */
    int set_sel;
    dialog_t dlg;
    int warn_item, warn_to;
    int upd_wait;        /* waiting for a check (1) or an install (2) started here */
    change_t *ch;        /* undo screen */
    int nch, ch_sel;
    char undo_msg[300];
    char (*pl)[48];      /* plugins screen */
    int npl, pl_sel, pl_to;
    char ask[1600];
    long long bk_kb;              /* size of the backups folder, -1 = to measure */
    int focus; /* 0 = Claude pane, 1 = activity pane */
    long long busy_until;
    /* status bar: clock, Wi-Fi, battery */
    int bat, charging, wifi_q;
    long long next_sys;
    char sys_sig[48];
} ui_t;

static void draw_dialog3(ui_t *u, const char *title, const char *body, const char *detail, const char *yes,
                         const char *alt, const char *no, uint32_t title_color);
static void draw_dialog(ui_t *u, const char *title, const char *body, const char *detail, const char *yes,
                        const char *no, uint32_t title_color)
{
    draw_dialog3(u, title, body, detail, yes, NULL, no, title_color);
}

static void draw_dialog3(ui_t *u, const char *title, const char *body, const char *detail, const char *yes,
                         const char *alt, const char *no, uint32_t title_color)
{
    for (size_t i = 0; i < (size_t)W * H; i++)
        canvas[i] = blend(canvas[i], 0xff000000, 160);
    int ox = (W - 640) / 2, oy = (H - 480) / 2;
    lines_t L = {0};
    wrap_into(&L, body, 18, 460, LK_BODY);
    lines_t D = {0};
    if (detail && *detail)
        wrap_into(&D, detail, 16, 440, LK_BODY);
    if (D.n > 3)
        D.n = 3;
    int h = 110 + L.n * 24 + (D.n ? D.n * 21 + 20 : 0);
    int y = oy + (480 - h) / 2;
    round_box(ox + 80, y + 4, 480, h, 18, 0xff0c0b0a);
    round_box(ox + 80, y, 480, h, 18, C_PANEL);
    text_center(W / 2, y + 18, title, 24, title_color);
    int ty = y + 58;
    for (int i = 0; i < L.n; i++, ty += 24)
        text_raw(ox + 100, ty, L.v[i].s, 18, C_TEXT);
    if (D.n) {
        round_box(ox + 96, ty + 4, 448, D.n * 21 + 12, 8, C_PANEL2);
        for (int i = 0; i < D.n; i++)
            text_raw(ox + 106, ty + 10 + i * 21, D.v[i].s, 16, C_ACCENT);
        ty += D.n * 21 + 20;
    }
    int bw = (yes ? text_width(yes, 15) + 45 : 0) + text_width(no, 15) + 45 + (alt ? text_width(alt, 15) + 45 : 0);
    int bx = W / 2 - bw / 2;
    if (yes)
        hint(&bx, y + h - 38, "A", yes);
    if (alt)
        hint(&bx, y + h - 38, "Y", alt);
    hint(&bx, y + h - 38, "B", no);
    free(L.v);
    free(D.v);
    (void)u;
}

/* battery %, charging, Wi-Fi quality (0-4 bars, -1 off); cheap, every few seconds */
static void sys_status(ui_t *u)
{
    char b[64];
    u->bat = read_small("/tmp/percBat", b, sizeof(b)) >= 0 ? atoi(b) : -1;
    if (u->bat > 100)
        u->bat = 100;
    char model[16] = "";
    read_small("/tmp/deviceModel", model, sizeof(model));
    u->charging = 0;
    if (atoi(model) == 283) {
        if (read_small("/sys/class/gpio/gpio59/value", b, sizeof(b)) >= 0)
            u->charging = b[0] == '1';
    }
    else if (access("/customer/app/axp_test", X_OK) == 0) {
        static long long last;
        static int cached;
        if (now_ms() - last > 10000) { /* a small program: not more than every 10 s */
            last = now_ms();
            FILE *p = popen("cd /customer/app && ./axp_test 2>/dev/null", "r");
            int c = 0;
            if (p) {
                char line[160] = "";
                if (fgets(line, sizeof(line), p)) {
                    char *k = strstr(line, "\"charging\":");
                    if (k)
                        c = atoi(k + 11);
                }
                pclose(p);
            }
            cached = c == 3;
        }
        u->charging = cached;
    }
    u->wifi_q = -1;
    if (u->ip[0]) {
        u->wifi_q = 2;
        FILE *f = fopen("/proc/net/wireless", "r");
        if (f) {
            /* "wlan0: 0000   99.  -60.  -256. ...": the signal level in dBm. There is also an idle wlan1
               at -256: take the best interface, not the last one */
            char line[200];
            int best = -1000;
            while (fgets(line, sizeof(line), f)) {
                char *c = strchr(line, ':');
                if (!c || !strstr(line, "wlan"))
                    continue;
                int st;
                float q, lvl;
                if (sscanf(c + 1, "%x %f %f", &st, &q, &lvl) == 3 && lvl > -200 && lvl < 0 && lvl > best)
                    best = (int)lvl;
            }
            if (best > -1000)
                u->wifi_q = best >= -55 ? 4 : best >= -65 ? 3 : best >= -75 ? 2 : 1;
            fclose(f);
        }
    }
}

/* the status icons, right-aligned ending at x */
static void draw_status_bar(ui_t *u, int x, int y)
{
    char clock[16];
    time_t t = time(NULL);
    strftime(clock, sizeof(clock), "%H:%M", localtime(&t));
    /* battery */
    if (u->bat >= 0) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", u->bat);
        int tw = text_width(pct, 15);
        x -= tw;
        text(x, y + 3, pct, 15, C_DIM);
        x -= 32;
        uint32_t col = u->charging ? C_OK : u->bat <= 15 ? C_BAD : C_TEXT;
        round_box(x, y + 5, 24, 13, 3, C_DIM);
        round_box(x + 1, y + 6, 22, 11, 2, C_BG);
        int w = u->bat * 20 / 100;
        if (w < 2)
            w = 2;
        round_box(x + 2, y + 7, w, 9, 2, col);
        fill(x + 24, y + 9, 2, 5, C_DIM);
        if (u->charging) { /* a small bolt */
            for (int i = 0; i < 5; i++) {
                fill(x + 12 - i / 2, y + 7 + i, 2, 1, C_BG);
                fill(x + 11 + i / 2, y + 11 + i, 2, 1, C_BG);
            }
        }
        x -= 14;
    }
    /* Wi-Fi: four bars */
    if (u->wifi_hw) {
        x -= 22;
        for (int i = 0; i < 4; i++) {
            int bh = 4 + i * 3;
            fill(x + i * 5, y + 18 - bh, 3, bh, u->wifi_q > i ? C_TEXT : C_LINE);
        }
        x -= 14;
    }
    int cw = text_width(clock, 16);
    text(x - cw, y + 2, clock, 16, C_TEXT);
}

/* Claude's plan before a big change: the steps, then A to go ahead or B to stop */
static void draw_plan(ui_t *u)
{
    for (size_t i = 0; i < (size_t)W * H; i++)
        canvas[i] = blend(canvas[i], 0xff000000, 170);
    int ox = (W - 640) / 2, oy = (H - 480) / 2;
    char title[200], steps[1200], note[320];
    kv_get(u->ask, "title", title, sizeof(title));
    kv_get(u->ask, "text", steps, sizeof(steps));
    kv_get(u->ask, "detail", note, sizeof(note));
    for (char *p = note; *p; p++)
        if (*p == '\v')
            *p = ' ';
    lines_t T1 = {0}, S = {0}, N = {0};
    wrap_into(&T1, title, 20, 548, LK_BODY);
    if (T1.n > 2)
        T1.n = 2;
    /* numbered steps, separated by \v; the lines after the first are indented */
    int nums[64], k = 0;
    char *save = NULL;
    for (char *st = strtok_r(steps, "\v", &save); st && k < 10; st = strtok_r(NULL, "\v", &save)) {
        k++;
        int before = S.n;
        wrap_into(&S, st, 17, 516, LK_BODY);
        for (int i = before; i < S.n && i < 64; i++)
            nums[i] = i == before ? k : 0;
    }
    if (note[0])
        wrap_into(&N, note, 16, 528, LK_BODY);
    if (N.n > 3)
        N.n = 3;
    int max_steps = (440 - 106 - T1.n * 26 - (N.n ? N.n * 21 + 20 : 0)) / 23;
    if (S.n > max_steps)
        S.n = max_steps;
    int h = 44 + T1.n * 26 + 10 + S.n * 23 + (N.n ? N.n * 21 + 20 : 0) + 52;
    int y = oy + (480 - h) / 2;
    round_box(ox + 24, y + 4, 592, h, 18, 0xff0c0b0a);
    round_box(ox + 24, y, 592, h, 18, C_PANEL);
    text(ox + 46, y + 16, "Claude's plan", 17, C_ACCENT);
    int ty = y + 44;
    for (int i = 0; i < T1.n; i++, ty += 26)
        text_raw(ox + 46, ty, T1.v[i].s, 20, C_TEXT);
    ty += 10;
    for (int i = 0; i < S.n; i++, ty += 23) {
        if (i < 64 && nums[i]) {
            char n[8];
            snprintf(n, sizeof(n), "%d", nums[i]);
            round_box(ox + 46, ty + 1, 20, 20, 10, C_PANEL2);
            text_raw(ox + 56 - text_width_raw(n, 13) / 2, ty + 3, n, 13, C_ACCENT);
        }
        text_raw(ox + 76, ty, S.v[i].s, 17, C_TEXT);
    }
    if (N.n) {
        ty += 8;
        round_box(ox + 42, ty, 556, N.n * 21 + 12, 8, C_PANEL2);
        for (int i = 0; i < N.n; i++)
            text_raw(ox + 52, ty + 6 + i * 21, N.v[i].s, 16, C_ACCENT);
    }
    int bw = text_width("Go ahead", 15) + 45 + text_width("Not now", 15) + 45;
    int bx = W / 2 - bw / 2;
    hint(&bx, y + h - 38, "A", "Go ahead");
    hint(&bx, y + h - 38, "B", "Not now");
    free(T1.v);
    free(S.v);
    free(N.v);
}

static void draw(ui_t *u, pane_t *msg, pane_t *act)
{
    char state[32], code[16], client[64];
    kv_get(u->status, "state", state, sizeof(state));
    kv_get(u->status, "code", code, sizeof(code));
    kv_get(u->status, "client", client, sizeof(client));
    int ox = (W - 640) / 2, oy = (H - 480) / 2;

    fill(0, 0, W, H, C_BG);
    if (ox || oy) /* 752x560 panels: the 640x480 layout in the middle */
        fill(ox, oy, 640, 480, C_BG);

    /* header */
    text(ox + 24, oy + 11, "Claude Agent", 24, C_TEXT);
    const char *st = "Starting";
    uint32_t sc = C_DIM;
    if (!u->wifi_hw) {
        st = "No Wi-Fi on this Miyoo";
        sc = C_BAD;
    }
    else if (!u->ip[0]) {
        st = u->note[0] ? u->note : "Waiting for Wi-Fi";
        sc = C_WARN;
    }
    else if (!strcmp(state, "connected")) {
        st = client[0] ? "Connected" : "Online";
        sc = C_OK;
    }
    else if (!strcmp(state, "connecting")) {
        st = "Connecting";
        sc = C_WARN;
    }
    else if (!strcmp(state, "offline")) {
        st = "Server not reachable";
        sc = C_BAD;
    }
    int sw = text_width(st, 16) + 40;
    round_box(ox + 616 - sw, oy + 12, sw, 28, 14, C_PANEL);
    /* while Claude is working the dot becomes the app's pixel star, twinkling */
    if (now_ms() < u->busy_until) {
        static const char *frames[4][7] = {
            {"...#...", "...#...", "...#...", "#######", "...#...", "...#...", "...#..."},
            {"#..#..#", ".#.#.#.", "..###..", "#######", "..###..", ".#.#.#.", "#..#..#"},
            {"#.....#", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#...#.", "#.....#"},
            {"#..#..#", ".#.#.#.", "..###..", "#######", "..###..", ".#.#.#.", "#..#..#"},
        };
        int f = (int)((now_ms() / 160) % 4);
        int sx = ox + 616 - sw + 10, sy = oy + 19;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 7; c++)
                if (frames[f][r][c] == '#')
                    fill(sx + c * 2, sy + r * 2, 2, 2, C_ACCENT);
    }
    else
        round_box(ox + 616 - sw + 12, oy + 21, 10, 10, 5, sc);
    text(ox + 616 - sw + 28, oy + 17, st, 16, sc);
    if (u->screen == 0 && sw < 230) /* main screen: clock, Wi-Fi and battery, like OnionOS */
        draw_status_bar(u, ox + 616 - sw - 14, oy + 15);

    if (u->screen == 4) {
        static const char *pm[4] = {"Off", "Ask every time", "Ask once", "Don't ask"};
        text(ox + 24, oy + 56, "Developer options  >  Plugins", 20, C_ACCENT);
        if (!u->npl)
            text(ox + 30, oy + 90, "No plugins yet. Claude can make one when a task needs it.", 17, C_DIM);
        int first = u->pl_sel - (MENU_ROWS - 1);
        if (first < 0)
            first = 0;
        char title[64], summary[300], perm[16];
        for (int k = first; k < u->npl && k < first + MENU_ROWS; k++) {
            int y = oy + 84 + (k - first) * 30, sel = k == u->pl_sel;
            if (sel)
                round_box(ox + 16, y - 2, 608, 29, 10, C_PANEL2);
            if (plugin_info(u->pl[k], title, sizeof(title), summary, sizeof(summary), perm, sizeof(perm)) != 0)
                snprintf(title, sizeof(title), "%s", u->pl[k]);
            text(ox + 30, y + 3, title, 18, sel ? C_TEXT : C_DIM);
            int v = plugin_mode(u->pl[k]);
            const char *lab = pm[v];
            int tw = text_width(lab, 17), pw = tw + (sel ? 46 : 26), px = ox + 612 - pw;
            round_box(px, y + 1, pw, 25, 12, v == 3 ? blend(C_PANEL, C_ACCENT, 90) : C_PANEL);
            text(px + pw / 2 - tw / 2, y + 3, lab, 17, v == 3 ? C_ACCENT : sel ? C_TEXT : C_DIM);
            if (sel) {
                text(px + 8, y + 3, "<", 17, C_DIM);
                text(px + pw - 16, y + 3, ">", 17, C_DIM);
            }
        }
        lines_t L = {0};
        char help[800] = "";
        if (u->undo_msg[0])
            snprintf(help, sizeof(help), "%s", u->undo_msg);
        else if (u->npl && plugin_info(u->pl[u->pl_sel], title, sizeof(title), summary, sizeof(summary), perm,
                                       sizeof(perm)) == 0) {
            const char *pt = perm_titles[4];
            for (int i = 0; i < PERM_COUNT; i++)
                if (!strcmp(perm, perm_keys[i]))
                    pt = perm_titles[i];
            snprintf(help, sizeof(help), T("%s It can: %s. Folder: plugins/%s"), summary, T(pt), u->pl[u->pl_sel]);
        }
        else
            snprintf(help, sizeof(help), "%s", set_help(13));
        wrap_into(&L, help, 15, 580, LK_BODY);
        round_box(ox + 16, oy + 362, 608, 76, 12, C_PANEL);
        for (int i = 0; i < L.n && i < 3; i++)
            text_raw(ox + 30, oy + 370 + i * 21, L.v[i].s, 15, C_TEXT);
        free(L.v);
        int fx = ox + 24;
        hint(&fx, oy + 446, "A", "Change");
        if (u->npl)
            hint(&fx, oy + 446, "Y", "Remove");
        hint(&fx, oy + 446, "B", "Back");
    }
    else if (u->screen == 3) {
        text(ox + 24, oy + 56, "Settings  >  Undo changes", 20, C_ACCENT);
        if (!u->nch)
            text(ox + 30, oy + 90, "Claude has not changed anything on this Miyoo yet.", 17, C_DIM);
        int first = u->ch_sel - (MENU_ROWS - 1);
        if (first < 0)
            first = 0;
        for (int k = first; k < u->nch && k < first + MENU_ROWS; k++) {
            change_t *c = &u->ch[k];
            int y = oy + 84 + (k - first) * 30, sel = k == u->ch_sel;
            if (sel)
                round_box(ox + 16, y - 2, 608, 29, 10, C_PANEL2);
            char when[16];
            snprintf(when, sizeof(when), "%.5s %.5s", c->when + 5, c->when + 11); /* MM-DD HH:MM */
            text(ox + 30, y + 5, when, 14, C_DIM);
            const char *verb = !strcmp(c->action, "created") ? "Created" : !strcmp(c->action, "deleted") ? "Deleted" : "Changed";
            text(ox + 130, y + 3, verb, 17, c->undone ? C_DIM : C_ACCENT);
            const char *name = strrchr(c->path, '/');
            char shown[300];
            snprintf(shown, sizeof(shown), "%s%s%s", name ? name + 1 : c->path, c->undone ? "  " : "", c->undone ? T("(undone)") : "");
            text(ox + 210, y + 3, shown, 17, sel && !c->undone ? C_TEXT : C_DIM);
        }
        if (u->nch > MENU_ROWS) {
            int bh = 270 * MENU_ROWS / u->nch, by = oy + 84 + (270 - bh) * first / (u->nch - MENU_ROWS);
            round_box(ox + 628, by, 4, bh, 2, C_LINE);
        }
        lines_t L = {0};
        char help[1600];
        if (u->undo_msg[0])
            snprintf(help, sizeof(help), "%s", u->undo_msg);
        else if (u->nch)
            snprintf(help, sizeof(help), T("%s (%s). Press A to undo it."), u->ch[u->ch_sel].path, u->ch[u->ch_sel].when);
        else
            snprintf(help, sizeof(help), "%s", set_help(12));
        wrap_into(&L, help, 15, 580, LK_BODY);
        round_box(ox + 16, oy + 362, 608, 76, 12, C_PANEL);
        for (int i = 0; i < L.n && i < 3; i++)
            text_raw(ox + 30, oy + 370 + i * 21, L.v[i].s, 15, C_TEXT);
        free(L.v);
        int fx = ox + 24;
        hint(&fx, oy + 446, "A", "Undo");
        hint(&fx, oy + 446, "B", "Back");
    }
    else if (u->screen == 1 || u->screen == 2) {
        /* settings */
        const int *menu = u->screen == 1 ? menu_main : menu_dev;
        int count = u->screen == 1 ? MAIN_COUNT : DEV_COUNT;
        text(ox + 24, oy + 56, u->screen == 1 ? "Settings" : "Settings  >  Developer options", 20, C_ACCENT);
        int first = u->set_sel - (MENU_ROWS - 1);
        if (first < 0)
            first = 0;
        for (int k = first; k < count && k < first + MENU_ROWS; k++) {
            int i = menu[k];
            int y = oy + 84 + (k - first) * 30;
            int sel = k == u->set_sel;
            if (sel)
                round_box(ox + 16, y - 2, 608, 29, 10, C_PANEL2);
            text(ox + 30, y + 3, set_label(i), 18, sel ? C_TEXT : C_DIM);
            int v = set_value(i);
            if (set_choices(i)) {
                /* the choice, in a pill; arrows when selected */
                const char *lab = choice_label(i, v);
                int risky = v > set_default(i);
                int tw = text_width(lab, 17), pw = tw + (sel ? 46 : 26);
                int px = ox + 612 - pw;
                round_box(px, y + 1, pw, 25, 12, risky ? blend(C_PANEL, C_ACCENT, 90) : C_PANEL);
                text(px + pw / 2 - tw / 2, y + 3, lab, 17, risky ? C_ACCENT : sel ? C_TEXT : C_DIM);
                if (sel) {
                    text(px + 8, y + 3, "<", 17, C_DIM);
                    text(px + pw - 16, y + 3, ">", 17, C_DIM);
                }
            }
            else if (i == 16) {
                if (u->bk_kb < 0)
                    u->bk_kb = backups_kb();
                char sz[32];
                snprintf(sz, sizeof(sz), "%.1f MB", u->bk_kb / 1024.0);
                text_raw(ox + 604 - text_width_raw(sz, 17), y + 3, sz, 17, sel ? C_TEXT : C_DIM);
            }
            else
                text(ox + 584, y + 1, ">", 20, sel ? C_ACCENT : C_DIM);
        }
        if (count > MENU_ROWS) { /* scroll bar */
            int bh = 270 * MENU_ROWS / count, by = oy + 84 + (270 - bh) * first / (count - MENU_ROWS);
            round_box(ox + 628, by, 4, bh, 2, C_LINE);
        }
        lines_t L = {0};
        wrap_into(&L, set_help(menu[u->set_sel]), 15, 580, LK_BODY);
        round_box(ox + 16, oy + 362, 608, 76, 12, C_PANEL);
        for (int i = 0; i < L.n && i < 3; i++)
            text_raw(ox + 30, oy + 370 + i * 21, L.v[i].s, 15, C_TEXT);
        free(L.v);
        int fx = ox + 24;
        hint(&fx, oy + 446, "A", "Change");
        hint(&fx, oy + 446, "B", "Back");
    }
    else if (code[0] && !strcmp(state, "connected")) {
        /* pairing: QR to the help page on the left, the code and three short steps on the right */
        text(ox + 24, oy + 62, "Connect this Miyoo to Claude", 22, C_TEXT);
        text(ox + 24, oy + 92, "This lets Claude see and fix things on this Miyoo. You choose what it may do.", 15,
             C_DIM);
        const char *site = u->server;
        if (!strncmp(site, "https://", 8))
            site += 8;
        qr_draw(ox + 24, oy + 122, 204, u->server);
        text_center(ox + 126, oy + 332, "Scan with your phone", 15, C_DIM);

        int rx = ox + 248, rw = 368;
        text(rx, oy + 120, "YOUR CODE", 14, C_DIM);
        char big[16];
        snprintf(big, sizeof(big), "%.3s %.3s", code, code + 3);
        round_box(rx, oy + 140, rw, 80, 16, C_PANEL);
        text_center(rx + rw / 2, oy + 146, big, 60, C_ACCENT);

        static const char *steps[] = {"Scan the QR code, or open this site:", NULL,
                                      "Follow the steps on that page.", "When asked, type the code above."};
        int sy = oy + 236, num = 1;
        for (int i = 0; i < 4; i++) {
            if (!steps[i]) {
                text_raw(rx + 26, sy - 4, site, 16, C_ACCENT);
                sy += 26;
                continue;
            }
            char nb[4];
            snprintf(nb, sizeof(nb), "%d", num++);
            round_box(rx, sy, 18, 18, 9, C_PANEL2);
            text_center(rx + 9, sy + 1, nb, 13, C_ACCENT);
            text(rx + 26, sy, steps[i], 16, C_TEXT);
            sy += 26;
        }

        round_box(ox + 24, oy + 360, 592, 62, 12, C_PANEL);
        text(ox + 40, oy + 369, "You only do this once. The code changes every 10 minutes.", 15, C_TEXT);
        text(ox + 40, oy + 393, "No phone? Open the same site on any computer.", 15, C_DIM);
        int fx = ox + 24;
        hint(&fx, oy + 446, "START", "Settings");
        hint(&fx, oy + 446, "B", "Exit");
    }
    else {
        pane_draw(msg, u->focus == 0);
        pane_draw(act, u->focus == 1);
        if (access(BG_FLAG, F_OK) == 0)
            text(ox + 24, oy + 424, "Keeps working after you leave the app", 14, C_ACCENT);
        int fx = ox + 24;
        hint(&fx, oy + 446, "L1/R1", "Switch");
        hint(&fx, oy + 446, "UP/DOWN", "Scroll");
        hint(&fx, oy + 446, "START", "Settings");
        hint(&fx, oy + 446, "B", "Exit");
    }

    char ask_cat[48] = "";
    if (u->dlg == DLG_PERM)
        kv_get(u->ask, "cat", ask_cat, sizeof(ask_cat));
    if (u->dlg == DLG_PERM && !strcmp(ask_cat, "plan"))
        draw_plan(u);
    else if (u->dlg == DLG_PERM) {
        char cat[48], title[120], body[700], detail[320];
        kv_get(u->ask, "cat", cat, sizeof(cat));
        kv_get(u->ask, "title", title, sizeof(title));
        kv_get(u->ask, "text", body, sizeof(body));
        kv_get(u->ask, "detail", detail, sizeof(detail));
        for (char *p = detail; *p; p++)
            if (*p == '\v')
                *p = ' ';
        for (char *p = body; *p; p++)
            if (*p == '\v')
                *p = ' ';
        int risky = !strcmp(cat, "command") || !strcmp(cat, "install");
        draw_dialog(u, title, body, detail, "Allow", "Deny", risky ? C_WARN : C_TEXT);
    }
    else if (u->dlg == DLG_BACKGROUND)
        draw_dialog(u, "Keep working in the background?",
                    "Claude asks to keep control of this Miyoo after you leave the app, for example to open "
                    "and test a game. It ends when you turn it off in the settings or restart the Miyoo.",
                    NULL, "Allow", "Deny", C_TEXT);
    else if (u->dlg == DLG_WARN) {
        char body[600];
        snprintf(body, sizeof(body), T("%s New setting: %s."), T(warn_text(u->warn_item)),
                 T(choice_label(u->warn_item, u->warn_to)));
        draw_dialog(u, "Less security", body, NULL, "Confirm", "Cancel", C_WARN);
    }
    else if (u->dlg == DLG_UPDATE) {
        char v[64], note[220], body[500];
        update_get("version", v, sizeof(v));
        update_get("note", note, sizeof(note));
        snprintf(body, sizeof(body), T("Claude Agent %s is available (you have %s). It takes about a minute, with "
                                     "Wi-Fi. Your settings and connection stay as they are."),
                 v, AGENT_VERSION);
        draw_dialog(u, "Update available", body, note[0] ? note : NULL, "Update now", "Later", C_TEXT);
    }
    else if (u->dlg == DLG_UPDINFO) {
        char st[32], v[64], note[220], body[400];
        update_get("state", st, sizeof(st));
        update_get("version", v, sizeof(v));
        update_get("note", note, sizeof(note));
        const char *title = "Updates";
        if (!strcmp(st, "latest"))
            snprintf(body, sizeof(body), T("You have the latest version (%s)."), AGENT_VERSION);
        else if (!strcmp(st, "installing"))
            snprintf(body, sizeof(body), T("Updating to %s. %s Do not turn the Miyoo off."), v, T(note));
        else if (!strcmp(st, "installed"))
            snprintf(body, sizeof(body), T("Updated to %s. Restarting the app..."), v);
        else if (!strcmp(st, "error") || !strcmp(st, "failed"))
            snprintf(body, sizeof(body), "%s", note[0] ? T(note) : T("Something went wrong."));
        else
            snprintf(body, sizeof(body), "%s", T("Checking for updates..."));
        draw_dialog(u, title, body, NULL, NULL, "Close", C_TEXT);
    }
    else if (u->dlg == DLG_UNDO && u->ch_sel < u->nch) {
        change_t *c = &u->ch[u->ch_sel];
        const char *what = !strcmp(c->action, "created")  ? "Remove this file, which Claude created?"
                           : !strcmp(c->action, "deleted") ? "Bring back what Claude deleted?"
                                                           : "Put back the version from before this change?";
        draw_dialog(u, "Undo this change?", what, c->path, "Undo", "Cancel", C_TEXT);
    }
    else if (u->dlg == DLG_PLWARN && u->pl_sel < u->npl) {
        char t[64], sm[300], pm[16], body[500];
        plugin_info(u->pl[u->pl_sel], t, sizeof(t), sm, sizeof(sm), pm, sizeof(pm));
        snprintf(body, sizeof(body), T("Claude will use the plugin \"%s\" without asking you first."), t);
        draw_dialog(u, "Less security", body, NULL, "Confirm", "Cancel", C_WARN);
    }
    else if (u->dlg == DLG_PLREMOVE && u->pl_sel < u->npl) {
        char t[64], sm[300], pm[16], body[400];
        plugin_info(u->pl[u->pl_sel], t, sizeof(t), sm, sizeof(sm), pm, sizeof(pm));
        snprintf(body, sizeof(body), T("Remove the plugin \"%s\"? A copy is kept in the app's backups folder."), t);
        draw_dialog(u, "Remove plugin?", body, NULL, "Remove", "Cancel", C_TEXT);
    }
    else if (u->dlg == DLG_WELCOME)
        draw_dialog(u, "Before you start",
                    "Claude is an AI and can make mistakes. Before you ask it to change anything, back up your SD "
                    "card: copy everything on it to a computer.",
                    "Claude asks before it acts, keeps a copy of every file it changes, and its changes can be "
                    "undone in Settings.",
                    "I understand", "Exit", C_WARN);
    else if (u->dlg == DLG_EMPTY) {
        char body[400];
        snprintf(body, sizeof(body), T("This deletes %.1f MB of copies for good. The changes Claude made until now "
                                       "can no longer be undone."),
                 (u->bk_kb < 0 ? 0 : u->bk_kb) / 1024.0);
        draw_dialog(u, "Empty backups?", body, NULL, "Empty", "Cancel", C_WARN);
    }
    else if (u->dlg == DLG_UNPAIR)
        draw_dialog(u, "Disconnect every Claude?",
                    "Claude loses access to this Miyoo. To use it again, connect with a new code.", NULL,
                    "Disconnect", "Cancel", C_TEXT);
    screen_present();
}

static void apply_setting(ui_t *u, int i, int on)
{
    char key[32], grant[64];
    static const char *how[3] = {"off", "on until the app closes", "always on"};
    if (i < PERM_COUNT) {
        snprintf(key, sizeof(key), "perm_%s", perm_keys[i]);
        setting_set_num(key, on);
        snprintf(key, sizeof(key), "always_%s", perm_keys[i]); /* old setting */
        setting_set(key, 0);
        snprintf(grant, sizeof(grant), "%s%s", PERM_GRANT, perm_keys[i]);
        if (on != 1)
            unlink(grant);
        activity("Setting: \"%s\": %s", T(perm_titles[i]), T(choice_label(i, on)));
    }
    else if (i == 5) {
        setting_set("background_auto", on == 2);
        if (on == 1)
            touch(BG_AUTO_SESSION);
        else
            unlink(BG_AUTO_SESSION);
        static const char *bg_how[3] = {"off", "on until the Miyoo restarts", "always on"};
        activity("Setting: background without asking: %s", T(bg_how[on]));
    }
    else if (i == 10) {
        setting_set("dev_tools", on == 2);
        if (on == 1)
            touch(DEV_SESSION);
        else
            unlink(DEV_SESSION);
        activity("Setting: developer tools: %s", T(how[on]));
        pid_t p = agent_pid(); /* tell Claude's side which tools there are now */
        if (p)
            kill(p, SIGHUP);
    }
    else if (i == 6) {
        /* one setting: off (Claude asks when it needs it), on until the Miyoo restarts, or always */
        setting_set("background_auto", on == 2);
        if (on) {
            touch(BG_FLAG);
            touch(BG_AUTO_SESSION);
        }
        else {
            unlink(BG_FLAG);
            unlink(BG_AUTO_SESSION);
        }
        static const char *bg_how[3] = {"off", "on until the Miyoo restarts", "always on"};
        activity("Keep working after leaving the app: %s", T(bg_how[on]));
    }
    else if (i == 14) {
        setting_set("plans_skip", on);
        activity("Setting: show Claude's plan first: %s", T(on ? "off" : "on"));
    }
    else if (i == 15) {
        setting_set_num("backup_clean", on);
        static const char *bc[3] = {"never", "after 30 days", "after 7 days"};
        activity("Setting: delete old backups: %s", T(bc[on]));
        static const int days[3] = {0, 30, 7};
        backups_prune(days[on]);
        u->bk_kb = -1;
    }
    (void)u;
}

int ui_main(void)
{
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    if (screen_open() != 0) {
        fprintf(stderr, "cannot use the screen\n");
        return 1;
    }
    font_load();
    input_fd = open(getenv("CA_INPUT") ? getenv("CA_INPUT") : "/dev/input/event0", O_RDONLY | O_NONBLOCK);

    ui_t u;
    memset(&u, 0, sizeof(u));
    u.bk_kb = -1;
    if (setting_get("background_auto")) /* "Always": keep working after leaving the app */
        touch(BG_FLAG);
    {
        static const int days[3] = {0, 30, 7}; /* old copies in backups/ */
        int m = setting_num("backup_clean", 1);
        backups_prune(days[m >= 0 && m < 3 ? m : 1]);
    }
    char path[1100];
    snprintf(path, sizeof(path), "%s/logs", g_home);
    mkdir(path, 0755);

    int ox = (W - 640) / 2, oy = (H - 480) / 2;
    pane_t msg = {"CLAUDE", "messages.log", ox + 16, oy + 50, 608, 220, 18, 23};
    pane_t act = {"ACTIVITY", "activity.log", ox + 16, oy + 276, 608, 144, 18, 23};
    msg.follow = act.follow = 1;

    /* relay address, printed by the connection code */
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "'%s' server", exe);
        FILE *f = popen(cmd, "r");
        if (f) {
            if (fgets(u.server, sizeof(u.server), f))
                u.server[strcspn(u.server, "\r\n")] = 0;
            pclose(f);
        }
    }

    if (getenv("CA_TEST_SERVER"))
        snprintf(u.server, sizeof(u.server), "%s", getenv("CA_TEST_SERVER"));
    char model[16] = "";
    read_small("/tmp/deviceModel", model, sizeof(model));
    u.wifi_hw = strcmp(model, "283") != 0;

    /* a new app session: permissions allowed "until the app closes" start over,
       unless the agent kept working in the background */
    if (!agent_pid())
        perm_clear_session();
    unlink(PERM_ANSWER);

    /* Wi-Fi: turned on here if it is off (and off again when leaving) */
    int wifi_started = 0;
    if (u.wifi_hw && !has_ip(u.ip, sizeof(u.ip))) {
        snprintf(u.note, sizeof(u.note), "Turning on Wi-Fi");
        draw(&u, &msg, &act);
        snprintf(path, sizeof(path), "'%s/wifi.sh' on", g_home);
        wifi_started = system(path) == 0;
        has_ip(u.ip, sizeof(u.ip));
        if (!u.ip[0])
            snprintf(u.note, sizeof(u.note), "No Wi-Fi: check Settings > Network");
    }

    /* first time: show a pairing code straight away */
    snprintf(path, sizeof(path), "%s/config/paired", g_home);
    int paired_before = access(path, F_OK) == 0;
    if (u.wifi_hw && u.ip[0]) {
        pid_t running = agent_pid();
        if (!running)
            agent_start(!paired_before);
        else if (!paired_before)
            kill(running, SIGUSR1);
    }

    if (getenv("CA_UI_SCREEN"))
        u.screen = atoi(getenv("CA_UI_SCREEN"));
    if (getenv("CA_UI_SEL"))
        u.set_sel = atoi(getenv("CA_UI_SEL"));
    if (u.screen == 3) /* tests */
        u.nch = changes_load(&u.ch);
    if (u.screen == 4)
        u.npl = plugins_list(&u.pl);
    if (!setting_get("welcome_ok")) /* the first time: what Claude is, and a backup */
        u.dlg = DLG_WELCOME;
    if (getenv("CA_UI_DLG"))
        u.dlg = atoi(getenv("CA_UI_DLG"));
    if (getenv("CA_UI_WARN"))
        u.warn_item = atoi(getenv("CA_UI_WARN"));
    char last_key[1400] = "";
    long long next_ip_check = 0, last_act_change = 0;
    int leave = 0, force = 1;
    while (!ui_stop && !leave) {
        snprintf(path, sizeof(path), "%s/status.txt", g_home);
        if (read_small(path, u.status, sizeof(u.status)) < 0)
            u.status[0] = 0;
        {
            /* updates: offer a new version once per power-on, follow a check or install started here */
            char ust[32], uv[64], later[64] = "";
            update_get("state", ust, sizeof(ust));
            update_get("version", uv, sizeof(uv));
            read_small("/tmp/claude_agent_update_later", later, sizeof(later));
            if (!strcmp(ust, "available") && u.dlg == DLG_NONE && u.upd_wait != 2 &&
                (u.upd_wait == 1 || strcmp(later, uv))) {
                u.dlg = DLG_UPDATE;
                u.upd_wait = 0;
                force = 1;
            }
            if (u.dlg == DLG_UPDINFO)
                force = 1; /* the text follows the state */
            if (u.upd_wait == 2 && !strcmp(ust, "installed")) {
                draw(&u, &msg, &act);
                sleep_ms(1500);
                /* start again with the new program (same process, so launch.sh keeps waiting for us) */
                agent_stop();
                char exe[1100];
                snprintf(exe, sizeof(exe), "%s/claude-agent", g_home);
                if (input_fd >= 0)
                    close(input_fd);
                execl(exe, "claude-agent", "ui", (char *)NULL);
                u.upd_wait = 0; /* exec failed: carry on with this one */
            }
        }
        if (u.wifi_hw && now_ms() > next_ip_check) {
            int had = u.ip[0] != 0;
            has_ip(u.ip, sizeof(u.ip));
            if (u.ip[0] && !had) {
                u.note[0] = 0;
                agent_start(!paired_before);
                spawn_self("update.out", "update", "check", "--auto");
            }
            next_ip_check = now_ms() + 3000;
        }
        if (now_ms() > u.next_sys) {
            sys_status(&u);
            char sig[48];
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(sig, sizeof(sig), "%d %d %d %d:%d", u.bat, u.charging, u.wifi_q, tm.tm_hour, tm.tm_min);
            if (strcmp(sig, u.sys_sig)) {
                snprintf(u.sys_sig, sizeof(u.sys_sig), "%s", sig);
                force = 1;
            }
            u.next_sys = now_ms() + 3000;
        }
        if (pane_load(&msg, 1))
            force = 1;
        if (pane_load(&act, 0)) {
            force = 1;
            last_act_change = now_ms();
            u.busy_until = now_ms() + 2500;
        }
        if (u.dlg == DLG_NONE && read_small(PERM_ASK, u.ask, sizeof(u.ask)) > 0)
            u.dlg = DLG_PERM;
        if (u.dlg == DLG_NONE && access(BG_REQUEST, F_OK) == 0)
            u.dlg = DLG_BACKGROUND;
        if (u.dlg == DLG_PERM && access(PERM_ASK, F_OK) != 0)
            u.dlg = DLG_NONE; /* the question timed out */

        char key[1400];
        snprintf(key, sizeof(key), "%s|%s|%s|%d|%d|%d|%d|%d|%d|%d|%d", u.status, u.ip, u.note, u.screen,
                 u.set_sel, u.dlg, u.focus, msg.top, act.top, access(BG_FLAG, F_OK), u.warn_item);
        int animating = now_ms() < u.busy_until;
        if (force || animating || strcmp(key, last_key) != 0) {
            draw(&u, &msg, &act);
            snprintf(last_key, sizeof(last_key), "%s", key);
            force = 0;
            if (getenv("CA_UI_ONCE"))
                return 0;
        }
        (void)last_act_change;
        if (sys_redraw_at && now_ms() >= sys_redraw_at) {
            sys_redraw_at = 0;
            force = 1;
            continue;
        }

        int k = input_fd >= 0 ? input_poll(animating ? 160 : 200) : (usleep(200000), 0);
        if (!k)
            continue;
        force = 1;

        /* dialogs first */
        if (u.dlg == DLG_WELCOME) {
            if (k == KEY_SPACE) {
                setting_set("welcome_ok", 1);
                u.dlg = DLG_NONE;
            }
            else if (k == KEY_LEFTCTRL || k == KEY_ESC)
                leave = 1;
            continue;
        }
        if (u.dlg == DLG_PERM) {
            if (k == KEY_SPACE || k == KEY_LEFTCTRL) {
                FILE *f = fopen(PERM_ANSWER, "w");
                if (f) {
                    fputs(k == KEY_SPACE ? "allow\n" : "deny\n", f);
                    fclose(f);
                }
                u.dlg = DLG_NONE;
            }
            continue;
        }
        if (u.dlg == DLG_BACKGROUND) {
            if (k == KEY_SPACE || k == KEY_LEFTCTRL) {
                FILE *f = fopen(BG_ANSWER, "w");
                if (f) {
                    fputs(k == KEY_SPACE ? "allow\n" : "deny\n", f);
                    fclose(f);
                }
                if (k == KEY_SPACE)
                    touch(BG_FLAG);
                unlink(BG_REQUEST);
                u.dlg = DLG_NONE;
            }
            continue;
        }
        if (u.dlg == DLG_EMPTY) {
            if (k == KEY_SPACE) {
                long long freed = backups_empty();
                activity("Backups emptied: %.1f MB freed", freed / 1024.0);
                u.bk_kb = -1;
            }
            if (k == KEY_SPACE || k == KEY_LEFTCTRL || k == KEY_ESC)
                u.dlg = DLG_NONE;
            continue;
        }
        if (u.dlg == DLG_WARN) {
            if (k == KEY_SPACE)
                apply_setting(&u, u.warn_item, u.warn_to);
            if (k == KEY_SPACE || k == KEY_LEFTCTRL)
                u.dlg = DLG_NONE;
            continue;
        }
        if (u.dlg == DLG_UPDATE) {
            if (k == KEY_SPACE) {
                spawn_self("update.out", "update", "install", NULL);
                u.upd_wait = 2;
                u.dlg = DLG_UPDINFO;
            }
            else if (k == KEY_LEFTCTRL) {
                char uv[64];
                update_get("version", uv, sizeof(uv));
                FILE *f = fopen("/tmp/claude_agent_update_later", "w");
                if (f) {
                    fputs(uv, f);
                    fclose(f);
                }
                u.dlg = DLG_NONE;
            }
            continue;
        }
        if (u.dlg == DLG_UPDINFO) {
            if (k == KEY_LEFTCTRL || k == KEY_SPACE)
                u.dlg = DLG_NONE;
            continue;
        }
        if (u.dlg == DLG_UNDO) {
            if (k == KEY_SPACE && u.ch_sel < u.nch) {
                change_undo(u.ch[u.ch_sel].id, u.undo_msg, sizeof(u.undo_msg));
                free(u.ch);
                u.nch = changes_load(&u.ch);
            }
            if (k == KEY_SPACE || k == KEY_LEFTCTRL)
                u.dlg = DLG_NONE;
            continue;
        }
        if (u.dlg == DLG_PLWARN || u.dlg == DLG_PLREMOVE) {
            if (k == KEY_SPACE && u.pl_sel < u.npl) {
                if (u.dlg == DLG_PLWARN) {
                    char key[80];
                    snprintf(key, sizeof(key), "plugin_%s", u.pl[u.pl_sel]);
                    setting_set_num(key, u.pl_to);
                    activity("Setting: plugin %s: don't ask", u.pl[u.pl_sel]);
                }
                else {
                    char old[1400], pd[1200], stamp[32];
                    time_t t = time(NULL);
                    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
                    snprintf(pd, sizeof(pd), "%s/plugins/%s", g_home, u.pl[u.pl_sel]);
                    snprintf(old, sizeof(old), "%s/backups/%s-plugin-%s", g_home, stamp, u.pl[u.pl_sel]);
                    snprintf(path, sizeof(path), "%s/backups", g_home);
                    mkdir(path, 0755);
                    if (rename(pd, old) == 0)
                        activity("Plugin removed: %s", u.pl[u.pl_sel]);
                    free(u.pl);
                    u.npl = plugins_list(&u.pl);
                    if (u.pl_sel >= u.npl)
                        u.pl_sel = u.npl ? u.npl - 1 : 0;
                }
                pid_t p = agent_pid();
                if (p)
                    kill(p, SIGHUP);
            }
            if (k == KEY_SPACE || k == KEY_LEFTCTRL)
                u.dlg = DLG_NONE;
            continue;
        }
        if (u.dlg == DLG_UNPAIR) {
            if (k == KEY_SPACE) {
                pid_t p = agent_pid();
                if (p)
                    kill(p, SIGUSR2);
                snprintf(path, sizeof(path), "%s/config/paired", g_home);
                unlink(path);
                paired_before = 0;
                activity("Disconnected every Claude");
            }
            if (k == KEY_SPACE || k == KEY_LEFTCTRL)
                u.dlg = DLG_NONE;
            continue;
        }

        if (u.screen == 4) {
            if (k == KEY_UP && u.pl_sel > 0)
                u.pl_sel--;
            else if (k == KEY_DOWN && u.pl_sel < u.npl - 1)
                u.pl_sel++;
            else if ((k == KEY_SPACE || k == KEY_LEFT || k == KEY_RIGHT) && u.npl) {
                int v = plugin_mode(u.pl[u.pl_sel]);
                int to = k == KEY_LEFT ? v - 1 : k == KEY_RIGHT ? v + 1 : (v + 1) % 4;
                if (to >= 0 && to < 4 && to != v) {
                    if (to == 3) {
                        u.pl_to = to;
                        u.dlg = DLG_PLWARN;
                    }
                    else {
                        char key[80];
                        snprintf(key, sizeof(key), "plugin_%s", u.pl[u.pl_sel]);
                        setting_set_num(key, to);
                        snprintf(key, sizeof(key), "%splugin_%s", PERM_GRANT, u.pl[u.pl_sel]);
                        unlink(key);
                        static const char *pm[4] = {"off", "ask every time", "ask once", "don't ask"};
                        activity("Setting: plugin %s: %s", u.pl[u.pl_sel], T(pm[to]));
                        pid_t p = agent_pid();
                        if (p)
                            kill(p, SIGHUP);
                    }
                }
            }
            else if (k == KEY_LEFTALT && u.npl)
                u.dlg = DLG_PLREMOVE;
            else if (k == KEY_LEFTCTRL || k == KEY_ESC) {
                u.screen = 2;
                u.set_sel = menu_index(menu_dev, DEV_COUNT, 13);
            }
            else if (k == KEY_ENTER)
                u.screen = 0;
            continue;
        }
        if (u.screen == 3) {
            if (k == KEY_UP && u.ch_sel > 0)
                u.ch_sel--;
            else if (k == KEY_DOWN && u.ch_sel < u.nch - 1)
                u.ch_sel++;
            else if (k == KEY_SPACE && u.nch) {
                if (u.ch[u.ch_sel].undone)
                    snprintf(u.undo_msg, sizeof(u.undo_msg), "This change was already undone.");
                else
                    u.dlg = DLG_UNDO;
                continue;
            }
            else if (k == KEY_LEFTCTRL || k == KEY_ESC) {
                u.screen = 1;
                u.set_sel = menu_index(menu_main, MAIN_COUNT, 12);
            }
            else if (k == KEY_ENTER)
                u.screen = 0;
            u.undo_msg[0] = 0;
            continue;
        }

        if (u.screen == 1 || u.screen == 2) {
            const int *menu = u.screen == 1 ? menu_main : menu_dev;
            int count = u.screen == 1 ? MAIN_COUNT : DEV_COUNT;
            if (k == KEY_UP)
                u.set_sel = (u.set_sel + count - 1) % count;
            else if (k == KEY_DOWN)
                u.set_sel = (u.set_sel + 1) % count;
            else if (k == KEY_LEFTCTRL || k == KEY_ESC || k == KEY_ENTER) {
                if (u.screen == 2 && k != KEY_ENTER) { /* back to the settings, on "Developer options" */
                    u.screen = 1;
                    u.set_sel = menu_index(menu_main, MAIN_COUNT, 9);
                }
                else
                    u.screen = 0;
            }
            else if (k == KEY_SPACE || k == KEY_LEFT || k == KEY_RIGHT) {
                int i = menu[u.set_sel], v = set_value(i), n = set_choices(i);
                if (n) {
                    int to = k == KEY_LEFT ? v - 1 : k == KEY_RIGHT ? v + 1 : (v + 1) % n;
                    if (to >= 0 && to < n && to != v) {
                        if (to > v && to > set_default(i)) { /* lowers security: warn first */
                            u.warn_item = i;
                            u.warn_to = to;
                            u.dlg = DLG_WARN;
                        }
                        else
                            apply_setting(&u, i, to);
                    }
                }
                else if (k != KEY_SPACE)
                    ;
                else if (i == 7) {
                    pid_t p = agent_pid();
                    if (p)
                        kill(p, SIGUSR1);
                    else if (u.ip[0])
                        agent_start(1);
                    u.screen = 0;
                }
                else if (i == 8)
                    u.dlg = DLG_UNPAIR;
                else if (i == 9) {
                    u.screen = 2;
                    u.set_sel = 0;
                }
                else if (i == 11) {
                    spawn_self("update.out", "update", "check", NULL);
                    u.upd_wait = 1;
                    u.dlg = DLG_UPDINFO;
                }
                else if (i == 13) {
                    free(u.pl);
                    u.npl = plugins_list(&u.pl);
                    u.pl_sel = 0;
                    u.undo_msg[0] = 0;
                    u.screen = 4;
                }
                else if (i == 12) {
                    free(u.ch);
                    u.nch = changes_load(&u.ch);
                    u.ch_sel = 0;
                    u.undo_msg[0] = 0;
                    u.screen = 3;
                }
            }
            continue;
        }

        pane_t *fp = u.focus == 0 ? &msg : &act;
        switch (k) {
        case KEY_UP:
            fp->follow = 0;
            fp->top--;
            pane_clamp(fp);
            break;
        case KEY_DOWN:
            fp->top++;
            pane_clamp(fp);
            break;
        case KEY_E:  /* L1 */
        case KEY_T:  /* R1 */
        case KEY_LEFT:
        case KEY_RIGHT:
            u.focus ^= 1;
            break;
        case KEY_ENTER: /* START */
            u.screen = 1;
            u.set_sel = 0;
            u.bk_kb = -1;
            break;
        case KEY_LEFTCTRL: /* B */
        case KEY_ESC:      /* MENU */
            leave = 1;
            break;
        }
    }

    int keep = access(BG_FLAG, F_OK) == 0;
    if (!keep) {
        agent_stop();
        perm_clear_session();
        if (wifi_started) {
            snprintf(path, sizeof(path), "'%s/wifi.sh' restore", g_home);
            if (system(path) != 0) {
            }
        }
    }
    unlink(BG_REQUEST);
    unlink(PERM_ASK);
    fill(0, 0, W, H, 0xff000000);
    screen_present();
    memset(fb_mem, 0, fb_len);
    return 0;
}
