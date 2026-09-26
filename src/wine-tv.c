#define _GNU_SOURCE

/* Entry point of the Wine app for webOS TV (OLED55C56LB, webOS 25).
 *
 * webOS starts this program when the app icon is selected. It runs Wine on
 * the TV itself:
 *
 *   Xvfb        an X server whose screen is a file in /tmp (-fbdir)
 *   wine        Wine built for ARM64, running natively on the TV's
 *               Cortex-A76 cores, drawing into that X server. 32-bit Windows
 *               programs run under Wine's WoW64 layer; only their x86 code
 *               is translated, by FEX's libwow64fex.dll inside Wine.
 *   this        a Wayland client of the TV's compositor (LSM): it shows the
 *               X screen fullscreen, straight from Xvfb's file, and feeds the
 *               Magic Remote and keyboard back into X through XTEST.
 *
 * This program and Xvfb are armhf and use the app's rt/lib; Wine is aarch64
 * and uses rt64/lib. Neither uses the TV's own (softfp) libraries; only the
 * Wayland wire protocol is shared with the TV.
 *
 * Launch parameters (webOS "params" JSON, or argv when run by hand):
 *   {"exe":"C:\\path\\to\\program.exe"}  run a program inside the desktop
 * Settings, one KEY=VALUE per line, in home/wine-tv.conf:
 *   size=1280x720   the X screen and Wine desktop size
 *   program=...     what the desktop runs when no exe is given
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

#include <wayland-client.h>
#include "text-model-client-protocol.h"
#include "webos-shell-client-protocol.h"

#define APP_ID "com.github.gprot42.wine"
#define X_DISPLAY_NUM 7
#define FB_DIR "/tmp/wine-tv"
/* "Get Apps" shortcuts in Wine drop one empty file per request here, named
 * after a winetricks verb (build/make-prefix.sh). */
#define REQ_DIR FB_DIR "/req"
/* "1" while a Windows text field has the caret, else "0": written by
 * tools/wine-tv-kbd.exe (src/wine-tv-kbd.c), which runs inside Wine. */
#define KBD_FILE FB_DIR "/kbd"

/* Evdev codes the Magic Remote sends that <linux/input.h> may not name. */
#define RC_KEY_BACK 158
#define RC_KEY_EXIT 174
#define RC_KEY_RED 398
#define RC_KEY_GREEN 399
#define RC_KEY_YELLOW 400
#define RC_KEY_BLUE 401

static struct {
    char dir[PATH_MAX];
    char home[PATH_MAX];
    FILE *log;

    int width, height;           /* X screen */
    pid_t xvfb_pid, wine_pid, install_pid;
    struct timespec wine_started;
    char program[1024];          /* what the desktop opens */
    volatile sig_atomic_t running;
    int sigpipe[2];

    /* Xvfb's screen, mapped from its -fbdir file. */
    int fb_fd;
    size_t fb_size;
    uint32_t fb_offset, fb_stride;

    Display *x;
    int damage_event;
    int fixes_event;
    int cursor_ibeam;            /* X shows the text cursor (I-beam) */
    Damage damage;
    int dirty;                   /* X screen changed since the last commit */
    int kb_spare;                /* keycode borrowed for characters with no key */

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_shell *shell;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_webos_shell *webos_shell;
    struct text_model_factory *text_factory;
    struct text_model *text;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell_surface *webos_surface;
    struct wl_buffer *buffer;
    struct wl_callback *frame;
    struct timespec frame_at;    /* when the pending frame was requested */
    unsigned presents, stale_frames;
    int settle;                  /* one more frame is due once changes stop */
    uint32_t serial;
    int keyboard_up;
    char kbd_state;              /* last value read from KBD_FILE */
    /* The webOS keyboard's area and the remote's position, both in the
     * panel's 1920x1080 grid: clicks there belong to the keyboard. */
    int panel_x, panel_y, panel_w, panel_h;
    int remote_x, remote_y;

    /* Pointer position in X screen coordinates. */
    int px, py;
    int pointer_in;
    /* Compositors on some firmware report the pointer in the panel's
     * 1920x1080 grid rather than in surface coordinates. Switch to scaling
     * as soon as a position outside the surface shows which one this is. */
    int ptr_space_w, ptr_space_h;

    int evdev[16];
    int n_evdev;
    int wl_input;               /* the compositor delivers pointer/keys */
} g;

static void show_keyboard(void);
static void hide_keyboard(void);

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "wine-tv fatal: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (g.wine_pid > 0 || g.xvfb_pid > 0)
        kill(0, SIGTERM);
    exit(1);
}

static void mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    size_t i, len;

    snprintf(buf, sizeof buf, "%s", path);
    len = strlen(buf);
    for (i = 1; i < len; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        mkdir(buf, 0755);
        buf[i] = '/';
    }
    mkdir(buf, 0755);
}

static void path_join(char *out, size_t n, const char *a, const char *b)
{
    snprintf(out, n, "%s/%s", a, b);
}

/* ---------------------------------------------------------------- setup */

static void open_log(void)
{
    char path[PATH_MAX], old[PATH_MAX];
    struct stat st;
    int fd;

    path_join(path, sizeof path, g.dir, "wine-tv.log");
    if (stat(path, &st) == 0 && st.st_size > 4 * 1024 * 1024) {
        path_join(old, sizeof old, g.dir, "wine-tv.log.1");
        rename(path, old);
    }
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
}

static char conf_program[1024];

static void read_conf(void)
{
    char path[PATH_MAX], line[1200];
    FILE *f;

    g.width = 1280;
    g.height = 720;
    path_join(path, sizeof path, g.home, "wine-tv.conf");
    f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs("# wine-tv settings, one KEY=VALUE per line.\n"
                  "# size: the Wine desktop, scaled to the panel by the TV.\n"
                  "size=1280x720\n"
                  "# program: what the desktop opens when the app is launched\n"
                  "# without {\"exe\":...}. Empty for just the desktop.\n"
                  "program=winefile\n", f);
            fclose(f);
        }
        snprintf(conf_program, sizeof conf_program, "winefile");
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (line[0] == '#' || !eq)
            continue;
        *eq++ = '\0';
        if (!strcmp(line, "size")) {
            int w, h;
            if (sscanf(eq, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 3840 && h <= 2160) {
                g.width = w & ~7;
                g.height = h;
            }
        } else if (!strcmp(line, "program")) {
            snprintf(conf_program, sizeof conf_program, "%s", eq);
        }
    }
    fclose(f);
}

/* The "exe" string of the webOS launch parameters. JSON escapes other than
 * \\ and \" are not decoded. */
static int launch_exe(const char *json, char *out, size_t n)
{
    const char *p = strstr(json, "\"exe\"");
    size_t len = 0;

    if (!p)
        return 0;
    p += 5;
    while (*p == ' ' || *p == ':')
        p++;
    if (*p++ != '"')
        return 0;
    while (*p && *p != '"' && len + 1 < n) {
        if (*p == '\\' && (p[1] == '\\' || p[1] == '"'))
            p++;
        out[len++] = *p++;
    }
    out[len] = '\0';
    return len > 0;
}

