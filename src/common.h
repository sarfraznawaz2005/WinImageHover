/*
 * ImageHover - shared declarations.
 *
 * Modules:
 *   main.c      tray icon, hover state machine, message loop
 *   locate.c    "which file is under the cursor?" (UI Automation + shell)
 *   loader.c    background decoding (WIC + Windows thumbnails), animation frames, cache
 *   render.c    the click-through preview window
 *   settings.c  settings (HKCU registry), settings window, start with Windows
 */
#ifndef IMAGEHOVER_COMMON_H
#define IMAGEHOVER_COMMON_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <knownfolders.h>
#include <wincodec.h>
#include <uiautomation.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

#include "resource.h"

#define PATHBUF           1024
#define WM_TRAY           (WM_APP + 1)
#define WM_PREVIEW_READY  (WM_APP + 2)   /* wParam = request id, lParam = Preview* or NULL */

extern HINSTANCE g_inst;
extern HWND      g_msgWnd;

/* ---- Developer log (debug builds only: build.bat debug) ---- */
extern BOOL g_log;                 /* can be muted for quiet re-checks */
#ifdef IH_DEBUG
void LogInit(void);
void dbg(const WCHAR *fmt, ...);
#else
int dbg_unused(const WCHAR *fmt, ...);   /* never defined: only used inside sizeof */
#define LogInit() ((void)0)
#define dbg(...) ((void)sizeof(dbg_unused(__VA_ARGS__)))
#endif

/* ---- Settings (settings.c) ---- */
typedef struct Settings {
    DWORD hoverMs;      /* rest time before a preview, 100..3000 */
    DWORD sizePct;      /* max preview size, % of the monitor work area */
    BOOL  inExplorer;
    BOOL  inDialogs;
    BOOL  onDesktop;
    BOOL  shiftOnly;    /* only while Shift is held */
    BOOL  animate;      /* play animated GIF / WebP */
    BOOL  thumbnails;   /* videos, PDFs, RAW... through Windows thumbnails */
    BOOL  details;      /* file size, date, camera under the image */
} Settings;

extern Settings g_set;
void SettingsLoad(void);
void SettingsSave(void);
void SettingsOpen(void);            /* show or focus the settings window */
HWND SettingsWindow(void);          /* NULL when closed (for IsDialogMessage) */
BOOL AutostartEnabled(void);
BOOL SetAutostart(BOOL on);
void RefreshAutostartPath(void);
void OnSettingsChanged(BOOL sizeChanged);   /* main.c */

/* ---- File types and paths (locate.c) ---- */
BOOL IsImageExt(const WCHAR *path);
BOOL IsThumbExt(const WCHAR *path);
BOOL IsPreviewable(const WCHAR *path);
/* pdf.c: page 1 of a PDF as PNG, via the Windows built-in PDF reader */
BOOL PdfRenderFirstPage(const WCHAR *path, int maxW, int maxH, IStream **png);      /* respects g_set.thumbnails */
void JoinPath(WCHAR *out, size_t cch, const WCHAR *dir, const WCHAR *name);

/* ---- Locate the hovered file (locate.c) ---- */
BOOL LocateInit(void);
void LocateShutdown(void);
BOOL ResolveHoveredFile(POINT pt, WCHAR *out, size_t cch, RECT *itemRect, HWND *rootOut);

/* ---- Decoded preview (loader.c) ---- */
typedef struct Preview {
    LONG       refs;
    WCHAR      path[PATHBUF];
    FILETIME   mtime;           /* file identity for the cache */
    ULONGLONG  fileSize;
    int        maxW, maxH;      /* the box it was scaled for */

    UINT       imgW, imgH;      /* real image size as seen by the user; 0 = unknown */
    BOOL       fromThumbnail;
    BOOL       hasTaken;        /* EXIF date taken */
    SYSTEMTIME taken;
    WCHAR      camera[128];

    int        w, h;            /* scaled frame size */
    int        frameCount;
    BYTE     **frames;          /* premultiplied BGRA, w*h*4 each */
    UINT      *delays;          /* ms per frame */
    SIZE_T     bytes;           /* memory used by frames */
} Preview;

void     PreviewAddRef(Preview *p);
void     PreviewRelease(Preview *p);
BOOL     LoaderStart(HWND notify);
void     LoaderStop(void);
void     LoaderRequest(const WCHAR *path, int maxW, int maxH, UINT id);
Preview *CacheFind(const WCHAR *path, int maxW, int maxH);  /* +1 ref, or NULL */
void     CacheAdd(Preview *p);
void     CacheClear(void);

/* ---- Preview window (render.c) ---- */
BOOL PreviewWindowCreate(void);
void PreviewWindowDestroy(void);
void PreviewBoxFor(POINT pt, int *maxW, int *maxH);  /* max image size near pt */
void PreviewShow(Preview *p, POINT pt);
void PreviewHide(void);
BOOL PreviewVisible(void);
const WCHAR *PreviewPath(void);
HWND PreviewHwnd(void);

#endif
