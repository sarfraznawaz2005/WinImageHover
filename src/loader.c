/*
 * loader.c - decode previews on a background thread, keep a small cache.
 *
 *   - Images: WIC (Windows Imaging Component). Scales while decoding, applies
 *     EXIF rotation, reads camera and date taken.
 *   - Animated GIF / WebP: all frames are composed and scaled (memory capped).
 *   - Everything else (video, PDF, RAW...) and images WIC cannot read:
 *     Windows thumbnails (IShellItemImageFactory), if enabled.
 *
 * The UI thread asks with LoaderRequest(); the result comes back as
 * WM_PREVIEW_READY. Only the newest request is kept: if the user moves on while
 * a file is decoding, older requests are dropped.
 */
#include "common.h"

#define MAX_FRAMES       500
#define MAX_ANIM_BYTES   ((SIZE_T)128 << 20)   /* per animation */
#define CACHE_SLOTS      24
#define CACHE_BYTES      ((SIZE_T)192 << 20)
#define THUMB_MAX_REQ    1600

/* ======================================================================= */
/* Preview objects                                                          */
/* ======================================================================= */

static Preview *PreviewNew(const WCHAR *path, int maxW, int maxH)
{
    Preview *p = (Preview *)calloc(1, sizeof(Preview));
    if (!p) return NULL;
    p->refs = 1;
    wcsncpy(p->path, path, PATHBUF); p->path[PATHBUF - 1] = 0;
    p->maxW = maxW; p->maxH = maxH;
    return p;
}

void PreviewAddRef(Preview *p)
{
    if (p) InterlockedIncrement(&p->refs);
}

void PreviewRelease(Preview *p)
{
    if (!p || InterlockedDecrement(&p->refs) != 0) return;
    for (int i = 0; i < p->frameCount; i++) free(p->frames[i]);
    free(p->frames);
    free(p->delays);
    free(p);
}

static BOOL AllocFrames(Preview *p, int n)
{
    p->frames = (BYTE **)calloc((size_t)n, sizeof(BYTE *));
    p->delays = (UINT *)calloc((size_t)n, sizeof(UINT));
    return p->frames && p->delays;
}

static BOOL FileInfo(const WCHAR *path, FILETIME *mt, ULONGLONG *size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return FALSE;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return FALSE;
    *mt = d.ftLastWriteTime;
    *size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return TRUE;
}

/* Largest size that fits the box, never bigger than the original. */
static void FitBox(UINT w, UINT h, int maxW, int maxH, UINT *ow, UINT *oh)
{
    double s = 1.0;
    if (w > (UINT)maxW) s = (double)maxW / w;
    if (h * s > maxH) s = (double)maxH / h;
    *ow = (UINT)(w * s + 0.5); if (*ow < 1) *ow = 1;
    *oh = (UINT)(h * s + 0.5); if (*oh < 1) *oh = 1;
}

/* source -> (scale) -> (rotate) -> premultiplied BGRA pixels. */
static BYTE *ToPixels(IWICImagingFactory *wic, IWICBitmapSource *src, UINT sw, UINT sh,
                      WICBitmapTransformOptions xf, UINT *outW, UINT *outH)
{
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapFlipRotator *rot = NULL;
    IWICFormatConverter *conv = NULL;
    BYTE *pix = NULL;
    UINT w = 0, h = 0;

    IWICBitmapSource_GetSize(src, &w, &h);
    if (sw != w || sh != h) {
        if (FAILED(IWICImagingFactory_CreateBitmapScaler(wic, &scaler)) ||
            FAILED(IWICBitmapScaler_Initialize(scaler, src, sw, sh, WICBitmapInterpolationModeFant)))
            goto done;
        src = (IWICBitmapSource *)scaler;
    }
    if (xf != WICBitmapTransformRotate0) {
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapFlipRotator(wic, &rot)) &&
            SUCCEEDED(IWICBitmapFlipRotator_Initialize(rot, src, xf)))
            src = (IWICBitmapSource *)rot;
    }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(wic, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, src, &GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0,
                                              WICBitmapPaletteTypeCustom)))
        goto done;

    IWICFormatConverter_GetSize(conv, &w, &h);
    pix = (BYTE *)malloc((size_t)w * h * 4);
    if (!pix) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, pix))) {
        free(pix); pix = NULL;
        goto done;
    }
    *outW = w; *outH = h;