static void setup_env(void)
{
    char buf[PATH_MAX * 2];

    setenv("HOME", g.home, 1);
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    snprintf(buf, sizeof buf, ":%d", X_DISPLAY_NUM);
    setenv("DISPLAY", buf, 1);
    setenv("LANG", "C.UTF-8", 0);
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");

    snprintf(buf, sizeof buf, "%s/rt/share/X11/locale", g.dir);
    setenv("XLOCALEDIR", buf, 1);
    snprintf(buf, sizeof buf, "%s/rt/etc/fonts/fonts.conf", g.dir);
    setenv("FONTCONFIG_FILE", buf, 1);

    /* "wine64": a prefix made by 64-bit Wine. Earlier versions of this app
     * ran 32-bit Wine, whose prefix (".wine") 64-bit Wine cannot use. */
    snprintf(buf, sizeof buf, "%s/wine64", g.home);
    setenv("WINEPREFIX", buf, 1);
    setenv("WINEDEBUG", "-all", 0);
    /* No Mono download dialog, no desktop menu entries (there is no Linux
     * desktop to put them in). mshtml (the IE engine) stays on: programs that
     * embed IE need it, with Wine Gecko from Get Apps. */
    setenv("WINEDLLOVERRIDES", "mscoree=;winemenubuilder.exe=d", 0);
    snprintf(buf, sizeof buf, "%s/wine/bin/wineserver", g.dir);
    setenv("WINESERVER", buf, 1);
    /* The TV's PulseAudio, which the app jail shares. */
    if (access("/var/run/pulse/native", F_OK) == 0)
        setenv("PULSE_SERVER", "unix:/var/run/pulse/native", 0);
    /* Copy-on-write for the prefix's symlinked system files: the app's
     * aarch64 loader preloads what this file names (src/cow-preload.c). */
    {
        FILE *f;

        mkdir_p(FB_DIR);
        f = fopen(FB_DIR "/prelo", "w");
        if (f) {
            fprintf(f, "%s/rt64/lib/libwine-tv-cow.so\n", g.dir);
            fclose(f);
        }
    }

    /* KEY=VALUE lines in <app dir>/env override the above for debugging,
     * e.g. WINEDEBUG=err+all. */
    {
        FILE *f;
        char line[1024];

        path_join(buf, sizeof buf, g.dir, "env");
        f = fopen(buf, "r");
        while (f && fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            if (!eq || line[0] == '#' || eq == line)
                continue;
            *eq = '\0';
            setenv(line, eq + 1, 1);
            log_msg("env %s=%s", line, eq + 1);
        }
        if (f)
            fclose(f);
    }
}

/* Removable drives show up under /tmp/usb/<dev>/<partition>; give them
 * drive letters from D: on. Refreshed at every start. */
static void map_drives(void)
{
    char dosdev[PATH_MAX], link[PATH_MAX];
    const char *targets[] = { "/tmp/usb", "/media/internal" };
    char letter = 'd';
    size_t i;

    snprintf(dosdev, sizeof dosdev, "%s/wine64/dosdevices", g.home);
    if (access(dosdev, F_OK) != 0)
        return;
    for (i = 0; i < sizeof targets / sizeof *targets; i++, letter++) {
        snprintf(link, sizeof link, "%s/%c:", dosdev, letter);
        unlink(link);
        if (access(targets[i], R_OK) == 0 && symlink(targets[i], link) == 0)
            log_msg("drive %c: -> %s", letter - 32, targets[i]);
    }
}

static void on_signal(int sig)
{
    int saved = errno;
    char c = (char)sig;

    if (sig != SIGCHLD)
        g.running = 0;
    if (write(g.sigpipe[1], &c, 1) < 0) {
    }
    errno = saved;
}

static pid_t spawn(char *const argv[], const char *what)
{
    pid_t pid = fork();

    if (pid < 0)
        die("fork %s: %s", what, strerror(errno));
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        execv(argv[0], argv);
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    log_msg("started %s pid %d", what, (int)pid);
    return pid;
}

static void start_xvfb(void)
{
    char xvfb[PATH_MAX], disp[16], screen[64];
    char *argv[20];
    int n = 0;

    mkdir_p(FB_DIR);
    mkdir_p(REQ_DIR);
    chmod(REQ_DIR, 0777);
    unlink(KBD_FILE);
    g.kbd_state = '0';
    mkdir_p("/tmp/wine-xkb");
    snprintf(xvfb, sizeof xvfb, "%s/bin/Xvfb", g.dir);
    snprintf(disp, sizeof disp, ":%d", X_DISPLAY_NUM);
    snprintf(screen, sizeof screen, "%dx%dx24", g.width, g.height);
    /* A stale server from a crashed run still holds the display. */
    {
        char lock[64];
        snprintf(lock, sizeof lock, "/tmp/.X%d-lock", X_DISPLAY_NUM);
        unlink(lock);
    }
    argv[n++] = xvfb;
    argv[n++] = disp;
    argv[n++] = "-screen";
    argv[n++] = "0";
    argv[n++] = screen;
    argv[n++] = "-fbdir";
    argv[n++] = FB_DIR;
    argv[n++] = "-nolisten";
    argv[n++] = "tcp";
    argv[n++] = "-dpi";
    argv[n++] = "96";
    argv[n++] = "-noreset";
    argv[n++] = "+extension";
    argv[n++] = "XTEST";
    argv[n++] = "+extension";
    argv[n++] = "DAMAGE";
    argv[n] = NULL;
    g.xvfb_pid = spawn(argv, "Xvfb");
}

static void start_wine(const char *exe)
{
    char wine[PATH_MAX], desktop[64];
    char *argv[12];
    int n = 0;

    snprintf(wine, sizeof wine, "%s/wine/bin/wine", g.dir);
    /* A desktop named "shell" gets Wine's taskbar and Start menu. */
    snprintf(desktop, sizeof desktop, "/desktop=shell,%dx%d", g.width, g.height);
    argv[n++] = wine;
    argv[n++] = "explorer";
    argv[n++] = desktop;
    if (exe && exe[0])
        argv[n++] = (char *)exe;
    argv[n] = NULL;
    log_msg("wine explorer %s %s", desktop, exe && exe[0] ? exe : "");
    clock_gettime(CLOCK_MONOTONIC, &g.wine_started);
    g.wine_pid = spawn(argv, "wine");
    /* The helper that reports text-field focus (one per session: it exits
     * if one is already running). */
    {
        char helper[PATH_MAX];
        char *hargv[] = { wine, helper, NULL };

        snprintf(helper, sizeof helper, "%s/tools/wine-tv-kbd.exe", g.dir);
        if (access(helper, R_OK) == 0)
            spawn(hargv, "wine-tv-kbd");
    }
}

/* The prefix (C: drive) is built with the app (build/make-prefix.sh) and
 * shipped as prefix-template/, whose system DLLs are symlinks into wine/.
 * Copy it on first start; creating one here would copy ~500 MB of DLLs. */
