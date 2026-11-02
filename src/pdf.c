/*
 * PDF preview: draws page 1 with the PDF reader built into Windows 10/11
 * (Windows.Data.Pdf, a WinRT API). No PDF program has to be installed.
 *
 * MinGW has no C header for Windows.Data.Pdf, so the few interfaces we need
 * are called by vtable slot. Slot numbers follow the interface order in the
 * Windows SDK .idl files (IUnknown = 0..2, IInspectable = 3..5).
 *
 * Runs on the loader thread (STA). Returns the page as a PNG in an IStream.
 */
#include "common.h"
#include <roapi.h>
#include <winstring.h>
#include <shcore.h>

static const GUID IID_PdfDocStatics  = {0x433A0B5F, 0xC007, 0x4788, {0x90,0xF2,0x08,0x14,0x3D,0x92,0x25,0x99}};
static const GUID IID_PdfRenderOpts  = {0x3C98056F, 0xB7CF, 0x4C29, {0x9A,0x04,0x52,0xD9,0x02,0x67,0xF4,0x25}};
static const GUID IID_RandomAccess   = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C,0x49,0x00,0x1E,0x4F,0xC6,0x86,0xDA}};
static const GUID IID_AsyncInfo      = {0x00000036, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

#define PDF_TIMEOUT_MS 10000

/* Call vtable slot i of a COM/WinRT object. */
#define SLOT(o, i, sig) ((sig)((*(void ***)(o))[i]))
typedef HRESULT (STDMETHODCALLTYPE *QI_fn)(void *, REFIID, void **);
typedef ULONG   (STDMETHODCALLTYPE *Rel_fn)(void *);

static void Rel(void *o) { if (o) SLOT(o, 2, Rel_fn)(o); }

typedef struct { float w, h; } WinSize;

/* Wait for an IAsyncOperation / IAsyncAction to finish. Pumps messages. */
static BOOL WaitAsync(void *op)
{
    void *info = NULL;
    if (FAILED(SLOT(op, 0, QI_fn)(op, &IID_AsyncInfo, &info)) || !info) return FALSE;
    typedef HRESULT (STDMETHODCALLTYPE *Status_fn)(void *, int *);   /* IAsyncInfo::get_Status */
    int st = 0;                                                       /* 0 Started, 1 Completed */
    DWORD t0 = GetTickCount();
    while (SUCCEEDED(SLOT(info, 7, Status_fn)(info, &st)) && st == 0 &&
           GetTickCount() - t0 < PDF_TIMEOUT_MS) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    }
    if (st == 0) {
        typedef HRESULT (STDMETHODCALLTYPE *Cancel_fn)(void *);
        SLOT(info, 9, Cancel_fn)(info);
    }
    Rel(info);
    if (st != 1) dbg(L"  pdf: async step did not complete (status %d)", st);
    return st == 1;
}

/* IAsyncOperation<T>::GetResults is slot 8. */
static void *AsyncResult(void *op)
{
    typedef HRESULT (STDMETHODCALLTYPE *Get_fn)(void *, void **);
    void *r = NULL;
    if (!WaitAsync(op) || FAILED(SLOT(op, 8, Get_fn)(op, &r))) return NULL;
    return r;
}

static void *Activate(const WCHAR *cls, const GUID *iid)
{
    HSTRING hs = NULL;
    void *insp = NULL, *out = NULL;
    if (FAILED(WindowsCreateString(cls, (UINT32)wcslen(cls), &hs))) return NULL;
    HRESULT hr = RoActivateInstance(hs, (IInspectable **)&insp);
    WindowsDeleteString(hs);
    if (FAILED(hr) || !insp) { dbg(L"  pdf: cannot create %s (hr=0x%08lX)", cls, hr); return NULL; }
    SLOT(insp, 0, QI_fn)(insp, iid, &out);
    Rel(insp);
    return out;
}

