/*
 * render.c - the preview window.
 *
 * A layered, click-through, never-activated popup. Per-pixel alpha gives soft
 * rounded corners. Animated images advance with a timer on this window.
 */
#include "common.h"

#define ID_ANIM        1
#define CURSOR_OFFSET  24
#define PAD            8
#define LINE_H         18
#define MIN_WIDTH      240
#define RADIUS         8
#define SCREEN_MARGIN  8      /* Max mode: gap to the screen edge */

static const BYTE BG = 0x20, BORDER = 0x4A;

static HWND     s_wnd;
static Preview *s_cur;
static int      s_frame;

static HBITMAP  s_dib;
static BYTE    *s_bits;
static BYTE    *s_bg;           /* background template (rounded rect), W*H*4 */
static int      s_W, s_H;
static POINT    s_pos;
static int      s_imgX, s_imgY;
static int      s_imgW, s_imgH;  /* drawn image size (smaller than the frames in Max mode) */
static BYTE   **s_scaled;        /* Max mode: frames scaled to s_imgW x s_imgH, made when first drawn */
static int      s_scaledCount;

static UINT     s_fontDpi;
static HFONT    s_fontName, s_fontInfo;
static WCHAR    s_line1[PATHBUF], s_line2[512];
static RECT     s_rc1, s_rc2;
static int      s_lines;

/* ======================================================================= */
/* Layout helpers                                                           */
/* ======================================================================= */

static void MonitorAt(POINT pt, RECT *work, UINT *dpi)
{
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    *work = mi.rcWork;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    *dpi = dx;
}

static BOOL MaxMode(void) { return g_set.sizePct >= SIZE_PCT_MAX; }

/* Height of the caption area for n text lines. */
static int CaptionH(UINT dpi, int lines)
{
    return lines * MulDiv(LINE_H, dpi, 96) + MulDiv(PAD, dpi, 96) / 2;
}

/* Largest image size to decode. In Max mode: the whole work area minus frame and
   caption, so the image can later be fitted into the biggest free side. */
void PreviewBoxFor(POINT pt, int *maxW, int *maxH)
{
    RECT wa; UINT dpi;
    MonitorAt(pt, &wa, &dpi);
    if (MaxMode()) {
        int pad = MulDiv(PAD, dpi, 96), m = MulDiv(SCREEN_MARGIN, dpi, 96);
        *maxW = (int)(wa.right - wa.left) - 2 * m - 2 * pad;
        *maxH = (int)(wa.bottom - wa.top) - 2 * m - 2 * pad - CaptionH(dpi, 2);
        if (*maxW < 64) *maxW = 64;
        if (*maxH < 64) *maxH = 64;
        return;
    }
    *maxW = (int)((wa.right - wa.left) * g_set.sizePct / 100);
    *maxH = (int)((wa.bottom - wa.top) * g_set.sizePct / 100);
}

/* Box-filter downscale of premultiplied BGRA. */
static BYTE *ScaleDown(const BYTE *src, int sw, int sh, int dw, int dh)
{
    BYTE *dst = (BYTE *)malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh), y1 = (int)((long long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw), x1 = (int)((long long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sum[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < y1; yy++) {
                const BYTE *s = src + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, s += 4) {
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2]; sum[3] += s[3];
                }
            }
            unsigned n = (unsigned)((y1 - y0) * (x1 - x0));
            BYTE *d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++) d[c] = (BYTE)((sum[c] + n / 2) / n);
        }
    }
    return dst;
}

static void FreeScaled(void)
{
    for (int i = 0; i < s_scaledCount; i++) free(s_scaled[i]);
    free(s_scaled);
    s_scaled = NULL; s_scaledCount = 0;
}

/* Pixels of frame f at the drawn size (NULL if out of memory). */
static const BYTE *FramePixels(int f)
{
    if (!s_scaled) return s_cur->frames[f];
    if (!s_scaled[f]) s_scaled[f] = ScaleDown(s_cur->frames[f], s_cur->w, s_cur->h, s_imgW, s_imgH);
    return s_scaled[f];
}