static void install_prefix(void)
{
    char prefix[PATH_MAX], tmpl[PATH_MAX], cmd[PATH_MAX * 2 + 32];
    int status;

    snprintf(prefix, sizeof prefix, "%s/wine64", g.home);
    snprintf(tmpl, sizeof tmpl, "%s/prefix-template", g.dir);
    if (access(tmpl, F_OK) != 0)
        return;
    if (access(prefix, F_OK) == 0) {
        /* Never let Wine update the prefix on an app update: that rewrites
         * every system DLL, and copy-on-write would turn each symlink into a
         * full copy (build/make-prefix.sh). */
        {
            char ts[PATH_MAX];
            FILE *f;

            snprintf(ts, sizeof ts, "%s/.update-timestamp", prefix);
            f = fopen(ts, "r");
            if (!f || !fgets(cmd, sizeof cmd, f) || strncmp(cmd, "disable", 7) != 0) {
                if (f)
                    fclose(f);
                f = fopen(ts, "w");
                if (f) {
                    fputs("disable\n", f);
                    log_msg("prefix updates disabled");
                }
            }
            if (f)
                fclose(f);
        }
        /* Refresh the "Get Apps" shortcuts (desktop and Start menu) from
         * this version of the app: they are generated, never the user's. */
        static const char *const dirs[] = {
            "drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Get Apps",
            "drive_c/users/Public/Desktop/Get Apps.lnk",
        };
        size_t i;

        for (i = 0; i < sizeof dirs / sizeof *dirs; i++) {
            char have[PATH_MAX], from[PATH_MAX];

            snprintf(have, sizeof have, "%s/%s", prefix, dirs[i]);
            snprintf(from, sizeof from, "%s/%s", tmpl, dirs[i]);
            if (access(from, F_OK) != 0)
                continue;
            snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p \"$(dirname '%s')\" && cp -R '%s' '%s'",
                     have, have, from, have);
            if (system(cmd) != 0)
                log_msg("refreshing %s failed", dirs[i]);
        }
        return;
    }
    log_msg("first start: copying the prefix from %s", tmpl);
    snprintf(cmd, sizeof cmd, "cp -a '%s' '%s.new' && mv '%s.new' '%s'", tmpl, prefix, prefix, prefix);
    status = system(cmd);
    log_msg("prefix copy %s", status == 0 ? "done" : "failed");
}

/* Open or close the webOS keyboard when a Windows text field gains or loses
 * the caret. Only changes act, so Back or Red still close or open it until
 * the focus moves again. */
static void check_text_focus(void)
{
    char c = '0';
    int fd = open(KBD_FILE, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        if (read(fd, &c, 1) != 1)
            c = '0';
        close(fd);
    }
    if (c == g.kbd_state)
        return;
    g.kbd_state = c;
    if (c == '1' && !g.keyboard_up)
        show_keyboard();
    else if (c == '0' && g.keyboard_up)
        hide_keyboard();
}

/* Run the next "Get Apps" request, if any and none is running: Wine cannot
 * start a Unix program from a Windows shortcut, so the shortcut leaves a file
 * and this starts tools/bin/wine-tv-install (winetricks) for it. */
static void run_install_request(void)
{
    char script[PATH_MAX], bash[PATH_MAX], verb[64], path[PATH_MAX];
    char *argv[] = { bash, script, verb, NULL };
    struct dirent *de;
    DIR *dir;

    if (g.install_pid || !(dir = opendir(REQ_DIR)))
        return;
    verb[0] = '\0';
    while ((de = readdir(dir)) != NULL) {
        size_t i, len = strlen(de->d_name);
        int ok = len > 0 && len < sizeof verb;

        if (de->d_name[0] == '.')
            continue;
        for (i = 0; ok && i < len; i++) {
            char c = de->d_name[i];
            ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }
        snprintf(path, sizeof path, "%s/%s", REQ_DIR, de->d_name);
        unlink(path);
        if (ok) {
            memcpy(verb, de->d_name, len + 1);
            break;
        }
        log_msg("ignored install request \"%s\"", de->d_name);
    }
    closedir(dir);
    if (!verb[0])
        return;
    snprintf(bash, sizeof bash, "%s/tools/bin/bash", g.dir);
    snprintf(script, sizeof script, "%s/tools/bin/wine-tv-install", g.dir);
    log_msg("installing %s (winetricks)", verb);
    g.install_pid = spawn(argv, "wine-tv-install");
}

/* Wine processes that outlived a crashed wineserver keep running on their
 * own; kill anything of ours still running from the app's wine directory. */
static void kill_strays(void)
{
    char prefix[PATH_MAX], exe[PATH_MAX], link[64];
    DIR *proc = opendir("/proc");
    struct dirent *de;
    size_t plen;

    if (!proc)
        return;
    snprintf(prefix, sizeof prefix, "%s/wine/", g.dir);
    plen = strlen(prefix);
    while ((de = readdir(proc)) != NULL) {
        pid_t pid = (pid_t)atoi(de->d_name);
        ssize_t len;

        if (pid <= 1 || pid == getpid())
            continue;
        snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
        len = readlink(link, exe, sizeof exe - 1);
        if (len <= 0)
            continue;
        exe[len] = '\0';
        if (strncmp(exe, prefix, plen) == 0 && kill(pid, SIGKILL) == 0)
            log_msg("killed leftover %s pid %d", exe + plen, (int)pid);
    }
    closedir(proc);
}

/* Stop Wine cleanly (wineserver -k), then everything else we started. */
static void stop_all(void)
{
    char server[PATH_MAX];
    char *argv[] = { server, "-k", NULL };
    pid_t pid;
    int i;

    snprintf(server, sizeof server, "%s/wine/bin/wineserver", g.dir);
    pid = spawn(argv, "wineserver -k");
    for (i = 0; i < 50; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            break;
        usleep(100000);
    }
    signal(SIGTERM, SIG_IGN);
    kill(0, SIGTERM);
    usleep(300000);
    kill_strays();
    kill(0, SIGKILL);
}

/* ------------------------------------------------------------- X server */

static int x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    log_msg("X error %d request %d.%d", e->error_code, e->request_code, e->minor_code);
    return 0;
}

