/*
 * settings.c - settings stored in HKCU\Software\ImageHover, the settings window,
 * and "Start with Windows" (HKCU Run key, current user only, no admin needed).
 */
#include "common.h"

#define APP_KEY       L"Software\\ImageHover"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define RUN_VALUE     L"ImageHover"

Settings g_set;

static const DWORD  kSizePct[]   = { 25, 35, 45, 60 };
static const WCHAR *kSizeNames[] = { L"Small", L"Medium", L"Large", L"Extra large" };
#define SIZE_COUNT 4

static HWND  s_dlg;
static HICON s_iconBig, s_iconSmall;

/* ======================================================================= */
/* Load / save                                                              */
/* ======================================================================= */

static DWORD ReadDw(HKEY k, const WCHAR *name, DWORD def)
{
    DWORD v = 0, cb = sizeof(v);
    if (k && RegGetValueW(k, NULL, name, RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS) return v;
    return def;
}

static void WriteDw(HKEY k, const WCHAR *name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
}

void SettingsLoad(void)
{
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) k = NULL;
    g_set.hoverMs    = ReadDw(k, L"HoverDelayMs", 400);
    g_set.sizePct    = ReadDw(k, L"SizePercent", 45);
    g_set.inExplorer = ReadDw(k, L"InExplorer", 1) != 0;
    g_set.inDialogs  = ReadDw(k, L"InDialogs", 1) != 0;
    g_set.onDesktop  = ReadDw(k, L"OnDesktop", 1) != 0;
    g_set.shiftOnly  = ReadDw(k, L"ShiftOnly", 0) != 0;
    g_set.animate    = ReadDw(k, L"Animate", 1) != 0;
    g_set.thumbnails = ReadDw(k, L"Thumbnails", 1) != 0;
    g_set.details    = ReadDw(k, L"Details", 1) != 0;
    if (k) RegCloseKey(k);

    if (g_set.hoverMs < 100) g_set.hoverMs = 100;
    if (g_set.hoverMs > 3000) g_set.hoverMs = 3000;
    if (g_set.sizePct < 15) g_set.sizePct = 15;
    if (g_set.sizePct > 80) g_set.sizePct = 80;
}

void SettingsSave(void)
{
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    WriteDw(k, L"HoverDelayMs", g_set.hoverMs);
    WriteDw(k, L"SizePercent", g_set.sizePct);
    WriteDw(k, L"InExplorer", g_set.inExplorer);
    WriteDw(k, L"InDialogs", g_set.inDialogs);
    WriteDw(k, L"OnDesktop", g_set.onDesktop);
    WriteDw(k, L"ShiftOnly", g_set.shiftOnly);
    WriteDw(k, L"Animate", g_set.animate);
    WriteDw(k, L"Thumbnails", g_set.thumbnails);
    WriteDw(k, L"Details", g_set.details);
    RegCloseKey(k);
}

/* ======================================================================= */
/* Start with Windows                                                       */
/* ======================================================================= */

static void AutostartCommand(WCHAR *out, size_t cch)
{
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(out, cch, L"\"%s\"", exe);
    out[cch - 1] = 0;
}

/* TRUE if the Run entry exists and is not disabled in Task Manager. */
BOOL AutostartEnabled(void)
{
    WCHAR v[PATHBUF];
    DWORD cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, v, &cb) != ERROR_SUCCESS)
        return FALSE;
    /* Task Manager > Startup apps stores on/off here. First byte odd (03, 07) = disabled. */
    BYTE b[16];
    cb = sizeof(b);
    if (RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_VALUE, RRF_RT_REG_BINARY, NULL, b, &cb) == ERROR_SUCCESS &&
        cb > 0 && (b[0] & 1))
        return FALSE;
    return TRUE;
}

BOOL SetAutostart(BOOL on)
{
    LSTATUS rc;
    if (on) {
        WCHAR cmd[PATHBUF];
        AutostartCommand(cmd, PATHBUF);
        rc = RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ,
                             cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
    } else {
        rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    /* Clear any "disabled" mark from Task Manager so our choice wins. */
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, RUN_VALUE);
    dbg(L"autostart %s -> rc=%ld", on ? L"on" : L"off", (long)rc);
    if (s_dlg) CheckDlgButton(s_dlg, IDC_AUTOSTART, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    return rc == ERROR_SUCCESS;
}

/* If the exe was moved, point the existing Run entry at the new location. */
void RefreshAutostartPath(void)
{
    WCHAR v[PATHBUF], cmd[PATHBUF];
    DWORD cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, v, &cb) != ERROR_SUCCESS)
        return;
    AutostartCommand(cmd, PATHBUF);
    if (_wcsicmp(v, cmd) != 0)
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, REG_SZ,
                        cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
}

/* ======================================================================= */
/* Settings window                                                          */
/* ======================================================================= */