static void EnsureFonts(UINT dpi)
{
    if (s_fontName && s_fontDpi == dpi) return;
    if (s_fontName) DeleteObject(s_fontName);
    if (s_fontInfo) DeleteObject(s_fontInfo);
    int h = -MulDiv(9, dpi, 72);
    s_fontName = CreateFontW(h, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    s_fontInfo = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    s_fontDpi = dpi;
}

static void Append(WCHAR *buf, size_t cch, const WCHAR *part)
{
    if (!part[0]) return;
    if (buf[0]) wcsncat(buf, L"  \x00B7  ", cch - wcslen(buf) - 1);
    wcsncat(buf, part, cch - wcslen(buf) - 1);
}

static void BuildCaption(const Preview *p)
{
    WCHAR dims[64] = L"";
    if (p->imgW && p->imgH) _snwprintf(dims, 64, L"%u \x00D7 %u", p->imgW, p->imgH);
    const WCHAR *name = PathFindFileNameW(p->path);

    s_line2[0] = 0;
    if (!g_set.details) {
        _snwprintf(s_line1, PATHBUF, dims[0] ? L"%s   %s" : L"%s", name, dims);
        s_line1[PATHBUF - 1] = 0;
        s_lines = 1;
        return;
    }

    wcsncpy(s_line1, name, PATHBUF); s_line1[PATHBUF - 1] = 0;
    Append(s_line2, 512, dims);

    WCHAR size[32] = L"";
    StrFormatByteSizeW((LONGLONG)p->fileSize, size, 32);
    Append(s_line2, 512, size);

    /* Date taken (EXIF) if present, else last modified. */
    SYSTEMTIME st;
    BOOL haveDate = FALSE;
    if (p->hasTaken) { st = p->taken; haveDate = TRUE; }
    else {
        SYSTEMTIME utc;
        if (FileTimeToSystemTime(&p->mtime, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &st))
            haveDate = TRUE;
    }
    if (haveDate) {
        WCHAR d[64] = L"", t[32] = L"", both[100];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 64, NULL);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, t, 32);
        _snwprintf(both, 100, L"%s %s", d, t); both[99] = 0;
        Append(s_line2, 512, both);
    }
    Append(s_line2, 512, p->camera);
    if (p->frameCount > 1) Append(s_line2, 512, L"animated");
    s_lines = s_line2[0] ? 2 : 1;
}

/* Coverage of a rounded rectangle at a pixel centre (anti-aliased). */
static float Cover(float px, float py, float w, float h, float r)
{
    if (px < 0 || py < 0 || px > w || py > h) return 0.0f;
    float cx = px < r ? r : (px > w - r ? w - r : px);
    float cy = py < r ? r : (py > h - r ? h - r : py);
    float dx = px - cx, dy = py - cy;
    float c = r + 0.5f - sqrtf(dx * dx + dy * dy);
    return c < 0 ? 0.0f : (c > 1 ? 1.0f : c);
}

static void BuildBackground(UINT dpi)
{
    float r = (float)MulDiv(RADIUS, dpi, 96);
    for (int y = 0; y < s_H; y++) {
        BYTE *row = s_bg + (size_t)y * s_W * 4;
        for (int x = 0; x < s_W; x++) {
            float ao = Cover(x + 0.5f, y + 0.5f, (float)s_W, (float)s_H, r);
            float ai = Cover(x - 0.5f, y - 0.5f, (float)(s_W - 2), (float)(s_H - 2), r - 1);
            if (ai > ao) ai = ao;
            /* premultiplied: border ring + inner fill */
            BYTE c = (BYTE)(BORDER * (ao - ai) + BG * ai + 0.5f);
            row[x * 4 + 0] = c;
            row[x * 4 + 1] = c;
            row[x * 4 + 2] = c;
            row[x * 4 + 3] = (BYTE)(ao * 255 + 0.5f);
        }
    }
}

/* ======================================================================= */
/* Drawing                                                                  */
/* ======================================================================= */