static void connect_x(void)
{
    int i, err, ev, maj, min;

    for (i = 0; i < 300 && !g.x; i++) {
        int status;
        if (waitpid(g.xvfb_pid, &status, WNOHANG) == g.xvfb_pid)
            die("Xvfb exited with status %d", status);
        g.x = XOpenDisplay(NULL);
        if (!g.x)
            usleep(100000);
    }
    if (!g.x)
        die("no X display after 30 s");
    XSetErrorHandler(x_error);
    if (!XTestQueryExtension(g.x, &ev, &err, &maj, &min))
        die("Xvfb has no XTEST");
    if (!XDamageQueryExtension(g.x, &g.damage_event, &err))
        die("Xvfb has no DAMAGE");
    XFixesQueryExtension(g.x, &g.fixes_event, &err);
    XFixesQueryVersion(g.x, &maj, &min);
    /* Cursor changes show when the pointer is over a text field. */
    XFixesSelectCursorInput(g.x, DefaultRootWindow(g.x), XFixesDisplayCursorNotifyMask);
    g.damage = XDamageCreate(g.x, DefaultRootWindow(g.x), XDamageReportNonEmpty);
    /* The TV draws the Magic Remote pointer; X's own would be a second one,
     * drawn into the picture a little behind it. */
    XFixesHideCursor(g.x, DefaultRootWindow(g.x));
    /* A keycode with no keys on it, for typing characters the US layout
     * lacks (the webOS keyboard offers accents and other scripts). */
    {
        int lo, hi, per, kc;
        KeySym *map;

        XDisplayKeycodes(g.x, &lo, &hi);
        map = XGetKeyboardMapping(g.x, lo, hi - lo + 1, &per);
        for (kc = hi; kc >= lo && map; kc--) {
            int j, empty = 1;
            for (j = 0; j < per; j++)
                if (map[(kc - lo) * per + j] != NoSymbol)
                    empty = 0;
            if (empty) {
                g.kb_spare = kc;
                break;
            }
        }
        if (map)
            XFree(map);
    }
    XSync(g.x, False);
    log_msg("X connected, %dx%d, spare keycode %d", DisplayWidth(g.x, 0), DisplayHeight(g.x, 0),
            g.kb_spare);
}

/* Xvfb -fbdir writes the screen as an XWD file: a big-endian header, the
 * colormap, then the pixels, which the server keeps drawing into. */
static void map_framebuffer(void)
{
    char path[PATH_MAX];
    unsigned char hdr[100];
    uint32_t header_size, ncolors, bpp, width, height;
    struct stat st;
    int i;

    snprintf(path, sizeof path, "%s/Xvfb_screen0", FB_DIR);
    for (i = 0; i < 100; i++) {
        /* Read-write: the compositor maps a wl_shm pool read-write and
         * fails with EACCES on a read-only descriptor. */
        g.fb_fd = open(path, O_RDWR | O_CLOEXEC);
        if (g.fb_fd >= 0)
            break;
        usleep(100000);
    }
    if (g.fb_fd < 0)
        die("open %s: %s", path, strerror(errno));
    if (pread(g.fb_fd, hdr, sizeof hdr, 0) != (ssize_t)sizeof hdr)
        die("short XWD header");
#define BE32(o) ((uint32_t)hdr[o] << 24 | (uint32_t)hdr[o + 1] << 16 | (uint32_t)hdr[o + 2] << 8 | hdr[o + 3])
    header_size = BE32(0);
    width = BE32(16);
    height = BE32(20);
    bpp = BE32(44);
    g.fb_stride = BE32(48);
    ncolors = BE32(76);
#undef BE32
    g.fb_offset = header_size + ncolors * 12;
    fstat(g.fb_fd, &st);
    g.fb_size = st.st_size;
    log_msg("framebuffer %ux%u bpp %u stride %u offset %u file %zu", width, height, bpp,
            g.fb_stride, g.fb_offset, g.fb_size);
    if (bpp != 32 || (int)width != g.width || (int)height != g.height)
        die("unexpected Xvfb framebuffer %ux%u bpp %u", width, height, bpp);
    if (g.fb_offset + (size_t)g.fb_stride * height > g.fb_size)
        die("framebuffer file is short");
}

/* ------------------------------------------------------------ X input */

static void x_key(int evdev_code, int down)
{
    if (evdev_code <= 0 || evdev_code + 8 > 255)
        return;
    XTestFakeKeyEvent(g.x, evdev_code + 8, down ? True : False, CurrentTime);
    XFlush(g.x);
}

static void x_tap_key(int evdev_code)
{
    x_key(evdev_code, 1);
    x_key(evdev_code, 0);
}

static void x_button(int button, int down)
{
    XTestFakeButtonEvent(g.x, button, down ? True : False, CurrentTime);
    XFlush(g.x);
    /* A click on a text field opens the webOS keyboard; a click anywhere
     * else in Wine closes it, unless a native text field still has the
     * caret (check_text_focus). */
    if (button == 1 && !down) {
        if (g.cursor_ibeam && !g.keyboard_up)
            show_keyboard();
        else if (!g.cursor_ibeam && g.keyboard_up && g.kbd_state != '1')
            hide_keyboard();
    }
}

static void x_motion(int x, int y)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= g.width)
        x = g.width - 1;
    if (y >= g.height)
        y = g.height - 1;
    g.px = x;
    g.py = y;
    XTestFakeMotionEvent(g.x, 0, x, y, CurrentTime);
    XFlush(g.x);
}

/* Type one Unicode character: press its key (with Shift if needed), or put
 * it on the spare keycode first. */
static void x_type_char(uint32_t cp)
{
    KeySym sym;
    KeyCode kc;
    int shift = 0;

    if (cp == '\n') {
        x_tap_key(KEY_ENTER);
        return;
    }
    sym = cp < 0x100 ? cp : 0x01000000 | cp;
    kc = XKeysymToKeycode(g.x, sym);
    if (kc) {
        if (XkbKeycodeToKeysym(g.x, kc, 0, 0) != sym)
            shift = XkbKeycodeToKeysym(g.x, kc, 0, 1) == sym;
    } else if (g.kb_spare) {
        KeySym syms[2] = { sym, sym };
        kc = g.kb_spare;
        XChangeKeyboardMapping(g.x, kc, 2, syms, 1);
        XSync(g.x, False);
    } else {
        return;
    }
    if (shift)
        XTestFakeKeyEvent(g.x, KEY_LEFTSHIFT + 8, True, CurrentTime);
    XTestFakeKeyEvent(g.x, kc, True, CurrentTime);
    XTestFakeKeyEvent(g.x, kc, False, CurrentTime);
    if (shift)
        XTestFakeKeyEvent(g.x, KEY_LEFTSHIFT + 8, False, CurrentTime);
    XFlush(g.x);
}

static void x_type_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    while (*p) {
        uint32_t cp;
        int extra;

        if (*p < 0x80) {
            cp = *p++;
            extra = 0;
        } else if ((*p & 0xe0) == 0xc0) {
            cp = *p++ & 0x1f;
            extra = 1;
        } else if ((*p & 0xf0) == 0xe0) {
            cp = *p++ & 0x0f;
            extra = 2;
        } else if ((*p & 0xf8) == 0xf0) {
            cp = *p++ & 0x07;
            extra = 3;
        } else {
            p++;
            continue;
        }
        while (extra-- > 0 && (*p & 0xc0) == 0x80)
            cp = cp << 6 | (*p++ & 0x3f);
        x_type_char(cp);
    }
}

/* ------------------------------------------------------ webOS keyboard */

static void text_commit_string(void *data, struct text_model *m, uint32_t serial, const char *text)
{
    (void)data;
    (void)m;
    (void)serial;
    x_type_utf8(text);
}

static void text_preedit_string(void *data, struct text_model *m, uint32_t serial,
                                const char *text, const char *commit)
{
    (void)data;
    (void)m;
    (void)serial;
    (void)text;
    (void)commit;
}