BOOL PdfRenderFirstPage(const WCHAR *path, int maxW, int maxH, IStream **png)
{
    void *statics = NULL, *fileStream = NULL, *opLoad = NULL, *doc = NULL, *page = NULL;
    void *opts = NULL, *outStream = NULL, *opRender = NULL;
    HSTRING cls = NULL;
    BOOL ok = FALSE;
    HRESULT hr;
    *png = NULL;

    const WCHAR *docCls = L"Windows.Data.Pdf.PdfDocument";
    if (FAILED(WindowsCreateString(docCls, (UINT32)wcslen(docCls), &cls))) goto done;
    hr = RoGetActivationFactory(cls, &IID_PdfDocStatics, &statics);
    if (FAILED(hr) || !statics) { dbg(L"  pdf: no Windows PDF reader (hr=0x%08lX)", hr); goto done; }

    /* Open the file as a WinRT stream (read only). */
    hr = CreateRandomAccessStreamOnFile(path, 0 /* FileAccessMode_Read */, &IID_RandomAccess, &fileStream);
    if (FAILED(hr) || !fileStream) { dbg(L"  pdf: cannot open file (hr=0x%08lX)", hr); goto done; }

    /* IPdfDocumentStatics::LoadFromStreamAsync = slot 8 */
    typedef HRESULT (STDMETHODCALLTYPE *Load_fn)(void *, void *, void **);
    hr = SLOT(statics, 8, Load_fn)(statics, fileStream, &opLoad);
    if (FAILED(hr) || !opLoad || !(doc = AsyncResult(opLoad))) {
        dbg(L"  pdf: cannot load document (hr=0x%08lX). Password protected or damaged?", hr);
        goto done;
    }

    /* IPdfDocument::GetPage = slot 6 */
    typedef HRESULT (STDMETHODCALLTYPE *GetPage_fn)(void *, UINT32, void **);
    if (FAILED(SLOT(doc, 6, GetPage_fn)(doc, 0, &page)) || !page) goto done;

    /* IPdfPage::get_Size = slot 10 (in DIPs, 96 per inch) */
    typedef HRESULT (STDMETHODCALLTYPE *Size_fn)(void *, WinSize *);
    WinSize sz = {0, 0};
    if (FAILED(SLOT(page, 10, Size_fn)(page, &sz)) || sz.w < 1 || sz.h < 1) goto done;

    /* Fit the page into the preview box. Pages may be drawn larger than their
       DIP size (they are vector), so scale up too, for a sharp preview. */
    double s = (double)maxW / sz.w;
    if ((double)maxH / sz.h < s) s = (double)maxH / sz.h;
    UINT tw = (UINT)(sz.w * s + 0.5), th = (UINT)(sz.h * s + 0.5);
    if (!tw) tw = 1;
    if (!th) th = 1;

    opts = Activate(L"Windows.Data.Pdf.PdfPageRenderOptions", &IID_PdfRenderOpts);
    outStream = Activate(L"Windows.Storage.Streams.InMemoryRandomAccessStream", &IID_RandomAccess);
    if (!outStream) goto done;

    if (opts) {
        /* IPdfPageRenderOptions::put_DestinationWidth = 9, put_DestinationHeight = 11 */
        typedef HRESULT (STDMETHODCALLTYPE *PutU_fn)(void *, UINT32);
        SLOT(opts, 9, PutU_fn)(opts, tw);
        SLOT(opts, 11, PutU_fn)(opts, th);
        /* IPdfPage::RenderWithOptionsToStreamAsync = slot 7 */
        typedef HRESULT (STDMETHODCALLTYPE *RenderO_fn)(void *, void *, void *, void **);
        hr = SLOT(page, 7, RenderO_fn)(page, outStream, opts, &opRender);
    } else {
        /* IPdfPage::RenderToStreamAsync = slot 6 (page at its own size) */
        typedef HRESULT (STDMETHODCALLTYPE *Render_fn)(void *, void *, void **);
        hr = SLOT(page, 6, Render_fn)(page, outStream, &opRender);
    }
    if (FAILED(hr) || !opRender || !WaitAsync(opRender)) {
        dbg(L"  pdf: cannot draw page (hr=0x%08lX)", hr);
        goto done;
    }

    IStream *st = NULL;
    if (FAILED(CreateStreamOverRandomAccessStream((IUnknown *)outStream, &IID_IStream, (void **)&st)) || !st)
        goto done;
    LARGE_INTEGER zero = {0};
    IStream_Seek(st, zero, STREAM_SEEK_SET, NULL);
    *png = st;
    ok = TRUE;
    dbg(L"  pdf: page 1 drawn at %ux%u", tw, th);

done:
    Rel(opRender); Rel(outStream); Rel(opts); Rel(page); Rel(doc);
    Rel(opLoad); Rel(fileStream); Rel(statics);
    if (cls) WindowsDeleteString(cls);
    return ok;
}
