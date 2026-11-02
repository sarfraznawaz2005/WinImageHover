/*
 * locate.c - find the file under the mouse cursor.
 *
 *   1. UI Automation gives the hovered list item (name, rect, list index).
 *      On Windows 11 Explorer an invisible "PopupHost" can cover the list, so if
 *      ElementFromPoint does not give an item we search down from the window.
 *   2. The real file:
 *        Explorer -> IShellWindows / IShellBrowser / IFolderView. The item's
 *                    AutomationId is its index in the view, so IFolderView::Item
 *                    gives the exact file (no guessing with hidden extensions).
 *        Dialogs  -> address bar text + name match.
 *        Desktop  -> user + public Desktop folders + name match.
 */
#include "common.h"

static const GUID MY_CLSID_CUIAutomation =
    {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const GUID MY_IID_IUIAutomation =
    {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const GUID MY_SID_STopLevelBrowser =
    {0x4c96be40, 0x915c, 0x11cf, {0x99, 0xd3, 0x00, 0xaa, 0x00, 0x4a, 0xe8, 0x37}};

enum { KIND_NONE, KIND_EXPLORER, KIND_DIALOG, KIND_DESKTOP };

static IUIAutomation             *s_uia;
static IUIAutomationTreeWalker   *s_walker;
static IUIAutomationCacheRequest *s_cacheReq;
static IUIAutomationCondition    *s_trueCond;

/* ======================================================================= */
/* File types                                                               */
/* ======================================================================= */

static BOOL ExtIn(const WCHAR *path, const WCHAR *const *exts)
{
    const WCHAR *dot = wcsrchr(path, L'.');
    const WCHAR *slash = wcsrchr(path, L'\\');
    if (!dot || (slash && dot < slash)) return FALSE;
    for (int i = 0; exts[i]; i++)
        if (_wcsicmp(dot, exts[i]) == 0) return TRUE;
    return FALSE;
}

BOOL IsImageExt(const WCHAR *path)
{
    static const WCHAR *const exts[] = {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".bmp", L".dib",
        L".tif", L".tiff", L".ico", L".webp", L".heic", L".heif", L".avif",
        L".jxr", L".wdp", L".dds", L".jxl", NULL
    };
    return ExtIn(path, exts);
}

/* Files we only preview through Windows thumbnails. */
BOOL IsThumbExt(const WCHAR *path)
{
    static const WCHAR *const exts[] = {
        /* video */
        L".mp4", L".m4v", L".mov", L".avi", L".wmv", L".mkv", L".webm", L".mpg", L".mpeg",
        L".3gp", L".ts", L".mts", L".m2ts", L".flv",
        /* documents */
        L".pdf",
        /* camera RAW */
        L".cr2", L".cr3", L".nef", L".nrw", L".arw", L".srw", L".dng", L".orf", L".rw2",
        L".raf", L".pef", L".3fr", L".erf", L".kdc", L".mrw", L".x3f",
        /* design */
        L".psd", L".svg", L".xcf",
        NULL
    };
    return ExtIn(path, exts);
}

BOOL IsPreviewable(const WCHAR *path)
{
    return IsImageExt(path) || (g_set.thumbnails && IsThumbExt(path));
}

void JoinPath(WCHAR *out, size_t cch, const WCHAR *dir, const WCHAR *name)
{
    size_t n = wcslen(dir);
    if (n && (dir[n - 1] == L'\\' || dir[n - 1] == L'/'))
        _snwprintf(out, cch, L"%s%s", dir, name);
    else
        _snwprintf(out, cch, L"%s\\%s", dir, name);
    out[cch - 1] = 0;
}

static const WCHAR *ClassOf(HWND h, WCHAR *buf, int cch)
{
    buf[0] = 0;
    if (h) GetClassNameW(h, buf, cch);
    return buf;
}

static BOOL ClassIs(HWND h, const WCHAR *cls)
{
    WCHAR c[128];
    if (!h || !GetClassNameW(h, c, 128)) return FALSE;
    return wcscmp(c, cls) == 0;
}

/* Does a shown item name belong to this file? ("a.jpg" or "a" with hidden extension) */
static BOOL NameMatchesFile(const WCHAR *name, const WCHAR *path)
{
    const WCHAR *fn = PathFindFileNameW(path);
    if (_wcsicmp(fn, name) == 0) return TRUE;
    const WCHAR *dot = wcsrchr(fn, L'.');
    size_t n = wcslen(name);
    return dot && (size_t)(dot - fn) == n && _wcsnicmp(fn, name, n) == 0;
}

/* Folder + shown name -> real file. Used for dialogs and the desktop. */
static BOOL MatchFile(const WCHAR *dir, const WCHAR *name, WCHAR *out, size_t cch)
{
    WCHAR p[PATHBUF];
    if (!dir[0] || !name[0] || wcspbrk(name, L"\\/:*?\"<>|")) return FALSE;

    /* 1. Name already includes the extension. */
    JoinPath(p, PATHBUF, dir, name);
    DWORD a = GetFileAttributesW(p);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && IsPreviewable(p)) {
        wcsncpy(out, p, cch); out[cch - 1] = 0;
        return TRUE;
    }

    /* 2. Extension hidden: look for "name.<ext>". */
    WCHAR pattern[PATHBUF], file[PATHBUF];
    _snwprintf(file, PATHBUF, L"%s.*", name); file[PATHBUF - 1] = 0;
    JoinPath(pattern, PATHBUF, dir, file);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        dbg(L"  MatchFile: no file '%s' and no '%s.*' in '%s'", name, name, dir);
        return FALSE;
    }
    BOOL found = FALSE;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (NameMatchesFile(name, fd.cFileName) && IsPreviewable(fd.cFileName)) {
            JoinPath(out, cch, dir, fd.cFileName);
            found = TRUE;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!found) dbg(L"  MatchFile: '%s' in '%s' is not a previewable file", name, dir);
    return found;
}