static void text_delete_surrounding(void *data, struct text_model *m, uint32_t serial,
                                    int32_t index, uint32_t length)
{
    uint32_t i;

    (void)data;
    (void)m;
    (void)serial;
    if (index < 0 && !length)
        length = (uint32_t)(-index);
    for (i = 0; i < length; i++)
        x_tap_key(KEY_BACKSPACE);
}

static void text_cursor_position(void *data, struct text_model *m, uint32_t serial,
                                 int32_t index, int32_t anchor)
{
    (void)data;
    (void)m;
    (void)serial;
    (void)index;
    (void)anchor;
}

static void text_preedit_styling(void *data, struct text_model *m, uint32_t serial,
                                 uint32_t index, uint32_t length, uint32_t style)
{
    (void)data;
    (void)m;
    (void)serial;
    (void)index;
    (void)length;
    (void)style;
}

static void text_preedit_cursor(void *data, struct text_model *m, uint32_t serial, int32_t index)
{
    (void)data;
    (void)m;
    (void)serial;
    (void)index;
}

static void text_modifiers_map(void *data, struct text_model *m, struct wl_array *map)
{
    (void)data;
    (void)m;
    (void)map;
}

/* The webOS keyboard sends editing keys as X11 keysyms. */
static void text_keysym(void *data, struct text_model *m, uint32_t serial, uint32_t time,
                        uint32_t sym, uint32_t state, uint32_t modifiers)
{
    int code = 0;

    (void)data;
    (void)m;
    (void)serial;
    (void)time;
    (void)modifiers;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;
    switch (sym) {
    case 0xff08: code = KEY_BACKSPACE; break;
    case 0xff09: code = KEY_TAB; break;
    case 0xff0d:
    case 0xff8d: code = KEY_ENTER; break;
    case 0xff1b: code = KEY_ESC; break;
    case 0xff50: code = KEY_HOME; break;
    case 0xff51: code = KEY_LEFT; break;
    case 0xff52: code = KEY_UP; break;
    case 0xff53: code = KEY_RIGHT; break;
    case 0xff54: code = KEY_DOWN; break;
    case 0xff57: code = KEY_END; break;
    case 0xffff: code = KEY_DELETE; break;
    default:
        log_msg("keyboard keysym %#x not mapped", sym);
        return;
    }
    x_tap_key(code);
}

static void text_enter(void *data, struct text_model *m, struct wl_surface *surface)
{
    (void)data;
    (void)m;
    (void)surface;
}

static void text_leave(void *data, struct text_model *m)
{
    (void)data;
    (void)m;
    g.keyboard_up = 0;
}

static void text_panel_state(void *data, struct text_model *m, uint32_t state)
{
    (void)data;
    (void)m;
    log_msg("keyboard panel state %u", state);
    if (state == 0)
        g.keyboard_up = 0;
}

static void text_panel_rect(void *data, struct text_model *m, int32_t x, int32_t y,
                            uint32_t w, uint32_t h)
{
    (void)data;
    (void)m;
    log_msg("keyboard panel %d,%d %ux%u", x, y, w, h);
    g.panel_x = x;
    g.panel_y = y;
    g.panel_w = (int)w;
    g.panel_h = (int)h;
}

/* True while the remote points at the open webOS keyboard. The compositor
 * gives those clicks to the keyboard; they must not reach Wine as well
 * (Wine would take them as clicks outside the text field). Until the
 * keyboard reports its area, assume the bottom 45% of the screen. */
static int on_keyboard(void)
{
    if (!g.keyboard_up)
        return 0;
    if (g.panel_w > 0 && g.panel_h > 0 && g.panel_y < 1080)
        return g.remote_x >= g.panel_x && g.remote_x < g.panel_x + g.panel_w &&
               g.remote_y >= g.panel_y && g.remote_y < g.panel_y + g.panel_h;
    return g.remote_y >= 1080 * 55 / 100;
}

static const struct text_model_listener text_listener = {
    text_commit_string,   text_preedit_string, text_delete_surrounding,
    text_cursor_position, text_preedit_styling, text_preedit_cursor,
    text_modifiers_map,   text_keysym,          text_enter,
    text_leave,           text_panel_state,     text_panel_rect,
};

static void show_keyboard(void)
{
    if (!g.text_factory || !g.seat)
        return;
    if (!g.text) {
        g.text = text_model_factory_create_text_model(g.text_factory);
        text_model_add_listener(g.text, &text_listener, NULL);
    }
    text_model_set_content_type(g.text, 0, 0);
    text_model_set_surrounding_text(g.text, "", 0, 0);
    text_model_activate(g.text, ++g.serial, g.seat, g.surface);
    text_model_show_input_panel(g.text);
    g.keyboard_up = 1;
    log_msg("keyboard shown");
}

static void hide_keyboard(void)
{
    if (!g.text || !g.keyboard_up)
        return;
    text_model_hide_input_panel(g.text);
    text_model_deactivate(g.text, g.seat);
    /* This TV destroys the text model on deactivate: using it again was a
     * fatal "invalid object". Start a new one next time (as webos-firefox
     * does on webOS 4). The protocol has no destroy request. */
    wl_proxy_destroy((struct wl_proxy *)g.text);
    g.text = NULL;
    g.panel_w = g.panel_h = 0;
    g.keyboard_up = 0;
    log_msg("keyboard hidden");
}

/* -------------------------------------------------------- Wayland input */

static void pointer_to_x(wl_fixed_t sx, wl_fixed_t sy)
{
    int x = wl_fixed_to_int(sx), y = wl_fixed_to_int(sy);

    if (!g.ptr_space_w && (x > g.width || y > g.height)) {
        g.ptr_space_w = 1920;
        g.ptr_space_h = 1080;
        log_msg("pointer at %d,%d is outside the %dx%d surface: scaling from 1920x1080", x, y,
                g.width, g.height);
    }
    if (g.ptr_space_w) {
        x = x * g.width / g.ptr_space_w;
        y = y * g.height / g.ptr_space_h;
    }
    x_motion(x, y);
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data;
    (void)p;
    (void)serial;
    (void)surface;
    /* No wl_pointer.set_cursor: the TV draws the Magic Remote pointer
     * itself, and a NULL cursor would hide it. */
    g.pointer_in = 1;
    g.wl_input = 1;
    log_msg("pointer enter %d,%d", wl_fixed_to_int(sx), wl_fixed_to_int(sy));
    pointer_to_x(sx, sy);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)data;
    (void)p;
    (void)serial;
    (void)s;
    g.pointer_in = 0;
    log_msg("pointer leave");
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx,
                           wl_fixed_t sy)
{
    (void)data;
    (void)p;
    (void)time;
    {
        static unsigned n;
        if (n++ % 200 == 0)
            log_msg("pointer motion %d,%d (%u events)", wl_fixed_to_int(sx), wl_fixed_to_int(sy), n);
    }
    pointer_to_x(sx, sy);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    int b;

    (void)data;
    (void)p;
    (void)serial;
    (void)time;
    switch (button) {
    case BTN_LEFT: b = 1; break;
    case BTN_MIDDLE: b = 2; break;
    case BTN_RIGHT: b = 3; break;
    default:
        log_msg("pointer button %#x ignored", button);
        return;
    }
    log_msg("pointer button %d %s at %d,%d", b, state ? "down" : "up", g.px, g.py);
    x_button(b, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
                         wl_fixed_t value)
{
    int b;

    (void)data;
    (void)p;
    (void)time;
    /* The remote's wheel is read from evdev when that is open. */
    if (g.n_evdev)
        return;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        b = value > 0 ? 5 : 4;
    else
        b = value > 0 ? 7 : 6;
    x_button(b, 1);
    x_button(b, 0);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
                            uint32_t size)
{
    (void)data;
    (void)k;
    (void)format;
    (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *s, struct wl_array *keys)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)s;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)s;
}

