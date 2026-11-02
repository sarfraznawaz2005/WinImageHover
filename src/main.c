/*
 * ImageHover - show an image preview when the mouse rests on an image file
 * in File Explorer, Open/Save dialogs, and on the Desktop.
 *
 * main.c: startup, tray icon, and the hover state machine.
 *
 *   idle --(cursor rests hoverMs)--> locate file --(in cache)--> shown
 *                                         \--(not cached)--> loading --(WM_PREVIEW_READY)--> shown
 *   loading/shown --(cursor leaves item, click, Shift released...)--> idle
 */
#include "common.h"

#define TICK_MS         50      /* cursor polling interval              */
#define RECHECK_TICKS   6       /* while shown, re-verify every 300 ms  */
#define MOVE_TOLERANCE  3       /* px of jitter that still counts "still" */
#define ID_TIMER        1

enum { IDM_SETTINGS = 200, IDM_PAUSE, IDM_AUTOSTART, IDM_EXIT };

HINSTANCE g_inst;
HWND      g_msgWnd;
BOOL      g_log;

static UINT  s_msgTaskbarCreated;
static HICON s_trayIcon;
static BOOL  s_paused;

/* hover state */
static POINT s_lastPt;
static DWORD s_stillSince;
static BOOL  s_queried;
static BOOL  s_prevShift;
static BOOL  s_loading;
static UINT  s_reqId;
static RECT  s_itemRect;
static HWND  s_itemRoot;
static int   s_recheck;

/* ======================================================================= */
/* Developer log (debug builds only)                                        */
/* ======================================================================= */
#ifdef IH_DEBUG
static WCHAR            s_logPath[MAX_PATH];
static CRITICAL_SECTION s_logCs;

void LogInit(void)
{
    InitializeCriticalSection(&s_logCs);
    GetModuleFileNameW(NULL, s_logPath, MAX_PATH);
    PathRemoveFileSpecW(s_logPath);
    PathAppendW(s_logPath, L"ImageHover.log");
    g_log = TRUE;
}

void dbg(const WCHAR *fmt, ...)
{
    if (!g_log) return;
    WCHAR buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap); buf[2047] = 0;
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    EnterCriticalSection(&s_logCs);
    FILE *f = _wfopen(s_logPath, L"a, ccs=UTF-8");
    if (f) {
        fwprintf(f, L"%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fclose(f);
    }
    LeaveCriticalSection(&s_logCs);
}
#endif

/* ======================================================================= */
/* Hover state machine                                                      */
/* ======================================================================= */

static void Cancel(const WCHAR *why)
{
    if (PreviewVisible()) dbg(L"HIDE: %s", why);
    else if (s_loading) dbg(L"CANCEL load: %s", why);
    PreviewHide();
    if (s_loading) {
        s_loading = FALSE;
        s_reqId++;              /* the late result goes to the cache only */
    }
    (void)why;
}

static void StartPreview(POINT pt)
{
    dbg(L"REST at %ld,%ld -> checking what is under the cursor", pt.x, pt.y);
    WCHAR path[PATHBUF];
    RECT r;
    HWND root;
    if (!ResolveHoveredFile(pt, path, PATHBUF, &r, &root)) return;

    int maxW, maxH;
    PreviewBoxFor(pt, &maxW, &maxH);
    s_itemRect = r;
    s_itemRoot = root;
    s_recheck = 0;

    Preview *p = CacheFind(path, maxW, maxH);
    if (p) {
        dbg(L"SHOW (from cache)");
        PreviewShow(p, pt);
        PreviewRelease(p);
        return;
    }
    s_loading = TRUE;
    LoaderRequest(path, maxW, maxH, ++s_reqId);
    dbg(L"  loading in background (request %u)", s_reqId);
}

static void OnPreviewReady(UINT id, Preview *p)
{
    if (p) CacheAdd(p);
    if (s_loading && id == s_reqId) {
        s_loading = FALSE;
        if (p) {
            POINT pt;
            GetCursorPos(&pt);
            dbg(L"SHOW (loaded)");
            PreviewShow(p, pt);
        }
    }
    PreviewRelease(p);
}

