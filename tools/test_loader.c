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

    CacheClear();
    LoaderStop();
    PreviewWindowDestroy();
    CoUninitialize();
    wprintf(L"\n%s (%d failures)\n", s_fail ? L"FAILED" : L"ALL CHECKS PASSED", s_fail);
    return s_fail ? 1 : 0;
}