/* Remote buttons with no PC equivalent:
 *   Back    Escape, or closes the webOS keyboard when it is open
 *   Red     opens or closes the webOS keyboard
 *   Green   right-click where the pointer is
 *   Yellow  Alt+Tab
 *   Blue    Windows key (Start menu)
 * Everything else with a PC key code goes to X as that key. */
static void handle_key(int key, int down)
{
    if (down)
        log_msg("key %d", key);
    switch (key) {
    /* The Magic Remote's OK/wheel click: a left click at the pointer. */
    case BTN_LEFT:
    case KEY_OK:
    case KEY_ENTER:
        if (key != KEY_ENTER || !g.keyboard_up) {
            x_button(1, down);
            return;
        }
        break;
    case BTN_RIGHT:
        x_button(3, down);
        return;
    case BTN_MIDDLE:
        x_button(2, down);
        return;
    case 1198: /* the wheel's cursor show/hide keys */
    case 1199:
        return;
    case RC_KEY_BACK:
        if (!down)
            return;
        if (g.keyboard_up)
            hide_keyboard();
        else
            x_tap_key(KEY_ESC);
        return;
    case RC_KEY_EXIT:
        return;
    case RC_KEY_RED:
        if (down) {
            if (g.keyboard_up)
                hide_keyboard();
            else
                show_keyboard();
        }
        return;
    case RC_KEY_GREEN:
        x_button(3, down);
        return;
    case RC_KEY_YELLOW:
        if (down) {
            x_key(KEY_LEFTALT, 1);
            x_tap_key(KEY_TAB);
            x_key(KEY_LEFTALT, 0);
        }
        return;
    case RC_KEY_BLUE:
        if (down)
            x_tap_key(KEY_LEFTMETA);
        return;
    }
    x_key(key, down);
}

static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)time;
    if (!g.wl_input)
        log_msg("Wayland input arrives; evdev now supplies only the wheel");
    g.wl_input = 1;
    handle_key((int)key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)data;
    (void)k;
    (void)serial;
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void)data;
    (void)k;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter,     keyboard_leave,
    keyboard_key,    keyboard_modifiers, keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    log_msg("seat capabilities %#x", caps);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &keyboard_listener, NULL);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

/* Remote and keyboard input read straight from evdev.
 *
 * On this TV the compositor sends a native app no wl_pointer or wl_keyboard
 * events (the seat advertises both, but nothing arrives), and it never
 * passes the Magic Remote's wheel on as a scroll. So pointer position,
 * buttons, keys and the wheel are read from /dev/input, as the Firefox port
 * does. If Wayland input does start arriving, everything but the wheel
 * comes from there instead, so nothing is delivered twice.
 * Input events are 16 bytes on this 32-bit userspace. */
#define MAX_EVDEV 16

static struct {
    int fd;
    int abs_min_x, abs_max_x, abs_min_y, abs_max_y;
    int logged;
} evdevs[MAX_EVDEV];

static int test_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}

static void open_evdev(void)
{
    int i;

    for (i = 0; i < 32 && g.n_evdev < MAX_EVDEV; i++) {
        char path[32], name[128] = "";
        unsigned long evbits[1] = { 0 }, absbits[2] = { 0 }, keybits[KEY_MAX / (8 * sizeof(long)) + 1];
        int fd;

        snprintf(path, sizeof path, "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
            continue;
        memset(keybits, 0, sizeof keybits);
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
        if (!test_bit(evbits, EV_KEY) && !test_bit(evbits, EV_REL) && !test_bit(evbits, EV_ABS)) {
            close(fd);
            continue;
        }
        evdevs[g.n_evdev].fd = fd;
        evdevs[g.n_evdev].abs_max_x = 1920;
        evdevs[g.n_evdev].abs_max_y = 1080;
        if (test_bit(absbits, ABS_X)) {
            struct input_absinfo ax, ay;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ax.maximum > ax.minimum) {
                evdevs[g.n_evdev].abs_min_x = ax.minimum;
                evdevs[g.n_evdev].abs_max_x = ax.maximum;
            }
            if (ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ay.maximum > ay.minimum) {
                evdevs[g.n_evdev].abs_min_y = ay.minimum;
                evdevs[g.n_evdev].abs_max_y = ay.maximum;
            }
        }
        g.evdev[g.n_evdev++] = fd;
        log_msg("input %s \"%s\"%s range %d..%d x %d..%d", path, name,
                test_bit(absbits, ABS_X) ? " pointer" : "", evdevs[g.n_evdev - 1].abs_min_x,
                evdevs[g.n_evdev - 1].abs_max_x, evdevs[g.n_evdev - 1].abs_min_y,
                evdevs[g.n_evdev - 1].abs_max_y);
    }
}

static void handle_key(int code, int down);

static void read_evdev(int idx)
{
    struct input_event ev;
    int fd = evdevs[idx].fd;

    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_SYN || ev.type == EV_MSC)
            continue;
        if (evdevs[idx].logged < 40) {
            evdevs[idx].logged++;
            log_msg("evdev %d type %d code %d value %d", idx, ev.type, ev.code, ev.value);
        }
        if (ev.type == EV_REL && ev.code == REL_WHEEL && ev.value) {
            int b = ev.value > 0 ? 4 : 5, n = ev.value > 0 ? ev.value : -ev.value;
            while (n-- > 0) {
                x_button(b, 1);
                x_button(b, 0);
            }
            continue;
        }
        if (g.wl_input)
            continue;
        if (ev.type == EV_ABS && (ev.code == ABS_X || ev.code == ABS_Y)) {
            int x = g.px, y = g.py;
            if (ev.code == ABS_X) {
                x = (ev.value - evdevs[idx].abs_min_x) * g.width /
                    (evdevs[idx].abs_max_x - evdevs[idx].abs_min_x);
                g.remote_x = (ev.value - evdevs[idx].abs_min_x) * 1920 /
                             (evdevs[idx].abs_max_x - evdevs[idx].abs_min_x);
            } else {
                y = (ev.value - evdevs[idx].abs_min_y) * g.height /
                    (evdevs[idx].abs_max_y - evdevs[idx].abs_min_y);
                g.remote_y = (ev.value - evdevs[idx].abs_min_y) * 1080 /
                             (evdevs[idx].abs_max_y - evdevs[idx].abs_min_y);
            }
            /* Over the keyboard the pointer belongs to it: leave Wine's
             * pointer (and its text field) where it was. */
            if (!on_keyboard())
                x_motion(x, y);
        } else if (ev.type == EV_REL && (ev.code == REL_X || ev.code == REL_Y)) {
            /* A USB mouse. */
            x_motion(g.px + (ev.code == REL_X ? ev.value : 0), g.py + (ev.code == REL_Y ? ev.value : 0));
        } else if (ev.type == EV_KEY && ev.value != 2) {
            /* The keyboard's own buttons are pressed with OK/click. */
            if (on_keyboard() && (ev.code == BTN_LEFT || ev.code == KEY_OK || ev.code == KEY_ENTER))
                continue;
            handle_key(ev.code, ev.value == 1);
        }
    }
}