done:
    if (conv) IWICFormatConverter_Release(conv);
    if (rot) IWICBitmapFlipRotator_Release(rot);
    if (scaler) IWICBitmapScaler_Release(scaler);
    return pix;
}

/* ======================================================================= */
/* Metadata                                                                 */
/* ======================================================================= */

static BOOL MetaString(IWICMetadataQueryReader *qr, const WCHAR *name, WCHAR *out, size_t cch)
{
    PROPVARIANT pv; PropVariantInit(&pv);
    out[0] = 0;
    if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(qr, name, &pv))) {
        if (pv.vt == VT_LPSTR && pv.pszVal)
            MultiByteToWideChar(CP_ACP, 0, pv.pszVal, -1, out, (int)cch);
        else if (pv.vt == VT_LPWSTR && pv.pwszVal)
            wcsncpy(out, pv.pwszVal, cch);
    }
    PropVariantClear(&pv);
    out[cch - 1] = 0;
    size_t n = wcslen(out);
    while (n && (out[n - 1] == L' ' || out[n - 1] == L'\t')) out[--n] = 0;
    return out[0] != 0;
}

static BOOL MetaUInt(IWICMetadataQueryReader *qr, const WCHAR *name, UINT *v)
{
    PROPVARIANT pv; PropVariantInit(&pv);
    BOOL ok = FALSE;
    if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(qr, name, &pv))) {
        ok = TRUE;
        switch (pv.vt) {
        case VT_UI1: *v = pv.bVal; break;
        case VT_UI2: *v = pv.uiVal; break;
        case VT_UI4: *v = pv.ulVal; break;
        default: ok = FALSE;
        }
    }
    PropVariantClear(&pv);
    return ok;
}

/* Orientation, camera and date taken from EXIF (JPEG: /app1/ifd, TIFF/HEIF: /ifd). */
static USHORT ReadPhotoInfo(IWICBitmapFrameDecode *fr, Preview *p)
{
    static const WCHAR *const prefixes[] = { L"/app1/ifd", L"/ifd" };
    USHORT orient = 1;
    IWICMetadataQueryReader *qr = NULL;
    if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(fr, &qr)) || !qr) return 1;

    for (int i = 0; i < 2; i++) {
        WCHAR q[64], make[64], model[64], taken[64];
        UINT o = 0;
        BOOL any = FALSE;

        _snwprintf(q, 64, L"%s/{ushort=274}", prefixes[i]);
        if (MetaUInt(qr, q, &o)) { any = TRUE; if (o >= 1 && o <= 8) orient = (USHORT)o; }

        _snwprintf(q, 64, L"%s/{ushort=271}", prefixes[i]);
        BOOL hasMake = MetaString(qr, q, make, 64);
        _snwprintf(q, 64, L"%s/{ushort=272}", prefixes[i]);
        BOOL hasModel = MetaString(qr, q, model, 64);
        if (hasModel) {
            /* "Canon" + "Canon EOS R6" -> "Canon EOS R6"; "NIKON CORPORATION" + "NIKON D750" -> "NIKON D750" */
            WCHAR firstWord[64];
            wcsncpy(firstWord, make, 64); firstWord[63] = 0;
            WCHAR *sp = wcschr(firstWord, L' ');
            if (sp) *sp = 0;
            if (!hasMake || _wcsnicmp(model, firstWord, wcslen(firstWord)) == 0)
                wcsncpy(p->camera, model, 128);
            else
                _snwprintf(p->camera, 128, L"%s %s", make, model);
            p->camera[127] = 0;
            any = TRUE;
        } else if (hasMake) {
            wcsncpy(p->camera, make, 128); p->camera[127] = 0;
            any = TRUE;
        }

        _snwprintf(q, 64, L"%s/exif/{ushort=36867}", prefixes[i]);
        if (MetaString(qr, q, taken, 64)) {
            int Y, M, D, h, m, s;
            if (swscanf(taken, L"%d:%d:%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6 && Y > 1900 && M >= 1 &&
                M <= 12 && D >= 1 && D <= 31) {
                SYSTEMTIME st = {0};
                st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
                st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)s;
                p->taken = st;
                p->hasTaken = TRUE;
            }
            any = TRUE;
        }
        if (any) break;
    }
    IWICMetadataQueryReader_Release(qr);
    return orient;
}