/* ======================================================================= */
/* Explorer: exact file through the shell view                              */
/* ======================================================================= */

/* Finds the Explorer tab containing hwPt. Gives its folder path, and when
 * index >= 0, the exact path of item #index in the view. */
static BOOL ExplorerLookup(HWND hwPt, int index, WCHAR *dir, size_t dcch, WCHAR *file, size_t fcch,
                           BOOL *haveFile)
{
    IShellWindows *sw = NULL;
    BOOL ok = FALSE;
    *haveFile = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_ALL,
                                &IID_IShellWindows, (void **)&sw)) || !sw)
        return FALSE;

    long count = 0;
    IShellWindows_get_Count(sw, &count);
    for (long i = 0; i < count && !ok; i++) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_I4; v.lVal = i;
        IDispatch *disp = NULL;
        if (FAILED(IShellWindows_Item(sw, v, &disp)) || !disp) continue;

        IServiceProvider *sp = NULL;
        IShellBrowser *sb = NULL;
        IShellView *sv = NULL;
        IFolderView *fv = NULL;
        IPersistFolder2 *pf = NULL;
        PIDLIST_ABSOLUTE folder = NULL;
        HWND h = NULL;

        if (FAILED(IDispatch_QueryInterface(disp, &IID_IServiceProvider, (void **)&sp)) || !sp) goto next;
        if (FAILED(IServiceProvider_QueryService(sp, &MY_SID_STopLevelBrowser, &IID_IShellBrowser,
                                                 (void **)&sb)) || !sb) goto next;
        IShellBrowser_GetWindow(sb, &h);
        /* On Windows 11 h is the tab window, so IsChild picks the right tab. */
        if (!h || !(h == hwPt || IsChild(h, hwPt))) goto next;
        if (FAILED(IShellBrowser_QueryActiveShellView(sb, &sv)) || !sv) goto next;
        if (FAILED(IShellView_QueryInterface(sv, &IID_IFolderView, (void **)&fv)) || !fv) goto next;
        if (FAILED(IFolderView_GetFolder(fv, &IID_IPersistFolder2, (void **)&pf)) || !pf) goto next;
        if (FAILED(IPersistFolder2_GetCurFolder(pf, &folder)) || !folder) goto next;

        ok = SHGetPathFromIDListEx(folder, dir, (DWORD)dcch, GPFIDL_DEFAULT);
        if (!ok) { dir[0] = 0; ok = TRUE; }   /* virtual folder: item path may still work */

        if (index >= 0) {
            PITEMID_CHILD child = NULL;
            if (SUCCEEDED(IFolderView_Item(fv, index, &child)) && child) {
                PIDLIST_ABSOLUTE full = ILCombine(folder, child);
                if (full) {
                    if (SHGetPathFromIDListEx(full, file, (DWORD)fcch, GPFIDL_DEFAULT))
                        *haveFile = TRUE;
                    ILFree(full);
                }
                CoTaskMemFree(child);
            }
        }
    next:
        if (folder) CoTaskMemFree(folder);
        if (pf) IPersistFolder2_Release(pf);
        if (fv) IFolderView_Release(fv);
        if (sv) IShellView_Release(sv);
        if (sb) IShellBrowser_Release(sb);
        if (sp) IServiceProvider_Release(sp);
        IDispatch_Release(disp);
    }
    IShellWindows_Release(sw);
    return ok;
}