static void OnTick(void)
{
    if (s_paused) return;
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    DWORD now = GetTickCount();
    BOOL buttons = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                     GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
    BOOL shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    /* Shift-only mode: pressing Shift on a still cursor checks again. */
    if (g_set.shiftOnly && shift && !s_prevShift) s_queried = FALSE;
    s_prevShift = shift;

    if (PreviewVisible() || s_loading) {
        HWND root = GetAncestor(WindowFromPoint(pt), GA_ROOT);
        if (buttons)                            Cancel(L"mouse button pressed");
        else if (!PtInRect(&s_itemRect, pt))    Cancel(L"cursor left the item");
        else if (root != s_itemRoot)            Cancel(L"another window is under the cursor");
        else if (g_set.shiftOnly && !shift)     Cancel(L"Shift released");
        else if (PreviewVisible() && ++s_recheck >= RECHECK_TICKS) {
            /* The list may have scrolled under a still cursor. */
            s_recheck = 0;
            BOOL was = g_log;
            g_log = FALSE;                      /* keep the log short */
            WCHAR path[PATHBUF]; RECT r; HWND rt;
            BOOL same = ResolveHoveredFile(pt, path, PATHBUF, &r, &rt) && _wcsicmp(path, PreviewPath()) == 0;
            g_log = was;
            if (same) s_itemRect = r;
            else Cancel(L"item under cursor changed (scroll?)");
        }
    }

    if (abs(pt.x - s_lastPt.x) > MOVE_TOLERANCE || abs(pt.y - s_lastPt.y) > MOVE_TOLERANCE) {
        s_lastPt = pt;
        s_stillSince = now;
        s_queried = FALSE;
        return;
    }
    if (buttons) { s_stillSince = now; return; }

    BOOL allowed = !g_set.shiftOnly || shift;
    if (!PreviewVisible() && !s_loading && !s_queried && allowed && now - s_stillSince >= g_set.hoverMs) {
        s_queried = TRUE;
        StartPreview(pt);
    }
}

void OnSettingsChanged(BOOL sizeChanged)
{
    (void)sizeChanged;
    Cancel(L"settings changed");
    CacheClear();               /* size, animation or details may differ now */
    s_queried = FALSE;
}

/* ======================================================================= */
/* Tray icon                                                                */
/* ======================================================================= */

static void UpdateTrayIcon(void)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = s_trayIcon ? s_trayIcon : LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(nid.szTip, s_paused ? L"ImageHover (paused)" : L"ImageHover");
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
        Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(void)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowTrayMenu(void)
{
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING, IDM_PAUSE, s_paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);
    SetForegroundWindow(g_msgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_msgWnd, NULL);
    PostMessageW(g_msgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_TIMER) OnTick();
        return 0;
    case WM_PREVIEW_READY:
        OnPreviewReady((UINT)wp, (Preview *)lp);
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) SettingsOpen();
        else if (lp == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS:
            SettingsOpen();
            break;
        case IDM_PAUSE:
            s_paused = !s_paused;
            Cancel(L"paused");
            UpdateTrayIcon();
            break;
        case IDM_AUTOSTART:
            if (!SetAutostart(!AutostartEnabled()))
                MessageBoxW(NULL, L"Could not change the \"Start with Windows\" setting.",
                            L"ImageHover", MB_ICONWARNING);
            break;
        case IDM_EXIT:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(h, ID_TIMER);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == s_msgTaskbarCreated && msg) { UpdateTrayIcon(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ======================================================================= */
/* Startup                                                                  */
/* ======================================================================= */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\ImageHover.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    LogInit();
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    SettingsLoad();

    if (!LocateInit()) {
        MessageBoxW(NULL, L"Could not start UI Automation.", L"ImageHover", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ImageHoverMain";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    g_msgWnd = CreateWindowExW(0, wc.lpszClassName, L"ImageHover", WS_OVERLAPPED,
                               0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_msgWnd || !PreviewWindowCreate() || !LoaderStart(g_msgWnd)) {
        MessageBoxW(NULL, L"Could not start ImageHover.", L"ImageHover", MB_ICONERROR);
        return 1;
    }

    if (FAILED(LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &s_trayIcon)))
        s_trayIcon = NULL;
    s_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    UpdateTrayIcon();
    RefreshAutostartPath();
    SetTimer(g_msgWnd, ID_TIMER, TICK_MS, NULL);
    dbg(L"=== ImageHover started ===");

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        HWND dlg = SettingsWindow();
        if (dlg && IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    LoaderStop();
    PreviewWindowDestroy();
    CacheClear();
    LocateShutdown();
    if (s_trayIcon) DestroyIcon(s_trayIcon);
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