static WICBitmapTransformOptions OrientationToTransform(USHORT o)
{
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

/* ======================================================================= */
/* Decoders                                                                 */
/* ======================================================================= */

static BOOL LoadStill(IWICImagingFactory *wic, IWICBitmapFrameDecode *fr, Preview *p)
{
    UINT w = 0, h = 0;
    IWICBitmapFrameDecode_GetSize(fr, &w, &h);
    if (!w || !h) return FALSE;

    USHORT orient = ReadPhotoInfo(fr, p);
    BOOL swap = orient >= 5;
    UINT dispW = swap ? h : w, dispH = swap ? w : h;
    UINT tw, th;
    FitBox(dispW, dispH, p->maxW, p->maxH, &tw, &th);
    UINT sw = swap ? th : tw, sh = swap ? tw : th;     /* scale in source orientation */

    UINT ow = 0, oh = 0;
    BYTE *pix = ToPixels(wic, (IWICBitmapSource *)fr, sw, sh, OrientationToTransform(orient), &ow, &oh);
    if (!pix || !AllocFrames(p, 1)) { free(pix); return FALSE; }
    p->frames[0] = pix;
    p->delays[0] = 0;
    p->frameCount = 1;
    p->w = (int)ow; p->h = (int)oh;
    p->imgW = dispW; p->imgH = dispH;
    p->bytes = (SIZE_T)ow * oh * 4;
    return TRUE;
}

/* Animated GIF (and WebP when its codec exposes frames). */
static BOOL LoadAnimated(IWICImagingFactory *wic, IWICBitmapDecoder *dec, UINT count, Preview *p)
{
    UINT cw = 0, ch = 0;
    IWICMetadataQueryReader *dq = NULL;
    if (SUCCEEDED(IWICBitmapDecoder_GetMetadataQueryReader(dec, &dq)) && dq) {
        MetaUInt(dq, L"/logscrdesc/Width", &cw);
        MetaUInt(dq, L"/logscrdesc/Height", &ch);
        IWICMetadataQueryReader_Release(dq);
    }
    if (!cw || !ch) {
        IWICBitmapFrameDecode *f0 = NULL;
        if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &f0)) || !f0) return FALSE;
        IWICBitmapFrameDecode_GetSize(f0, &cw, &ch);
        IWICBitmapFrameDecode_Release(f0);
    }
    if (!cw || !ch || (ULONGLONG)cw * ch > 64ull * 1024 * 1024) return FALSE;

    UINT tw, th;
    FitBox(cw, ch, p->maxW, p->maxH, &tw, &th);
    SIZE_T frameBytes = (SIZE_T)tw * th * 4;
    UINT maxFrames = count;
    if (maxFrames > MAX_FRAMES) maxFrames = MAX_FRAMES;
    if (maxFrames > MAX_ANIM_BYTES / frameBytes) maxFrames = (UINT)(MAX_ANIM_BYTES / frameBytes);
    if (maxFrames < 2) return FALSE;

    BYTE *canvas = (BYTE *)calloc((size_t)cw * ch, 4);
    if (!canvas || !AllocFrames(p, (int)maxFrames)) { free(canvas); return FALSE; }

    int n = 0;
    for (UINT i = 0; i < maxFrames; i++) {
        IWICBitmapFrameDecode *fr = NULL;
        if (FAILED(IWICBitmapDecoder_GetFrame(dec, i, &fr)) || !fr) break;

        UINT fw = 0, fh = 0, left = 0, top = 0, delay = 0, disposal = 0;
        IWICBitmapFrameDecode_GetSize(fr, &fw, &fh);
        IWICMetadataQueryReader *qr = NULL;
        if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(fr, &qr)) && qr) {
            MetaUInt(qr, L"/imgdesc/Left", &left);
            MetaUInt(qr, L"/imgdesc/Top", &top);
            if (MetaUInt(qr, L"/grctlext/Delay", &delay)) delay *= 10;     /* 1/100 s -> ms */
            MetaUInt(qr, L"/grctlext/Disposal", &disposal);
            IWICMetadataQueryReader_Release(qr);
        }

        UINT pw = 0, ph = 0;
        BYTE *buf = ToPixels(wic, (IWICBitmapSource *)fr, fw, fh, WICBitmapTransformRotate0, &pw, &ph);
        IWICBitmapFrameDecode_Release(fr);
        if (!buf) break;

        BYTE *saved = NULL;
        if (disposal == 3) {
            saved = (BYTE *)malloc((size_t)cw * ch * 4);
            if (saved) memcpy(saved, canvas, (size_t)cw * ch * 4);
        }

        /* Draw this frame onto the canvas. */
        for (UINT y = 0; y < ph && top + y < ch; y++) {
            BYTE *s = buf + (size_t)y * pw * 4;
            BYTE *d = canvas + ((size_t)(top + y) * cw + left) * 4;
            for (UINT x = 0; x < pw && left + x < cw; x++, s += 4, d += 4) {
                BYTE a = s[3];
                if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
                else if (a) {
                    BYTE inv = (BYTE)(255 - a);
                    d[0] = (BYTE)(s[0] + d[0] * inv / 255);
                    d[1] = (BYTE)(s[1] + d[1] * inv / 255);
                    d[2] = (BYTE)(s[2] + d[2] * inv / 255);
                    d[3] = (BYTE)(a + d[3] * inv / 255);
                }
            }
        }
        free(buf);

        /* Scale the composed canvas into a frame. */
        IWICBitmap *bm = NULL;
        BYTE *out = NULL;
        UINT ow = 0, oh = 0;
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(wic, cw, ch, &GUID_WICPixelFormat32bppPBGRA,
                                                                cw * 4, cw * ch * 4, canvas, &bm)) && bm) {
            out = ToPixels(wic, (IWICBitmapSource *)bm, tw, th, WICBitmapTransformRotate0, &ow, &oh);
            IWICBitmap_Release(bm);
        }
        if (!out || ow != tw || oh != th) { free(out); free(saved); break; }
        p->frames[n] = out;
        p->delays[n] = delay < 20 ? 100 : delay;    /* browsers treat 0-10 ms as 100 ms */
        n++;

        /* Dispose before the next frame. */
        if (disposal == 2) {
            for (UINT y = top; y < top + fh && y < ch; y++)
                for (UINT x = left; x < left + fw && x < cw; x++)
                    memset(canvas + ((size_t)y * cw + x) * 4, 0, 4);
        } else if (disposal == 3 && saved) {
            memcpy(canvas, saved, (size_t)cw * ch * 4);
        }
        free(saved);
    }
    free(canvas);

    p->frameCount = n;
    if (n < 2) return FALSE;        /* caller falls back to a still image */
    p->w = (int)tw; p->h = (int)th;
    p->imgW = cw; p->imgH = ch;
    p->bytes = frameBytes * (SIZE_T)n;
    return TRUE;
}