/* ------------------------------------------------------ Wayland window */

static void shell_ping(void *data, struct wl_shell_surface *s, uint32_t serial)
{
    (void)data;
    wl_shell_surface_pong(s, serial);
}

static void shell_configure(void *data, struct wl_shell_surface *s, uint32_t edges, int32_t w,
                            int32_t h)
{
    (void)data;
    (void)s;
    (void)edges;
    log_msg("configure %dx%d", w, h);
}

static void shell_popup_done(void *data, struct wl_shell_surface *s)
{
    (void)data;
    (void)s;
}

static const struct wl_shell_surface_listener shell_listener = {
    shell_ping, shell_configure, shell_popup_done,
};

static void webos_state_changed(void *data, struct wl_webos_shell_surface *s, uint32_t state)
{
    (void)data;
    (void)s;
    log_msg("webos state %u", state);
    /* Back in front: repaint everything. */
    g.dirty = 1;
}

static void webos_position_changed(void *data, struct wl_webos_shell_surface *s, int32_t x,
                                   int32_t y)
{
    (void)data;
    (void)s;
    (void)x;
    (void)y;
}

static void webos_close(void *data, struct wl_webos_shell_surface *s)
{
    (void)data;
    (void)s;
    log_msg("webos close");
    g.running = 0;
}

static void webos_exposed(void *data, struct wl_webos_shell_surface *s, struct wl_array *r)
{
    (void)data;
    (void)s;
    (void)r;
    g.dirty = 1;
}

static void webos_state_about_to_change(void *data, struct wl_webos_shell_surface *s,
                                        uint32_t state)
{
    (void)data;
    (void)s;
    (void)state;
}

static void webos_addon_status(void *data, struct wl_webos_shell_surface *s, uint32_t status)
{
    (void)data;
    (void)s;
    (void)status;
}

static const struct wl_webos_shell_surface_listener webos_listener = {
    webos_state_changed, webos_position_changed,      webos_close,
    webos_exposed,       webos_state_about_to_change, webos_addon_status,
};

static void registry_global(void *data, struct wl_registry *r, uint32_t id, const char *iface,
                            uint32_t version)
{
    (void)data;
    if (!strcmp(iface, "wl_compositor"))
        g.compositor = wl_registry_bind(r, id, &wl_compositor_interface, version < 3 ? version : 3);
    else if (!strcmp(iface, "wl_shm"))
        g.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_shell"))
        g.shell = wl_registry_bind(r, id, &wl_shell_interface, 1);
    else if (!strcmp(iface, "wl_webos_shell"))
        g.webos_shell = wl_registry_bind(r, id, &wl_webos_shell_interface, 1);
    else if (!strcmp(iface, "text_model_factory"))
        g.text_factory = wl_registry_bind(r, id, &text_model_factory_interface, 1);
    else if (!strcmp(iface, "wl_seat") && !g.seat) {
        g.seat = wl_registry_bind(r, id, &wl_seat_interface, version < 4 ? version : 4);
        wl_seat_add_listener(g.seat, &seat_listener, NULL);
    }
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t id)
{
    (void)data;
    (void)r;
    (void)id;
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static const char *app_id(void)
{
    const char *id = getenv("APPID");

    return id && id[0] && strcmp(id, "com.palm.devmode.openssh") ? id : APP_ID;
}

static void create_window(void)
{
    struct wl_shm_pool *pool;
    struct wl_region *region;

    g.display = wl_display_connect(NULL);
    if (!g.display)
        die("cannot connect to the TV compositor (wayland-0)");
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, NULL);
    wl_display_roundtrip(g.display);
    wl_display_roundtrip(g.display);
    if (!g.compositor || !g.shm || !g.shell || !g.webos_shell)
        die("compositor lacks wl_compositor, wl_shm, wl_shell or wl_webos_shell");

    g.surface = wl_compositor_create_surface(g.compositor);
    g.shell_surface = wl_shell_get_shell_surface(g.shell, g.surface);
    wl_shell_surface_add_listener(g.shell_surface, &shell_listener, NULL);
    wl_shell_surface_set_toplevel(g.shell_surface);
    g.webos_surface = wl_webos_shell_get_shell_surface(g.webos_shell, g.surface);
    wl_webos_shell_surface_add_listener(g.webos_surface, &webos_listener, NULL);
    wl_webos_shell_surface_set_property(g.webos_surface, "appId", app_id());
    wl_webos_shell_surface_set_property(g.webos_surface, "title", "Wine");
    wl_webos_shell_surface_set_property(g.webos_surface, "displayAffinity", "0");
    wl_webos_shell_surface_set_property(g.webos_surface, "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    wl_webos_shell_surface_set_property(g.webos_surface, "_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    wl_webos_shell_surface_set_property(g.webos_surface, "_WEBOS_CURSOR_SLEEP_TIME", "-1");
    /* Back is not in the default key mask; claim it for this window. */
    wl_webos_shell_surface_set_key_mask(g.webos_surface, 0xFFFFFFFAu);
    wl_webos_shell_surface_set_state(g.webos_surface, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);

    region = wl_compositor_create_region(g.compositor);
    wl_region_add(region, 0, 0, g.width, g.height);
    wl_surface_set_opaque_region(g.surface, region);
    wl_region_destroy(region);

    /* The buffer is Xvfb's own screen file: no copy on our side. */
    pool = wl_shm_create_pool(g.shm, g.fb_fd, (int32_t)g.fb_size);
    g.buffer = wl_shm_pool_create_buffer(pool, (int32_t)g.fb_offset, g.width, g.height,
                                         (int32_t)g.fb_stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    g.dirty = 1;
    wl_display_roundtrip(g.display);
    log_msg("window created");
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { frame_done };

static long ms_since(const struct timespec *t)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

static void present(void)
{
    /* LSM does not always answer a frame callback (for example while the
     * window is not in front). Waiting on one forever froze the picture
     * after the first change, so an unanswered one is given up after 50 ms. */
    if (g.frame) {
        wl_callback_destroy(g.frame);
        g.frame = NULL;
        if (g.stale_frames++ < 5)
            log_msg("frame callback not answered, presenting anyway");
    }
    g.presents++;
    clock_gettime(CLOCK_MONOTONIC, &g.frame_at);
    g.settle = g.dirty;
    g.dirty = 0;
    wl_surface_attach(g.surface, g.buffer, 0, 0);
    wl_surface_damage(g.surface, 0, 0, g.width, g.height);
    g.frame = wl_surface_frame(g.surface);
    wl_callback_add_listener(g.frame, &frame_listener, NULL);
    wl_surface_commit(g.surface);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)data;
    (void)time;
    wl_callback_destroy(cb);
    g.frame = NULL;
}

/* Wine shows a text field's I-beam as X's narrow "xterm" cursor (9x16 with
 * its hotspot in the middle); the arrow and other cursors are wider or
 * point from a corner. This is the one sign of a text field that Qt
 * programs (VLC) give as well as native ones. */
static void update_cursor_kind(void)
{
    XFixesCursorImage *c = XFixesGetCursorImage(g.x);
    int ibeam = 0;

    if (c) {
        ibeam = c->width <= 12 && c->height >= 14 && c->height <= 24 &&
                c->xhot >= c->width / 2 - 1 && c->xhot <= c->width / 2 + 1 &&
                c->yhot >= c->height / 2 - 2 && c->yhot <= c->height / 2 + 2;
        XFree(c);
    }
    g.cursor_ibeam = ibeam;
}

static void drain_x(void)
{
    while (XPending(g.x)) {
        XEvent ev;

        XNextEvent(g.x, &ev);
        if (ev.type == g.damage_event + XDamageNotify) {
            XDamageSubtract(g.x, g.damage, None, None);
            g.dirty = 1;
        } else if (ev.type == g.fixes_event + XFixesCursorNotify) {
            update_cursor_kind();
        }
    }
}

/* ----------------------------------------------------------------- main */

static int reap_children(void)
{
    int status;
    pid_t pid;
    int wine_done = 0;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == g.install_pid) {
            log_msg("install finished, status %#x", status);
            g.install_pid = 0;
        } else if (pid == g.wine_pid) {
            log_msg("wine exited, status %#x", status);
            wine_done = 1;
            g.wine_pid = 0;
        } else if (pid == g.xvfb_pid) {
            log_msg("Xvfb exited, status %#x", status);
            g.xvfb_pid = 0;
            g.running = 0;
        }
    }
    return wine_done;
}