/* ======================================================================= */
/* Dialogs                                                                  */
/* ======================================================================= */

static BOOL CALLBACK FindBreadcrumbProc(HWND h, LPARAM lp)
{
    if (ClassIs(h, L"Breadcrumb Parent")) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

/* Map a display name like "Downloads" to its real path. */
static BOOL KnownFolderByName(const WCHAR *name, WCHAR *out, size_t cch)
{
    const KNOWNFOLDERID *ids[] = {
        &FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Pictures,
        &FOLDERID_Music, &FOLDERID_Videos, &FOLDERID_Profile, &FOLDERID_PublicPictures
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        IShellItem *si = NULL;
        if (FAILED(SHGetKnownFolderItem(ids[i], KF_FLAG_DEFAULT, NULL, &IID_IShellItem, (void **)&si)) || !si)
            continue;
        WCHAR *dn = NULL;
        BOOL match = FALSE;
        if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_NORMALDISPLAY, &dn)) && dn) {
            match = (_wcsicmp(dn, name) == 0);
            CoTaskMemFree(dn);
        }
        IShellItem_Release(si);
        if (match) {
            WCHAR *p = NULL;
            if (SUCCEEDED(SHGetKnownFolderPath(ids[i], 0, NULL, &p)) && p) {
                wcsncpy(out, p, cch); out[cch - 1] = 0;
                CoTaskMemFree(p);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Open/Save dialog: read the address bar text, e.g. "Address: D:\Photos". */
static BOOL DialogFolder(HWND root, WCHAR *out, size_t cch)
{
    HWND bc = NULL;
    EnumChildWindows(root, FindBreadcrumbProc, (LPARAM)&bc);
    if (!bc) { dbg(L"  Dialog: no address bar found (not a file dialog?)"); return FALSE; }
    HWND tb = FindWindowExW(bc, NULL, L"ToolbarWindow32", NULL);
    if (!tb) { dbg(L"  Dialog: address bar has no toolbar"); return FALSE; }

    WCHAR text[PATHBUF];
    if (!GetWindowTextW(tb, text, PATHBUF)) { dbg(L"  Dialog: address bar text is empty"); return FALSE; }
    dbg(L"  Dialog: address bar text '%s'", text);

    /* Strip the (localized) "Address: " prefix. */
    WCHAR *p = wcsstr(text, L": ");
    p = p ? p + 2 : text;

    if ((iswalpha(p[0]) && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\')) {
        wcsncpy(out, p, cch); out[cch - 1] = 0;
        return TRUE;
    }
    /* Not a real path ("Downloads", "Libraries\Pictures"): try the last part. */
    WCHAR *last = wcsrchr(p, L'\\');
    return KnownFolderByName(last ? last + 1 : p, out, cch);
}

/* ======================================================================= */
/* UI Automation                                                            */
/* ======================================================================= */

BOOL LocateInit(void)
{
    if (FAILED(CoCreateInstance(&MY_CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                &MY_IID_IUIAutomation, (void **)&s_uia)) || !s_uia)
        return FALSE;
    IUIAutomation_get_ControlViewWalker(s_uia, &s_walker);
    IUIAutomation_CreateTrueCondition(s_uia, &s_trueCond);
    IUIAutomation_CreateCacheRequest(s_uia, &s_cacheReq);
    if (!s_walker || !s_trueCond || !s_cacheReq) return FALSE;
    IUIAutomationCacheRequest_AddProperty(s_cacheReq, UIA_BoundingRectanglePropertyId);
    IUIAutomationCacheRequest_AddProperty(s_cacheReq, UIA_ControlTypePropertyId);
    return TRUE;
}

void LocateShutdown(void)
{
    if (s_cacheReq) IUIAutomationCacheRequest_Release(s_cacheReq);
    if (s_trueCond) IUIAutomationCondition_Release(s_trueCond);
    if (s_walker) IUIAutomationTreeWalker_Release(s_walker);
    if (s_uia) IUIAutomation_Release(s_uia);
    s_cacheReq = NULL; s_trueCond = NULL; s_walker = NULL; s_uia = NULL;
}

static int WindowKind(HWND hwPt, HWND root)
{
    if (ClassIs(root, L"CabinetWClass") || ClassIs(root, L"ExploreWClass")) return KIND_EXPLORER;
    if (ClassIs(root, L"#32770")) return KIND_DIALOG;
    if ((ClassIs(root, L"Progman") || ClassIs(root, L"WorkerW")) && ClassIs(hwPt, L"SysListView32"))
        return KIND_DESKTOP;
    return KIND_NONE;
}

/* Find the list item that contains pt, searching down from parent.
   One cross-process call per level: children come with their rect and type cached. */
static IUIAutomationElement *FindItemAt(IUIAutomationElement *parent, POINT pt, int depth)
{
    if (depth > 12) return NULL;
    IUIAutomationElementArray *arr = NULL;
    if (FAILED(IUIAutomationElement_FindAllBuildCache(parent, TreeScope_Children, s_trueCond,
                                                      s_cacheReq, &arr)) || !arr)
        return NULL;

    IUIAutomationElement *hit = NULL;
    int n = 0;
    IUIAutomationElementArray_get_Length(arr, &n);
    for (int i = 0; i < n && !hit; i++) {
        IUIAutomationElement *c = NULL;
        if (FAILED(IUIAutomationElementArray_GetElement(arr, i, &c)) || !c) continue;
        RECT r = {0};
        CONTROLTYPEID ct = 0;
        IUIAutomationElement_get_CachedBoundingRectangle(c, &r);
        IUIAutomationElement_get_CachedControlType(c, &ct);
        if (PtInRect(&r, pt)) {
            if (ct == UIA_ListItemControlTypeId || ct == UIA_DataItemControlTypeId)
                hit = c;                                  /* keep our reference */
            else
                hit = FindItemAt(c, pt, depth + 1);
        }
        if (hit != c) IUIAutomationElement_Release(c);
    }
    IUIAutomationElementArray_Release(arr);
    return hit;
}

/* AutomationId "12" -> 12. Anything else -> -1. */
static int ParseIndex(BSTR id)
{
    if (!id || !id[0]) return -1;
    for (const WCHAR *p = id; *p; p++)
        if (*p < L'0' || *p > L'9') return -1;
    return (int)wcstol(id, NULL, 10);
}

BOOL ResolveHoveredFile(POINT pt, WCHAR *out, size_t cch, RECT *itemRect, HWND *rootOut)
{
    static const WCHAR *kindNames[] = { L"other app", L"Explorer", L"dialog", L"desktop" };
    HWND hwPt = WindowFromPoint(pt);
    if (!hwPt) { dbg(L"  no window under cursor"); return FALSE; }
    HWND root = GetAncestor(hwPt, GA_ROOT);
    if (root == PreviewHwnd() || root == g_msgWnd) { dbg(L"  cursor is over ImageHover itself"); return FALSE; }
    int kind = WindowKind(hwPt, root);
    { WCHAR c1[64], c2[64];
      dbg(L"  window under cursor: %s (top window: %s) -> %s",
          ClassOf(hwPt, c1, 64), ClassOf(root, c2, 64), kindNames[kind]); }
    if (kind == KIND_NONE) { dbg(L"  STOP: not Explorer, a file dialog, or the desktop"); return FALSE; }
    if ((kind == KIND_EXPLORER && !g_set.inExplorer) || (kind == KIND_DIALOG && !g_set.inDialogs) ||
        (kind == KIND_DESKTOP && !g_set.onDesktop)) {
        dbg(L"  STOP: previews are turned off here in Settings");
        return FALSE;
    }

    /* 1. UI Automation: find the list item under the cursor. */
    IUIAutomationElement *el = NULL;
    HRESULT hr = IUIAutomation_ElementFromPoint(s_uia, pt, &el);
    if (FAILED(hr) || !el) { dbg(L"  STOP: UI Automation failed (hr=0x%08lX)", hr); return FALSE; }

    BOOL isItem = FALSE;
    for (int depth = 0; depth < 5 && el; depth++) {
        CONTROLTYPEID ct = 0;
        IUIAutomationElement_get_CurrentControlType(el, &ct);
        { BSTR n = NULL; IUIAutomationElement_get_CurrentName(el, &n);
          dbg(L"  UIA level %d: type=%d name='%s'", depth, (int)ct, n ? n : L"");
          SysFreeString(n); }
        if (ct == UIA_ListItemControlTypeId || ct == UIA_DataItemControlTypeId) { isItem = TRUE; break; }
        if (ct == UIA_ListControlTypeId || ct == UIA_WindowControlTypeId || ct == UIA_PaneControlTypeId)
            break;
        IUIAutomationElement *parent = NULL;
        IUIAutomationTreeWalker_GetParentElement(s_walker, el, &parent);
        IUIAutomationElement_Release(el);
        el = parent;
    }
    if (!isItem) {
        /* Windows 11 Explorer can put an invisible "PopupHost" layer over the list.
           Then ElementFromPoint returns that layer. Search down from the window instead. */
        if (el) IUIAutomationElement_Release(el);
        el = NULL;
        IUIAutomationElement *top = NULL;
        if (SUCCEEDED(IUIAutomation_ElementFromHandle(s_uia, hwPt, &top)) && top) {
            el = FindItemAt(top, pt, 0);
            IUIAutomationElement_Release(top);
        }
        if (!el) { dbg(L"  STOP: cursor is not on a file item (search from window found nothing)"); return FALSE; }
        dbg(L"  found item by searching down from the window");
    }

    BSTR name = NULL, autoId = NULL;
    RECT r = {0};
    IUIAutomationElement_get_CurrentName(el, &name);
    IUIAutomationElement_get_CurrentAutomationId(el, &autoId);
    IUIAutomationElement_get_CurrentBoundingRectangle(el, &r);
    IUIAutomationElement_Release(el);
    int index = ParseIndex(autoId);
    SysFreeString(autoId);
    if (!name) { dbg(L"  STOP: item has no name"); return FALSE; }

    /* 2. The real file. */
    BOOL ok = FALSE;
    WCHAR dir[PATHBUF] = L"";
    if (kind == KIND_EXPLORER) {
        WCHAR exact[PATHBUF];
        BOOL haveFile = FALSE;
        BOOL f = ExplorerLookup(hwPt, index, dir, PATHBUF, exact, PATHBUF, &haveFile);
        if (!f) dbg(L"  STOP: could not find the Explorer folder");
        else dbg(L"  folder: '%s', item index %d", dir, index);
        if (haveFile && NameMatchesFile(name, exact)) {
            DWORD a = GetFileAttributesW(exact);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && IsPreviewable(exact)) {
                wcsncpy(out, exact, cch); out[cch - 1] = 0;
                ok = TRUE;
                dbg(L"  exact file from Explorer view");
            } else {
                dbg(L"  STOP: '%s' is a folder or not previewable", exact);
                SysFreeString(name);
                return FALSE;
            }
        } else if (f && dir[0]) {
            if (haveFile) dbg(L"  view index did not match the name, using name match");
            ok = MatchFile(dir, name, out, cch);
        }
    } else if (kind == KIND_DIALOG) {
        BOOL f = DialogFolder(root, dir, PATHBUF);
        if (f) dbg(L"  folder: '%s'", dir); else dbg(L"  STOP: could not find the dialog folder");
        ok = f && MatchFile(dir, name, out, cch);
    } else if (kind == KIND_DESKTOP) {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, dir)))
            ok = MatchFile(dir, name, out, cch);
        if (!ok && SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, dir)))
            ok = MatchFile(dir, name, out, cch);
    }
    SysFreeString(name);

    if (ok) {
        *itemRect = r; *rootOut = root;
        dbg(L"  file: '%s'", out);
    }
    return ok;
}