static void ResetFrames(Preview *p)
{
    for (int i = 0; i < p->frameCount; i++) free(p->frames[i]);
    free(p->frames); free(p->delays);
    p->frames = NULL; p->delays = NULL; p->frameCount = 0; p->bytes = 0;
}

static BOOL LoadWithWic(IWICImagingFactory *wic, Preview *p, BOOL animate)
{
    IWICBitmapDecoder *dec = NULL;
    HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(wic, p->path, NULL, GENERIC_READ,
                                                              WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr) || !dec) {
        dbg(L"  loader: Windows cannot read this image (hr=0x%08lX). Missing codec?", hr);
        return FALSE;
    }
    UINT count = 0;
    IWICBitmapDecoder_GetFrameCount(dec, &count);
    BOOL ok = FALSE;
    if (animate && count > 1) {
        ok = LoadAnimated(wic, dec, count, p);
        if (!ok) ResetFrames(p);
    }
    if (!ok) {
        IWICBitmapFrameDecode *fr = NULL;
        if (SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) && fr) {
            ok = LoadStill(wic, fr, p);
            IWICBitmapFrameDecode_Release(fr);
        }
    }
    IWICBitmapDecoder_Release(dec);
    return ok;
}

/* PDF: draw page 1 with the PDF reader built into Windows. */
static BOOL LoadPdf(IWICImagingFactory *wic, Preview *p)
{
    IStream *png = NULL;
    if (!PdfRenderFirstPage(p->path, p->maxW, p->maxH, &png)) return FALSE;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(wic, png, NULL, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) && fr)
        ok = LoadStill(wic, fr, p);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    IStream_Release(png);
    if (ok) { p->imgW = p->imgH = 0; p->fromThumbnail = TRUE; }   /* pixel size means nothing for a PDF */
    return ok;
}