int main(int argc, char **argv)
{
    char exe[PATH_MAX], program[1024] = "";
    ssize_t n;
    char *slash;
    struct sigaction sa;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0)
        return 1;
    exe[n] = '\0';
    snprintf(g.dir, sizeof g.dir, "%s", exe);
    slash = strrchr(g.dir, '/');
    if (!slash)
        return 1;
    *slash = '\0';
    /* The launcher lives in bin/; the app directory is its parent. */
    slash = strrchr(g.dir, '/');
    if (slash && !strcmp(slash, "/bin"))
        *slash = '\0';

    open_log();
    log_msg("wine-tv start, app %s, uid %d, args %d%s%s", g.dir, (int)getuid(), argc,
            argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
    snprintf(g.home, sizeof g.home, "%s/home", g.dir);
    mkdir_p(g.home);
    read_conf();
    setup_env();

    if (argc > 1 && argv[1][0] == '{')
        launch_exe(argv[1], program, sizeof program);
    else if (argc > 1)
        snprintf(program, sizeof program, "%s", argv[1]);
    else
        snprintf(program, sizeof program, "%s", conf_program);

    /* One process group for everything started here, so all of it can be
     * stopped together. */
    setpgid(0, 0);
    if (pipe2(g.sigpipe, O_CLOEXEC | O_NONBLOCK) < 0)
        die("pipe");
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g.running = 1;

    start_xvfb();
    connect_x();
    map_framebuffer();
    create_window();
    open_evdev();
    kill_strays();
    snprintf(g.program, sizeof g.program, "%s", program);
    install_prefix();
    map_drives();
    start_wine(program);

    while (g.running) {
        struct pollfd pfd[3 + 16];
        int nfd = 0, i;

        drain_x();
        if (g.dirty && (!g.frame || ms_since(&g.frame_at) > 50))
            present();
        /* The compositor reads the shared buffer when it draws, not when we
         * commit. If the last change landed after that read, the TV kept an
         * old picture (a black area where a closed window had been). So once
         * changes stop, show the whole buffer once more. */
        else if (g.settle && !g.dirty && ms_since(&g.frame_at) > 300) {
            g.settle = 0;
            g.dirty = 0;
            present();
            g.settle = 0;
        }
        if (wl_display_get_error(g.display)) {
            log_msg("compositor error %d, exiting", wl_display_get_error(g.display));
            break;
        }
        while (wl_display_prepare_read(g.display) != 0)
            if (wl_display_dispatch_pending(g.display) < 0)
                break;
        if (wl_display_get_error(g.display))
            continue;
        if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g.display);
            log_msg("compositor connection lost");
            break;
        }
        pfd[nfd].fd = wl_display_get_fd(g.display);
        pfd[nfd++].events = POLLIN;
        pfd[nfd].fd = ConnectionNumber(g.x);
        pfd[nfd++].events = POLLIN;
        pfd[nfd].fd = g.sigpipe[0];
        pfd[nfd++].events = POLLIN;
        for (i = 0; i < g.n_evdev; i++) {
            pfd[nfd].fd = g.evdev[i];
            pfd[nfd++].events = POLLIN;
        }
        if (poll(pfd, nfd, g.dirty ? 20 : 300) < 0 && errno != EINTR) {
            wl_display_cancel_read(g.display);
            break;
        }
        if (pfd[0].revents & POLLIN) {
            if (wl_display_read_events(g.display) < 0) {
                log_msg("compositor read failed");
                break;
            }
        } else {
            wl_display_cancel_read(g.display);
        }
        if (pfd[0].revents & (POLLERR | POLLHUP))
            break;
        wl_display_dispatch_pending(g.display);
        if (pfd[2].revents & POLLIN) {
            char buf[32];
            while (read(g.sigpipe[0], buf, sizeof buf) > 0) {
            }
            if (reap_children()) {
                /* The desktop closed. From "Exit desktop" (Start menu), leave
                 * the app, as the TV's Home or Exit button also does. Within
                 * 30 s of starting it is Wine instead: after an app update
                 * the prefix is updated and the first desktop exits. */
                static int restarts;

                if (ms_since(&g.wine_started) < 30000 && restarts < 3) {
                    restarts++;
                    log_msg("Wine desktop exited %ld ms after starting, opening it again",
                            ms_since(&g.wine_started));
                    start_wine(g.program);
                    continue;
                }
                log_msg("Wine desktop closed, exiting");
                break;
            }
        }
        for (i = 0; i < g.n_evdev; i++)
            if (pfd[3 + i].revents & POLLIN)
                read_evdev(i);
        run_install_request();
        check_text_focus();
    }

    log_msg("stopping");
    hide_keyboard();
    if (g.display)
        wl_display_flush(g.display);
    stop_all();
    return 0;
}
