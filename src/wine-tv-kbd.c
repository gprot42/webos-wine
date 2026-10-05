/* wine-tv-kbd: tells wine-tv when a Windows text field wants typing, so the
 * webOS on-screen keyboard can open by itself.
 *
 * The X server sees the whole Wine desktop as one window, so only Windows
 * knows which control has the focus. A control that takes typing shows a
 * caret; this polls the foreground thread's caret (GetGUIThreadInfo) and
 * writes "1" or "0" to Z:\tmp\wine-tv\kbd whenever that changes. wine-tv
 * reads the file and shows or hides the keyboard.
 *
 * It also repairs the desktop's painting. When a window vanished (a program
 * exited or crashed) the desktop sometimes left its area black, and desktop
 * icons could end up drawn over open windows: Wine's X11 driver paints the
 * desktop through the windows above it, so whichever repaints last wins.
 * So whenever the set of visible windows or the desktop's icons change, once
 * that has settled, the desktop is repainted first and then every window.
 *
 * It registers as a Wine system process, like services.exe: otherwise,
 * having no window to receive the end-session messages, it kept the desktop
 * alive and "Exit desktop" did nothing. Wine signals the event it gets back
 * when the last ordinary program has gone, and it exits then.
 *
 * Built for aarch64 Windows (llvm-mingw), so it runs natively in Wine;
 * wine-tv starts it with the desktop, and it has no window.
 */

#include <windows.h>
#include <shlobj.h>

/* Wine-specific: NtSetInformationProcess class ProcessWineMakeProcessSystem. */
#define ProcessWineMakeProcessSystem 1000
LONG WINAPI NtSetInformationProcess(HANDLE process, ULONG info_class, void *info, ULONG size);

static const wchar_t state_file[] = L"Z:\\tmp\\wine-tv\\kbd";

static void write_state(int want)
{
    HANDLE h = CreateFileW(state_file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written;

    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, want ? "1\n" : "0\n", 2, &written, NULL);
    CloseHandle(h);
}

/* A focused control with a visible caret takes typing. Edit controls of
 * combo boxes and rich edits have one; buttons, lists and menus do not. */
static int wants_keyboard(void)
{
    GUITHREADINFO info = { sizeof(info) };
    HWND fg = GetForegroundWindow();
    DWORD tid;

    if (!fg)
        return 0;
    tid = GetWindowThreadProcessId(fg, NULL);
    if (!GetGUIThreadInfo(tid, &info))
        return 0;
    if (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        return 0;
    return info.hwndCaret != NULL && info.hwndFocus != NULL;
}

#define MAX_TRACKED 256

struct window_set {
    HWND hwnd[MAX_TRACKED];
    int count;
};

static BOOL CALLBACK add_visible(HWND hwnd, LPARAM param)
{
    struct window_set *set = (struct window_set *)param;

    if (IsWindowVisible(hwnd) && set->count < MAX_TRACKED)
        set->hwnd[set->count++] = hwnd;
    return TRUE;
}

static int same_windows(const struct window_set *a, const struct window_set *b)
{
    int i, j;

    if (a->count != b->count)
        return 0;
    for (i = 0; i < a->count; i++) {
        for (j = 0; j < b->count && b->hwnd[j] != a->hwnd[i]; j++)
            ;
        if (j == b->count)
            return 0;
    }
    return 1;
}

static BOOL CALLBACK redraw_window(HWND hwnd, LPARAM param)
{
    (void)param;
    if (IsWindowVisible(hwnd))
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return TRUE;
}

/* Desktop first, windows after it; the second window pass catches programs
 * that were slow to paint. */
static void repaint_all(void)
{
    RedrawWindow(GetDesktopWindow(), NULL, NULL, RDW_INVALIDATE | RDW_ERASE);
    Sleep(500);
    EnumWindows(redraw_window, 0);
    Sleep(1000);
    EnumWindows(redraw_window, 0);
}

/* Change notifications for the two folders whose shortcuts the desktop shows. */
static int watch_desktops(HANDLE *watch)
{
    static const int folders[] = { CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY };
    wchar_t path[MAX_PATH];
    int i, n = 0;

    for (i = 0; i < 2; i++) {
        if (!SHGetSpecialFolderPathW(NULL, path, folders[i], FALSE))
            continue;
        watch[n] = FindFirstChangeNotificationW(path, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
        if (watch[n] != INVALID_HANDLE_VALUE)
            n++;
    }
    return n;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    int state = -1;
    HANDLE single;
    static struct window_set last, now;
    int settle = 0;             /* polls left before redrawing */
    HANDLE waits[3];             /* end of session, then desktop folder watches */
    HANDLE *watch = waits + 1;
    HANDLE session_end = NULL;
    int nwatch;

    (void)inst;
    (void)prev;
    (void)cmdline;
    (void)show;
    /* One per desktop session. */
    single = CreateMutexW(NULL, TRUE, L"wine-tv-kbd");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    NtSetInformationProcess(GetCurrentProcess(), ProcessWineMakeProcessSystem,
                            &session_end, sizeof(HANDLE *));
    waits[0] = session_end;
    nwatch = watch_desktops(watch);
    for (;;) {
        int want = wants_keyboard();
        DWORD w;

        if (want != state) {
            state = want;
            write_state(want);
        }

        now.count = 0;
        EnumWindows(add_visible, (LPARAM)&now);
        if (!same_windows(&now, &last)) {
            last = now;
            settle = 2;
        } else if (settle && --settle == 0) {
            repaint_all();
        }
        /* Wait 300 ms, or less if a desktop icon was added or removed or
         * the session ended. */
        if (session_end)
            w = WaitForMultipleObjects(1 + nwatch, waits, FALSE, 300);
        else
            w = nwatch ? WaitForMultipleObjects(nwatch, watch, FALSE, 300) + 1 : (Sleep(300), WAIT_TIMEOUT);
        if (session_end && w == WAIT_OBJECT_0)
            return 0;
        if (w > WAIT_OBJECT_0 && w <= WAIT_OBJECT_0 + (DWORD)nwatch) {
            FindNextChangeNotification(watch[w - WAIT_OBJECT_0 - 1]);
            settle = 2;
        }
    }
}