/* Windows thumbnail: videos, PDFs, RAW, PSD... whatever has a thumbnail handler. */
static BOOL LoadThumbnail(IWICImagingFactory *wic, Preview *p)
{
    IShellItem *si = NULL;
    IShellItemImageFactory *f = NULL;
    HBITMAP hb = NULL;
    BYTE *buf = NULL;
    BOOL ok = FALSE;

    if (FAILED(SHCreateItemFromParsingName(p->path, NULL, &IID_IShellItem, (void **)&si)) || !si) goto done;
    if (FAILED(IShellItem_QueryInterface(si, &IID_IShellItemImageFactory, (void **)&f)) || !f) goto done;

    int req = p->maxW > p->maxH ? p->maxW : p->maxH;
    if (req > THUMB_MAX_REQ) req = THUMB_MAX_REQ;
    SIZE sz = { req, req };
    HRESULT hr = IShellItemImageFactory_GetImage(f, sz, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hb);
    if (FAILED(hr) || !hb) { dbg(L"  loader: no Windows thumbnail (hr=0x%08lX)", hr); goto done; }

    BITMAP bm;
    if (!GetObjectW(hb, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) goto done;
    int w = bm.bmWidth, h = abs(bm.bmHeight);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    buf = (BYTE *)malloc((size_t)w * h * 4);
    if (!buf) goto done;
    HDC dc = GetDC(NULL);
    int lines = GetDIBits(dc, hb, 0, (UINT)h, buf, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (lines != h) goto done;

    /* Many thumbnails have no alpha at all: make them opaque. */
    BOOL anyAlpha = FALSE;
    for (size_t i = 0; i < (size_t)w * h && !anyAlpha; i++) anyAlpha = buf[i * 4 + 3] != 0;
    if (!anyAlpha)
        for (size_t i = 0; i < (size_t)w * h; i++) buf[i * 4 + 3] = 255;

    IWICBitmap *wb = NULL;
    if (FAILED(IWICImagingFactory_CreateBitmapFromMemory(wic, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppPBGRA,
                                                         (UINT)w * 4, (UINT)(w * h * 4), buf, &wb)) || !wb)
        goto done;
    UINT tw, th, ow = 0, oh = 0;
    FitBox((UINT)w, (UINT)h, p->maxW, p->maxH, &tw, &th);
    BYTE *pix = ToPixels(wic, (IWICBitmapSource *)wb, tw, th, WICBitmapTransformRotate0, &ow, &oh);
    IWICBitmap_Release(wb);
    if (!pix || !AllocFrames(p, 1)) { free(pix); goto done; }
    p->frames[0] = pix;
    p->frameCount = 1;
    p->w = (int)ow; p->h = (int)oh;
    p->bytes = (SIZE_T)ow * oh * 4;
    p->fromThumbnail = TRUE;
    ok = TRUE;

done:
    free(buf);
    if (hb) DeleteObject(hb);
    if (f) IShellItemImageFactory_Release(f);
    if (si) IShellItem_Release(si);
    return ok;
}

/* ======================================================================= */
/* Worker thread                                                            */
/* ======================================================================= */

typedef struct Request {
    WCHAR path[PATHBUF];
    int   maxW, maxH;
    UINT  id;
    BOOL  animate, thumbs;
    BOOL  pending;
} Request;

static CRITICAL_SECTION s_cs;
static HANDLE           s_event, s_thread;
static volatile LONG    s_quit;
static Request          s_req;
static HWND             s_notify;

static Preview *LoadPreview(IWICImagingFactory *wic, const Request *r)
{
    Preview *p = PreviewNew(r->path, r->maxW, r->maxH);
    if (!p) return NULL;
    if (!FileInfo(r->path, &p->mtime, &p->fileSize)) { PreviewRelease(p); return NULL; }

    BOOL ok = FALSE;
    if (IsImageExt(r->path)) ok = LoadWithWic(wic, p, r->animate);
    if (!ok && r->thumbs && !_wcsicmp(PathFindExtensionW(r->path), L".pdf")) { ResetFrames(p); ok = LoadPdf(wic, p); }
    if (!ok && r->thumbs) { ResetFrames(p); ok = LoadThumbnail(wic, p); }
    if (!ok) { PreviewRelease(p); return NULL; }
    return p;
}

static DWORD WINAPI WorkerProc(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *wic = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                     &IID_IWICImagingFactory, (void **)&wic);

    for (;;) {
        WaitForSingleObject(s_event, INFINITE);
        if (s_quit) break;
        Request r;
        EnterCriticalSection(&s_cs);
        r = s_req;
        s_req.pending = FALSE;
        LeaveCriticalSection(&s_cs);
        if (!r.pending) continue;

        DWORD t0 = GetTickCount();
        Preview *p = wic ? LoadPreview(wic, &r) : NULL;
        dbg(L"  loader: '%s' %s in %lu ms%s", r.path, p ? L"ready" : L"FAILED", GetTickCount() - t0,
            p && p->frameCount > 1 ? L" (animated)" : (p && p->fromThumbnail ? L" (thumbnail)" : L""));
        if (s_quit || !PostMessageW(s_notify, WM_PREVIEW_READY, (WPARAM)r.id, (LPARAM)p))
            PreviewRelease(p);
    }
    if (wic) IWICImagingFactory_Release(wic);
    CoUninitialize();
    return 0;
}

BOOL LoaderStart(HWND notify)
{
    s_notify = notify;
    InitializeCriticalSection(&s_cs);
    s_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_event) return FALSE;
    s_thread = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    return s_thread != NULL;
}