static void SetCheck(HWND d, int id, BOOL on) { CheckDlgButton(d, id, on ? BST_CHECKED : BST_UNCHECKED); }
static BOOL GetCheck(HWND d, int id)          { return IsDlgButtonChecked(d, id) == BST_CHECKED; }

static void InitDialog(HWND d)
{
    if (SUCCEEDED(LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &s_iconBig)))
        SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)s_iconBig);
    if (SUCCEEDED(LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &s_iconSmall)))
        SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)s_iconSmall);

    SetCheck(d, IDC_EXPLORER, g_set.inExplorer);
    SetCheck(d, IDC_DIALOGS, g_set.inDialogs);
    SetCheck(d, IDC_DESKTOP, g_set.onDesktop);
    SetCheck(d, IDC_SHIFT, g_set.shiftOnly);
    SetCheck(d, IDC_ANIMATE, g_set.animate);
    SetCheck(d, IDC_THUMBS, g_set.thumbnails);
    SetCheck(d, IDC_DETAILS, g_set.details);
    SetCheck(d, IDC_AUTOSTART, AutostartEnabled());

    SendDlgItemMessageW(d, IDC_DELAY_SPIN, UDM_SETRANGE32, 100, 3000);
    SendDlgItemMessageW(d, IDC_DELAY_SPIN, UDM_SETPOS32, 0, (LPARAM)g_set.hoverMs);

    int sel = SIZE_COUNT - 1;
    for (int i = 0; i < SIZE_COUNT; i++) {
        WCHAR s[64];
        _snwprintf(s, 64, L"%s (%lu%% of screen)", kSizeNames[i], kSizePct[i]);
        SendDlgItemMessageW(d, IDC_PREVIEW_SIZE, CB_ADDSTRING, 0, (LPARAM)s);
        if (g_set.sizePct >= kSizePct[i]) sel = i;
    }
    if (g_set.sizePct < kSizePct[0]) sel = 0;
    SendDlgItemMessageW(d, IDC_PREVIEW_SIZE, CB_SETCURSEL, (WPARAM)sel, 0);

    /* Centre on the monitor with the cursor. */
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r; GetWindowRect(d, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(d, NULL, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
}

static BOOL ApplyDialog(HWND d)
{
    BOOL okNum = FALSE;
    UINT delay = GetDlgItemInt(d, IDC_DELAY, &okNum, FALSE);
    if (!okNum || delay < 100 || delay > 3000) {
        MessageBoxW(d, L"Hover delay must be a number from 100 to 3000 milliseconds.",
                    L"ImageHover", MB_ICONWARNING);
        SetFocus(GetDlgItem(d, IDC_DELAY));
        return FALSE;
    }
    int sel = (int)SendDlgItemMessageW(d, IDC_PREVIEW_SIZE, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= SIZE_COUNT) sel = 2;

    Settings old = g_set;
    g_set.hoverMs    = delay;
    g_set.sizePct    = kSizePct[sel];
    g_set.inExplorer = GetCheck(d, IDC_EXPLORER);
    g_set.inDialogs  = GetCheck(d, IDC_DIALOGS);
    g_set.onDesktop  = GetCheck(d, IDC_DESKTOP);
    g_set.shiftOnly  = GetCheck(d, IDC_SHIFT);
    g_set.animate    = GetCheck(d, IDC_ANIMATE);
    g_set.thumbnails = GetCheck(d, IDC_THUMBS);
    g_set.details    = GetCheck(d, IDC_DETAILS);
    SettingsSave();

    BOOL wantAuto = GetCheck(d, IDC_AUTOSTART);
    if (wantAuto != AutostartEnabled() && !SetAutostart(wantAuto))
        MessageBoxW(d, L"Could not change the \"Start with Windows\" setting.", L"ImageHover", MB_ICONWARNING);

    OnSettingsChanged(old.sizePct != g_set.sizePct);
    return TRUE;
}

static INT_PTR CALLBACK SettingsProc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        InitDialog(d);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            if (ApplyDialog(d)) DestroyWindow(d);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(d);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(d);
        return TRUE;
    case WM_DESTROY:
        s_dlg = NULL;
        if (s_iconBig) DestroyIcon(s_iconBig);
        if (s_iconSmall) DestroyIcon(s_iconSmall);
        s_iconBig = s_iconSmall = NULL;
        return TRUE;
    }
    return FALSE;
}

HWND SettingsWindow(void) { return s_dlg; }

void SettingsOpen(void)
{
    if (!s_dlg) {
        s_dlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, SettingsProc, 0);
        if (!s_dlg) return;
    }
    ShowWindow(s_dlg, SW_SHOW);
    if (IsIconic(s_dlg)) ShowWindow(s_dlg, SW_RESTORE);
    SetForegroundWindow(s_dlg);
}
