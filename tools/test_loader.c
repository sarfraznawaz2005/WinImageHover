/*
 * Test harness for loader.c + render.c (no mouse, nothing shown on screen).
 * Loads each file given on the command line on the background thread, prints
 * what was decoded, checks the cache, and saves the composed preview as BMP.
 *
 * Build + run: tools\test_loader.bat
 */
#include "../src/common.h"

HINSTANCE g_inst;
HWND      g_msgWnd;
BOOL      g_log;
void OnSettingsChanged(BOOL sizeChanged) { (void)sizeChanged; }
BOOL PreviewDump(int f, const WCHAR *bmpPath);
void PreviewWindowRect(RECT *r);

static int s_fail;
#define CHECK(c, msg) do { if (c) wprintf(L"   ok   %s\n", msg); else { wprintf(L"   FAIL %s\n", msg); s_fail++; } } while (0)

static Preview *WaitResult(UINT id)
{
    DWORD t0 = GetTickCount();
    MSG m;
    while (GetTickCount() - t0 < 15000) {
        while (PeekMessageW(&m, g_msgWnd, 0, 0, PM_REMOVE)) {
            if (m.message == WM_PREVIEW_READY && (UINT)m.wParam == id) return (Preview *)m.lParam;
            DispatchMessageW(&m);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
    wprintf(L"   (timeout)\n");
    return NULL;
}

int wmain(int argc, WCHAR **argv)
{
    g_inst = GetModuleHandleW(NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SettingsLoad();
    g_set.animate = TRUE; g_set.thumbnails = TRUE; g_set.details = TRUE; g_set.sizePct = 45;

    g_msgWnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);
    if (!PreviewWindowCreate() || !LoaderStart(g_msgWnd)) { wprintf(L"start failed\n"); return 1; }

    POINT pt = { 200, 200 };
    int maxW, maxH;
    PreviewBoxFor(pt, &maxW, &maxH);
    wprintf(L"box %d x %d\n", maxW, maxH);

    for (int i = 1; i < argc; i++) {
        WCHAR full[PATHBUF];
        GetFullPathNameW(argv[i], PATHBUF, full, NULL);
        wprintf(L"\n%s\n", PathFindFileNameW(full));
        DWORD t0 = GetTickCount();
        LoaderRequest(full, maxW, maxH, (UINT)i);
        Preview *p = WaitResult((UINT)i);
        if (!p) { wprintf(L"   no preview (%lu ms)\n", GetTickCount() - t0); continue; }
        wprintf(L"   %lu ms  frames=%d  scaled=%dx%d  image=%ux%u  thumb=%d  camera='%s'  taken=%d %04u-%02u-%02u %02u:%02u\n",
                GetTickCount() - t0, p->frameCount, p->w, p->h, p->imgW, p->imgH, p->fromThumbnail,
                p->camera, p->hasTaken, p->taken.wYear, p->taken.wMonth, p->taken.wDay,
                p->taken.wHour, p->taken.wMinute);
        if (p->frameCount > 1) {
            wprintf(L"   delays:");
            for (int k = 0; k < p->frameCount && k < 6; k++) wprintf(L" %u", p->delays[k]);
            wprintf(L"\n");
        }

        CacheAdd(p);
        Preview *c = CacheFind(full, maxW, maxH);
        CHECK(c == p, L"cache returns the same preview");
        PreviewRelease(c);

        PreviewShow(p, pt);
        WCHAR bmp[PATHBUF];
        _snwprintf(bmp, PATHBUF, L"%s\\out_%s.bmp", argv[0][0] ? L"tools\\out" : L".", PathFindFileNameW(full));
        CreateDirectoryW(L"tools\\out", NULL);
        _snwprintf(bmp, PATHBUF, L"tools\\out\\%s.bmp", PathFindFileNameW(full));
        CHECK(PreviewDump(p->frameCount > 1 ? 5 : 0, bmp), L"rendered preview saved");
        PreviewHide();
        PreviewRelease(p);
    }

    /* Max size: a tall image next to a cursor near the top left must use (almost) the
       full screen height, stay on the screen and never cover the cursor. */
    {
        WCHAR full[PATHBUF];
        GetFullPathNameW(L"testdata\\tall.png", PATHBUF, full, NULL);
        POINT cur = { 300, 207 };
        HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        RECT wa = mi.rcWork;
        int large[2] = {0, 0};
        DWORD modes[2] = { 45, SIZE_PCT_MAX };
        for (int m = 0; m < 2; m++) {
            g_set.sizePct = modes[m];
            int bw, bh;
            PreviewBoxFor(cur, &bw, &bh);
            LoaderRequest(full, bw, bh, 900 + m);
            Preview *p = WaitResult(900 + m);
            if (!p) { CHECK(FALSE, L"tall.png loaded"); continue; }
            PreviewShow(p, cur);
            RECT r;
            PreviewWindowRect(&r);
            wprintf(L"\ntall.png size=%lu%%  window %ldx%ld at %ld,%ld  (work area %ldx%ld)\n", modes[m],
                    r.right - r.left, r.bottom - r.top, r.left, r.top,
                    wa.right - wa.left, wa.bottom - wa.top);
            large[m] = r.bottom - r.top;
            CHECK(!PtInRect(&r, cur), L"preview does not cover the cursor");
            CHECK(r.left >= wa.left && r.top >= wa.top && r.right <= wa.right && r.bottom <= wa.bottom,
                  L"preview stays on the screen");
            CreateDirectoryW(L"tools\\out", NULL);
            CHECK(PreviewDump(0, m ? L"tools\\out\\tall_max.bmp" : L"tools\\out\\tall_large.bmp"),
                  L"rendered preview saved");
            PreviewHide();
            PreviewRelease(p);
        }
        CHECK(large[1] > large[0] * 3 / 2, L"Max is much taller than Large");
        CHECK(large[1] >= (wa.bottom - wa.top) * 9 / 10, L"Max uses at least 90% of the screen height");
        g_set.sizePct = 45;
    }

    CacheClear();
    LoaderStop();
    PreviewWindowDestroy();
    CoUninitialize();
    wprintf(L"\n%s (%d failures)\n", s_fail ? L"FAILED" : L"ALL CHECKS PASSED", s_fail);
    return s_fail ? 1 : 0;
}