void LoaderStop(void)
{
    if (!s_thread) return;
    InterlockedExchange(&s_quit, 1);
    SetEvent(s_event);
    /* A thumbnail handler can be slow. Do not hang the exit for it. */
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = NULL;
}

void LoaderRequest(const WCHAR *path, int maxW, int maxH, UINT id)
{
    EnterCriticalSection(&s_cs);
    wcsncpy(s_req.path, path, PATHBUF); s_req.path[PATHBUF - 1] = 0;
    s_req.maxW = maxW; s_req.maxH = maxH;
    s_req.id = id;
    s_req.animate = g_set.animate;
    s_req.thumbs = g_set.thumbnails;
    s_req.pending = TRUE;
    LeaveCriticalSection(&s_cs);
    SetEvent(s_event);
}

/* ======================================================================= */
/* Cache (UI thread only)                                                   */
/* ======================================================================= */

static Preview *s_cache[CACHE_SLOTS];
static DWORD    s_used[CACHE_SLOTS];
static DWORD    s_clock;

static BOOL SameKey(const Preview *p, const WCHAR *path, int maxW, int maxH)
{
    return p->maxW == maxW && p->maxH == maxH && _wcsicmp(p->path, path) == 0;
}

Preview *CacheFind(const WCHAR *path, int maxW, int maxH)
{
    FILETIME mt;
    ULONGLONG size;
    if (!FileInfo(path, &mt, &size)) return NULL;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        Preview *p = s_cache[i];
        if (!p || !SameKey(p, path, maxW, maxH)) continue;
        if (CompareFileTime(&p->mtime, &mt) != 0 || p->fileSize != size) {
            PreviewRelease(p);              /* file changed on disk */
            s_cache[i] = NULL;
            return NULL;
        }
        s_used[i] = ++s_clock;
        PreviewAddRef(p);
        return p;
    }
    return NULL;
}

static SIZE_T CacheBytes(void)
{
    SIZE_T total = 0;
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (s_cache[i]) total += s_cache[i]->bytes;
    return total;
}

static int OldestSlot(void)
{
    int best = -1;
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (s_cache[i] && (best < 0 || s_used[i] < s_used[best])) best = i;
    return best;
}

void CacheAdd(Preview *p)
{
    if (!p || p->bytes > CACHE_BYTES) return;
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (s_cache[i] && SameKey(s_cache[i], p->path, p->maxW, p->maxH)) {
            PreviewRelease(s_cache[i]);
            s_cache[i] = NULL;
        }
    while (CacheBytes() + p->bytes > CACHE_BYTES) {
        int o = OldestSlot();
        if (o < 0) break;
        PreviewRelease(s_cache[o]);
        s_cache[o] = NULL;
    }
    int slot = -1;
    for (int i = 0; i < CACHE_SLOTS && slot < 0; i++)
        if (!s_cache[i]) slot = i;
    if (slot < 0) {
        slot = OldestSlot();
        PreviewRelease(s_cache[slot]);
    }
    PreviewAddRef(p);
    s_cache[slot] = p;
    s_used[slot] = ++s_clock;
}

void CacheClear(void)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        PreviewRelease(s_cache[i]);
        s_cache[i] = NULL;
    }
}