static void Render(void)
{
    if (!s_cur || !s_bits) return;
    memcpy(s_bits, s_bg, (size_t)s_W * s_H * 4);

    /* Image frame over the background (both premultiplied). */
    const BYTE *src = FramePixels(s_frame);
    for (int y = 0; src && y < s_imgH; y++) {
        const BYTE *s = src + (size_t)y * s_imgW * 4;
        BYTE *d = s_bits + ((size_t)(s_imgY + y) * s_W + s_imgX) * 4;
        for (int x = 0; x < s_imgW; x++, s += 4, d += 4) {
            BYTE a = s[3], inv = (BYTE)(255 - a);
            d[0] = (BYTE)(s[0] + d[0] * inv / 255);
            d[1] = (BYTE)(s[1] + d[1] * inv / 255);
            d[2] = (BYTE)(s[2] + d[2] * inv / 255);
        }
    }

    /* Caption. */
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ oldBmp = SelectObject(dc, s_dib);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, s_fontName);
    SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
    DrawTextW(dc, s_line1, -1, &s_rc1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (s_lines > 1) {
        SelectObject(dc, s_fontInfo);
        SetTextColor(dc, RGB(0xA8, 0xA8, 0xA8));
        DrawTextW(dc, s_line2, -1, &s_rc2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    GdiFlush();
    SelectObject(dc, oldFont);

    /* GDI text clears alpha; the caption area is fully inside the window, so make it opaque. */
    RECT cap = s_rc1;
    if (s_lines > 1) UnionRect(&cap, &s_rc1, &s_rc2);
    for (int y = cap.top; y < cap.bottom && y < s_H; y++)
        for (int x = cap.left; x < cap.right && x < s_W; x++)
            s_bits[((size_t)y * s_W + x) * 4 + 3] = 255;

    POINT zero = { 0, 0 };
    SIZE sz = { s_W, s_H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_wnd, NULL, &s_pos, &sz, dc, &zero, 0, &bf, ULW_ALPHA);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
}

static void FreeSurface(void)
{
    if (s_dib) DeleteObject(s_dib);
    free(s_bg);
    s_dib = NULL; s_bits = NULL; s_bg = NULL;
}

/* ======================================================================= */
/* Public                                                                   */
/* ======================================================================= */

void PreviewShow(Preview *p, POINT pt)
{
    PreviewHide();
    if (!p || p->frameCount < 1) return;

    RECT wa; UINT dpi;
    MonitorAt(pt, &wa, &dpi);
    EnsureFonts(dpi);
    BuildCaption(p);

    int pad = MulDiv(PAD, dpi, 96);
    int lineH = MulDiv(LINE_H, dpi, 96);
    int capH = CaptionH(dpi, s_lines);
    int off = MulDiv(CURSOR_OFFSET, dpi, 96);
    int margin = MulDiv(SCREEN_MARGIN, dpi, 96);

    /* Max mode: pick the side of the cursor (right, left, below, above) where the
       image can be drawn largest, and shrink the image to fit that side. */
    int side = -1, sideW = 0;
    s_imgW = p->w; s_imgH = p->h;
    if (MaxMode()) {
        int fullW = (int)(wa.right - wa.left) - 2 * margin - 2 * pad;
        int fullH = (int)(wa.bottom - wa.top) - 2 * margin - 2 * pad - capH;
        int aw[4] = { (int)wa.right - margin - (pt.x + off) - 2 * pad,      /* right */
                      (pt.x - off) - ((int)wa.left + margin) - 2 * pad,     /* left  */
                      fullW, fullW };                                       /* below, above */
        int ah[4] = { fullH, fullH,
                      (int)wa.bottom - margin - (pt.y + off) - 2 * pad - capH,
                      (pt.y - off) - ((int)wa.top + margin) - 2 * pad - capH };
        double best = 0;
        for (int i = 0; i < 4; i++) {
            if (aw[i] < 16 || ah[i] < 16) continue;
            double s = (double)aw[i] / p->w;
            if ((double)ah[i] / p->h < s) s = (double)ah[i] / p->h;
            if (s > 1) s = 1;
            if (s > best + 1e-9) { best = s; side = i; }
        }
        if (side >= 0) {
            sideW = aw[side] + 2 * pad;
            s_imgW = (int)(p->w * best + 0.5); if (s_imgW < 1) s_imgW = 1;
            s_imgH = (int)(p->h * best + 0.5); if (s_imgH < 1) s_imgH = 1;
        }
    }
    FreeScaled();
    if (s_imgW != p->w || s_imgH != p->h) {
        s_scaled = (BYTE **)calloc((size_t)p->frameCount, sizeof(BYTE *));
        if (!s_scaled) { s_imgW = p->w; s_imgH = p->h; }
        else s_scaledCount = p->frameCount;
    }

    int W = s_imgW + 2 * pad, H = s_imgH + 2 * pad + capH;
    int minW = MulDiv(MIN_WIDTH, dpi, 96);
    if (side >= 0 && minW > sideW) minW = sideW;
    if (W < minW) W = minW;

    /* Widen for the caption, up to the size limit, so details are not cut off. */
    {
        HDC sdc = GetDC(NULL);
        SIZE t1 = {0}, t2 = {0};
        HGDIOBJ of = SelectObject(sdc, s_fontName);
        GetTextExtentPoint32W(sdc, s_line1, (int)wcslen(s_line1), &t1);
        SelectObject(sdc, s_fontInfo);
        if (s_lines > 1) GetTextExtentPoint32W(sdc, s_line2, (int)wcslen(s_line2), &t2);
        SelectObject(sdc, of);
        ReleaseDC(NULL, sdc);
        int textW = (t1.cx > t2.cx ? t1.cx : t2.cx) + 2 * pad + MulDiv(4, dpi, 96);
        int limit = side >= 0 ? sideW
                              : (int)((wa.right - wa.left) * (MaxMode() ? 45 : g_set.sizePct) / 100) + 2 * pad;
        if (textW > limit) textW = limit;
        if (W < textW) W = textW;
    }

    FreeSurface();
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&s_bits, NULL, 0);
    s_bg = (BYTE *)malloc((size_t)W * H * 4);
    if (!s_dib || !s_bits || !s_bg) { FreeSurface(); return; }
    s_W = W; s_H = H;
    BuildBackground(dpi);

    s_imgX = (W - s_imgW) / 2;
    s_imgY = pad;
    int ty = s_imgY + s_imgH + pad / 2;
    SetRect(&s_rc1, pad, ty, W - pad, ty + lineH);
    SetRect(&s_rc2, pad, ty + lineH, W - pad, ty + 2 * lineH);

    /* Next to the cursor, never under it. */
    int x, y;
    if (side >= 0) {
        int lo = (int)wa.left + margin, hiX = (int)wa.right - margin - W;
        int top = (int)wa.top + margin, hiY = (int)wa.bottom - margin - H;
        if (side <= 1) {                                    /* right / left: centre on the cursor row */
            x = side == 0 ? pt.x + off : pt.x - off - W;
            y = pt.y - H / 2;
            if (y > hiY) y = hiY;
            if (y < top) y = top;
        } else {                                            /* below / above: centre on the cursor column */
            y = side == 2 ? pt.y + off : pt.y - off - H;
            x = pt.x - W / 2;
            if (x > hiX) x = hiX;
            if (x < lo) x = lo;
        }
    } else {
        x = pt.x + off; y = pt.y + off;
        if (x + W > wa.right) x = pt.x - off - W;
        if (x < wa.left) x = wa.left;
        if (y + H > wa.bottom) y = pt.y - off - H;
        if (y < wa.top) y = wa.top;
    }
    s_pos.x = x; s_pos.y = y;

    s_cur = p;
    PreviewAddRef(p);
    s_frame = 0;
    Render();
#ifndef IH_TEST     /* the test harness renders without showing anything on screen */
    SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (p->frameCount > 1) SetTimer(s_wnd, ID_ANIM, p->delays[0], NULL);
#endif
}

#ifdef IH_TEST
/* Test only: where the preview window would be on screen. */
void PreviewWindowRect(RECT *r) { SetRect(r, s_pos.x, s_pos.y, s_pos.x + s_W, s_pos.y + s_H); }

/* Test only: save the composed preview (frame f) as a 32-bit BMP. */
BOOL PreviewDump(int f, const WCHAR *bmpPath)
{
    if (!s_cur || !s_bits) return FALSE;
    s_frame = f % s_cur->frameCount;
    Render();
    BITMAPFILEHEADER fh = {0};
    BITMAPINFOHEADER ih = {0};
    ih.biSize = sizeof(ih); ih.biWidth = s_W; ih.biHeight = -s_H; ih.biPlanes = 1;
    ih.biBitCount = 32; ih.biCompression = BI_RGB;
    DWORD bytes = (DWORD)s_W * s_H * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + bytes;
    FILE *out = _wfopen(bmpPath, L"wb");
    if (!out) return FALSE;
    fwrite(&fh, sizeof(fh), 1, out);
    fwrite(&ih, sizeof(ih), 1, out);
    fwrite(s_bits, 1, bytes, out);
    fclose(out);
    return TRUE;
}
#endif

void PreviewHide(void)
{
    KillTimer(s_wnd, ID_ANIM);
    if (IsWindowVisible(s_wnd)) ShowWindow(s_wnd, SW_HIDE);
    PreviewRelease(s_cur);
    s_cur = NULL;
    FreeScaled();
}

BOOL PreviewVisible(void)        { return s_cur != NULL && IsWindowVisible(s_wnd); }
const WCHAR *PreviewPath(void)   { return s_cur ? s_cur->path : L""; }
HWND PreviewHwnd(void)           { return s_wnd; }

static LRESULT CALLBACK PreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST:     return HTTRANSPARENT;
    case WM_TIMER:
        if (wp == ID_ANIM && s_cur && s_cur->frameCount > 1) {
            s_frame = (s_frame + 1) % s_cur->frameCount;
            Render();
            SetTimer(h, ID_ANIM, s_cur->delays[s_frame], NULL);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

BOOL PreviewWindowCreate(void)
{
    WNDCLASSW pc = {0};
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = g_inst;
    pc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    pc.lpszClassName = L"ImageHoverPreview";
    RegisterClassW(&pc);
    s_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            pc.lpszClassName, L"", WS_POPUP,
                            0, 0, 1, 1, NULL, NULL, g_inst, NULL);
    return s_wnd != NULL;
}

void PreviewWindowDestroy(void)
{
    PreviewHide();
    FreeSurface();
    if (s_fontName) DeleteObject(s_fontName);
    if (s_fontInfo) DeleteObject(s_fontInfo);
    s_fontName = s_fontInfo = NULL;
    if (s_wnd) DestroyWindow(s_wnd);
    s_wnd = NULL;
}
