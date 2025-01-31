/* Copyright 2015 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include "FileUtil.h"
#include "UITask.h"
#include "WinUtil.h"

#include "BaseEngine.h"
#include "EngineManager.h"

#include "SettingsStructs.h"
#include "Controller.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "GlobalPrefs.h"
#include "TextSelection.h"
#include "TextSearch.h"

#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "TabInfo.h"
#include "AppUtil.h"
#include "Notifications.h"
#include "Print.h"
#include "Selection.h"
#include "SumatraDialogs.h"
#include "SumatraProperties.h"
#include "Translations.h"

struct PrintData {
    AutoFreeW printerName;
    ScopedMem<DEVMODE> devMode;
    BaseEngine* engine = nullptr;
    Vec<PRINTPAGERANGE> ranges; // empty when printing a selection
    Vec<SelectionOnPage> sel;   // empty when printing a page range
    Print_Advanced_Data advData;
    int rotation = 0;

    PrintData(BaseEngine* engine, PRINTER_INFO_2* printerInfo, DEVMODE* devMode, Vec<PRINTPAGERANGE>& ranges,
              Print_Advanced_Data& advData, int rotation = 0, Vec<SelectionOnPage>* sel = nullptr) {
        this->advData = advData;
        this->rotation = rotation;
        if (engine) {
            this->engine = engine->Clone();
        }

        if (printerInfo) {
            printerName.SetCopy(printerInfo->pPrinterName);
        }
        if (devMode) {
            this->devMode.Set((LPDEVMODE)memdup(devMode, devMode->dmSize + devMode->dmDriverExtra));
        }

        if (!sel) {
            this->ranges = ranges;
        } else {
            this->sel = *sel;
        }
    }

    ~PrintData() { delete engine; }
};

class AbortCookieManager {
    CRITICAL_SECTION cookieAccess;

  public:
    AbortCookie* cookie;

    AbortCookieManager() : cookie(nullptr) { InitializeCriticalSection(&cookieAccess); }
    ~AbortCookieManager() {
        Clear();
        DeleteCriticalSection(&cookieAccess);
    }

    void Abort() {
        ScopedCritSec scope(&cookieAccess);
        if (cookie) {
            cookie->Abort();
        }
        Clear();
    }

    void Clear() {
        ScopedCritSec scope(&cookieAccess);
        if (cookie) {
            delete cookie;
            cookie = nullptr;
        }
    }
};

static RectD BoundSelectionOnPage(const Vec<SelectionOnPage>& sel, int pageNo) {
    RectD bounds;
    for (size_t i = 0; i < sel.Count(); i++) {
        if (sel.At(i).pageNo == pageNo) {
            bounds = bounds.Union(sel.At(i).rect);
        }
    }
    return bounds;
}

static bool PrintToDevice(const PrintData& pd, ProgressUpdateUI* progressUI = nullptr,
                          AbortCookieManager* abortCookie = nullptr) {
    CrashIf(!pd.engine);
    if (!pd.engine) {
        return false;
    }
    CrashIf(!pd.printerName);
    if (!pd.printerName) {
        return false;
    }

    BaseEngine& engine = *pd.engine;
    AutoFreeW fileName;

    DOCINFO di = {0};
    di.cbSize = sizeof(DOCINFO);
    if (gPluginMode) {
        fileName.Set(url::GetFileName(gPluginURL));
        // fall back to a generic "filename" instead of the more confusing temporary filename
        di.lpszDocName = fileName ? fileName : L"filename";
    } else {
        di.lpszDocName = engine.FileName();
    }

    int current = 1, total = 0;
    if (pd.sel.Count() == 0) {
        for (size_t i = 0; i < pd.ranges.Count(); i++) {
            if (pd.ranges.At(i).nToPage < pd.ranges.At(i).nFromPage) {
                total += pd.ranges.At(i).nFromPage - pd.ranges.At(i).nToPage + 1;
            } else {
                total += pd.ranges.At(i).nToPage - pd.ranges.At(i).nFromPage + 1;
            }
        }
    } else {
        for (int pageNo = 1; pageNo <= engine.PageCount(); pageNo++) {
            if (!BoundSelectionOnPage(pd.sel, pageNo).IsEmpty()) {
                total++;
            }
        }
    }
    AssertCrash(total > 0);
    if (0 == total) {
        return false;
    }

    if (progressUI) {
        progressUI->UpdateProgress(current, total);
    }

    // cf. http://blogs.msdn.com/b/oldnewthing/archive/2012/11/09/10367057.aspx
    ScopedHDC hdc(CreateDC(nullptr, pd.printerName, nullptr, pd.devMode));
    if (!hdc) {
        return false;
    }

    if (StartDoc(hdc, &di) <= 0) {
        return false;
    }

    // MM_TEXT: Each logical unit is mapped to one device pixel.
    // Positive x is to the right; positive y is down.
    SetMapMode(hdc, MM_TEXT);

    const SizeI paperSize(GetDeviceCaps(hdc, PHYSICALWIDTH), GetDeviceCaps(hdc, PHYSICALHEIGHT));
    const RectI printable(GetDeviceCaps(hdc, PHYSICALOFFSETX), GetDeviceCaps(hdc, PHYSICALOFFSETY),
                          GetDeviceCaps(hdc, HORZRES), GetDeviceCaps(hdc, VERTRES));
    const float dpiFactor = std::min(GetDeviceCaps(hdc, LOGPIXELSX) / engine.GetFileDPI(),
                                     GetDeviceCaps(hdc, LOGPIXELSY) / engine.GetFileDPI());
    bool bPrintPortrait = paperSize.dx < paperSize.dy;
    if (pd.devMode && (pd.devMode.Get()->dmFields & DM_ORIENTATION))
        bPrintPortrait = DMORIENT_PORTRAIT == pd.devMode.Get()->dmOrientation;
    if (pd.advData.rotation == PrintRotationAdv::Portrait) {
        bPrintPortrait = true;
    } else if (pd.advData.rotation == PrintRotationAdv::Landscape) {
        bPrintPortrait = false;
    }

    if (pd.sel.Count() > 0) {
        for (int pageNo = 1; pageNo <= engine.PageCount(); pageNo++) {
            RectD bounds = BoundSelectionOnPage(pd.sel, pageNo);
            if (bounds.IsEmpty()) {
                continue;
            }

            if (progressUI) {
                progressUI->UpdateProgress(current, total);
            }

            StartPage(hdc);

            geomutil::SizeT<float> bSize = bounds.Size().Convert<float>();
            float zoom = std::min((float)printable.dx / bSize.dx, (float)printable.dy / bSize.dy);
            // use the correct zoom values, if the page fits otherwise
            // and the user didn't ask for anything else (default setting)
            if (PrintScaleAdv::Shrink == pd.advData.scale) {
                zoom = std::min(dpiFactor, zoom);
            } else if (PrintScaleAdv::None == pd.advData.scale) {
                zoom = dpiFactor;
            }

            for (size_t i = 0; i < pd.sel.Count(); i++) {
                if (pd.sel.At(i).pageNo != pageNo) {
                    continue;
                }

                RectD* clipRegion = &pd.sel.At(i).rect;
                PointI offset((int)((clipRegion->x - bounds.x) * zoom), (int)((clipRegion->y - bounds.y) * zoom));
                if (pd.advData.scale != PrintScaleAdv::None) {
                    // center the selection on the physical paper
                    offset.x += (int)(printable.dx - bSize.dx * zoom) / 2;
                    offset.y += (int)(printable.dy - bSize.dy * zoom) / 2;
                }

                bool ok = false;
                short shrink = 1;
                do {
                    RenderedBitmap* bmp =
                        engine.RenderBitmap(pd.sel.At(i).pageNo, zoom / shrink, pd.rotation, clipRegion, Target_Print,
                                            abortCookie ? &abortCookie->cookie : nullptr);
                    if (abortCookie) {
                        abortCookie->Clear();
                    }
                    if (bmp && bmp->GetBitmap()) {
                        RectI rc(offset.x, offset.y, bmp->Size().dx * shrink, bmp->Size().dy * shrink);
                        ok = bmp->StretchDIBits(hdc, rc);
                    }
                    delete bmp;
                    shrink *= 2;
                } while (!ok && shrink < 32 && !(progressUI && progressUI->WasCanceled()));
            }
            // TODO: abort if !ok?

            if (EndPage(hdc) <= 0 || progressUI && progressUI->WasCanceled()) {
                AbortDoc(hdc);
                return false;
            }
            current++;
        }

        EndDoc(hdc);
        return false;
    }

    // print all the pages the user requested
    for (size_t i = 0; i < pd.ranges.Count(); i++) {
        int dir = pd.ranges.At(i).nFromPage > pd.ranges.At(i).nToPage ? -1 : 1;
        for (DWORD pageNo = pd.ranges.At(i).nFromPage; pageNo != pd.ranges.At(i).nToPage + dir; pageNo += dir) {
            if ((PrintRangeAdv::Even == pd.advData.range && pageNo % 2 != 0) ||
                (PrintRangeAdv::Odd == pd.advData.range && pageNo % 2 == 0)) {
                continue;
            }
            if (progressUI) {
                progressUI->UpdateProgress(current, total);
            }

            StartPage(hdc);

            geomutil::SizeT<float> pSize = engine.PageMediabox(pageNo).Size().Convert<float>();
            int rotation = 0;
            // Turn the document by 90 deg if it isn't in portrait mode
            if (pSize.dx > pSize.dy) {
                rotation += 90;
                std::swap(pSize.dx, pSize.dy);
            }
            // make sure not to print upside-down
            rotation = (rotation % 180) == 0 ? 0 : 270;
            // finally turn the page by (another) 90 deg in landscape mode
            if (!bPrintPortrait) {
                rotation = (rotation + 90) % 360;
                std::swap(pSize.dx, pSize.dy);
            }

            // dpiFactor means no physical zoom
            float zoom = dpiFactor;
            // offset of the top-left corner of the page from the printable area
            // (negative values move the page into the left/top margins, etc.);
            // offset adjustments are needed because the GDI coordinate system
            // starts at the corner of the printable area and we rather want to
            // center the page on the physical paper (except for PrintScaleNone
            // where the page starts at the very top left of the physical paper so
            // that printing forms/labels of varying size remains reliably possible)
            PointI offset(-printable.x, -printable.y);

            if (pd.advData.scale != PrintScaleAdv::None) {
                // make sure to fit all content into the printable area when scaling
                // and the whole document page on the physical paper
                RectD rect = engine.PageContentBox(pageNo, Target_Print);
                geomutil::RectT<float> cbox = engine.Transform(rect, pageNo, 1.0, rotation).Convert<float>();
                zoom = std::min((float)printable.dx / cbox.dx,
                                std::min((float)printable.dy / cbox.dy,
                                         std::min((float)paperSize.dx / pSize.dx, (float)paperSize.dy / pSize.dy)));
                // use the correct zoom values, if the page fits otherwise
                // and the user didn't ask for anything else (default setting)
                if (PrintScaleAdv::Shrink == pd.advData.scale && dpiFactor < zoom)
                    zoom = dpiFactor;
                // center the page on the physical paper
                offset.x += (int)(paperSize.dx - pSize.dx * zoom) / 2;
                offset.y += (int)(paperSize.dy - pSize.dy * zoom) / 2;
                // make sure that no content lies in the non-printable paper margins
                geomutil::RectT<float> onPaper(printable.x + offset.x + cbox.x * zoom,
                                               printable.y + offset.y + cbox.y * zoom, cbox.dx * zoom, cbox.dy * zoom);
                if (onPaper.x < printable.x) {
                    offset.x += (int)(printable.x - onPaper.x);
                } else if (onPaper.BR().x > printable.BR().x) {
                    offset.x -= (int)(onPaper.BR().x - printable.BR().x);
                }
                if (onPaper.y < printable.y) {
                    offset.y += (int)(printable.y - onPaper.y);
                } else if (onPaper.BR().y > printable.BR().y) {
                    offset.y -= (int)(onPaper.BR().y - printable.BR().y);
                }
            }

            bool ok = false;
            short shrink = 1;
            do {
                RenderedBitmap* bmp = engine.RenderBitmap(pageNo, zoom / shrink, rotation, nullptr, Target_Print,
                                                          abortCookie ? &abortCookie->cookie : nullptr);
                if (abortCookie) {
                    abortCookie->Clear();
                }
                if (bmp && bmp->GetBitmap()) {
                    RectI rc(offset.x, offset.y, bmp->Size().dx * shrink, bmp->Size().dy * shrink);
                    ok = bmp->StretchDIBits(hdc, rc);
                }
                delete bmp;
                shrink *= 2;
            } while (!ok && shrink < 32 && !(progressUI && progressUI->WasCanceled()));
            // TODO: abort if !ok?

            if (EndPage(hdc) <= 0 || progressUI && progressUI->WasCanceled()) {
                AbortDoc(hdc);
                return false;
            }
            current++;
        }
    }

    EndDoc(hdc);
    return true;
}

class PrintThreadData : public ProgressUpdateUI {
    NotificationWnd* wnd = nullptr;
    AbortCookieManager cookie;
    bool isCanceled = false;
    WindowInfo* win = nullptr;

  public:
    PrintData* data = nullptr;
    HANDLE thread = nullptr; // close the print thread handle after execution

    PrintThreadData(WindowInfo* win, PrintData* data) {
        this->win = win;
        this->data = data;
        wnd = new NotificationWnd(win->hwndCanvas, L"", _TR("Printing page %d of %d..."),
                                  [this](NotificationWnd* wnd) { this->RemoveNotification(wnd); });
        // don't use a groupId for this notification so that
        // multiple printing notifications could coexist between tabs
        win->notifications->Add(wnd);
    }

    // called when printing has been canceled
    void RemoveNotification(NotificationWnd* wnd) {
        isCanceled = true;
        cookie.Abort();
        this->wnd = nullptr;
        if (WindowInfoStillValid(win)) {
            win->notifications->RemoveNotification(wnd);
        }
    }

    ~PrintThreadData() {
        CloseHandle(thread);
        delete data;
        RemoveNotification(wnd);
    }

    virtual void UpdateProgress(int current, int total) {
        uitask::Post([=] {
            if (WindowInfoStillValid(win) && win->notifications->Contains(wnd)) {
                wnd->UpdateProgress(current, total);
            }
        });
    }

    virtual bool WasCanceled() { return isCanceled || !WindowInfoStillValid(win) || win->printCanceled; }

    static DWORD WINAPI PrintThread(LPVOID data) {
        PrintThreadData* threadData = (PrintThreadData*)data;
        WindowInfo* win = threadData->win;
        // wait for PrintToDeviceOnThread to return so that we
        // close the correct handle to the current printing thread
        while (!win->printThread) {
            Sleep(1);
        }

        HANDLE thread = threadData->thread = win->printThread;
        PrintToDevice(*threadData->data, threadData, &threadData->cookie);

        uitask::Post([=] {
            if (WindowInfoStillValid(win) && thread == win->printThread) {
                win->printThread = nullptr;
            }
            delete threadData;
        });
        return 0;
    }
};

static void PrintToDeviceOnThread(WindowInfo* win, PrintData* data) {
    CrashIf(win->printThread);
    PrintThreadData* threadData = new PrintThreadData(win, data);
    win->printThread = nullptr;
    win->printThread = CreateThread(nullptr, 0, PrintThreadData::PrintThread, threadData, 0, nullptr);
}

void AbortPrinting(WindowInfo* win) {
    if (win->printThread) {
        win->printCanceled = true;
        WaitForSingleObject(win->printThread, INFINITE);
    }
    win->printCanceled = false;
}

static HGLOBAL GlobalMemDup(const void* data, size_t len) {
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hGlobal) {
        return nullptr;
    }

    void* globalData = GlobalLock(hGlobal);
    if (!globalData) {
        GlobalFree(hGlobal);
        return nullptr;
    }

    memcpy(globalData, data, len);
    GlobalUnlock(hGlobal);
    return hGlobal;
}

/* Show Print Dialog box to allow user to select the printer
and the pages to print.

For reference: In order to print with Adobe Reader instead: ViewWithAcrobat(win, L"/P");

Note: The following only applies for printing as image

Creates a new dummy page for each page with a large zoom factor,
and then uses StretchDIBits to copy this to the printer's dc.

So far have tested printing from XP to
 - Acrobat Professional 6 (note that acrobat is usually set to
   downgrade the resolution of its bitmaps to 150dpi)
 - HP Laserjet 2300d
 - HP Deskjet D4160
 - Lexmark Z515 inkjet, which should cover most bases.
*/
enum { MAXPAGERANGES = 10 };
void OnMenuPrint(WindowInfo* win, bool waitForCompletion) {
    // we remember some printer settings per process
    static ScopedMem<DEVMODE> defaultDevMode;
    static PrintScaleAdv defaultScaleAdv = PrintScaleAdv::Shrink;

    static bool hasDefaults = false;
    if (!hasDefaults) {
        hasDefaults = true;
        if (str::EqI(gGlobalPrefs->printerDefaults.printScale, "fit")) {
            defaultScaleAdv = PrintScaleAdv::Fit;
        } else if (str::EqI(gGlobalPrefs->printerDefaults.printScale, "none")) {
            defaultScaleAdv = PrintScaleAdv::None;
        }
    }

    bool printSelection = false;
    Vec<PRINTPAGERANGE> ranges;
    PRINTER_INFO_2 printerInfo = {0};

    if (!HasPermission(Perm_PrinterAccess)) {
        return;
    }
    if (!win->IsDocLoaded()) {
        return;
    }

    if (win->AsChm()) {
        // the Print dialog allows access to the file system, so fall back
        // to printing the entire document without dialog if that isn't desired
        bool showUI = HasPermission(Perm_DiskAccess);
        win->AsChm()->PrintCurrentPage(showUI);
        return;
    }
    if (win->AsEbook()) {
        // TODO: use EbookEngine for printing?
        return;
    }

    CrashIf(!win->AsFixed());
    if (!win->AsFixed()) {
        return;
    }
    DisplayModel* dm = win->AsFixed();

#ifndef DISABLE_DOCUMENT_RESTRICTIONS
    if (!dm->GetEngine()->AllowsPrinting()) {
        return;
    }
#endif

    if (win->printThread) {
        int res = MessageBox(win->hwndFrame, _TR("Printing is still in progress. Abort and start over?"),
                             _TR("Printing in progress."), MB_ICONEXCLAMATION | MB_YESNO | MbRtlReadingMaybe());
        if (res == IDNO) {
            return;
        }
    }
    AbortPrinting(win);

    // the Print dialog allows access to the file system, so fall back
    // to printing the entire document without dialog if that isn't desired
    if (!HasPermission(Perm_DiskAccess)) {
        PrintFile(dm->GetEngine());
        return;
    }

    PRINTDLGEX pd = {0};
    pd.lStructSize = sizeof(PRINTDLGEX);
    pd.hwndOwner = win->hwndFrame;
    pd.Flags = PD_USEDEVMODECOPIESANDCOLLATE | PD_COLLATE;
    if (!win->currentTab->selectionOnPage) {
        pd.Flags |= PD_NOSELECTION;
    }
    pd.nCopies = 1;
    /* by default print all pages */
    pd.nPageRanges = 1;
    pd.nMaxPageRanges = MAXPAGERANGES;
    PRINTPAGERANGE* ppr = AllocArray<PRINTPAGERANGE>(MAXPAGERANGES);
    pd.lpPageRanges = ppr;
    ppr->nFromPage = 1;
    ppr->nToPage = dm->PageCount();
    pd.nMinPage = 1;
    pd.nMaxPage = dm->PageCount();
    pd.nStartPage = START_PAGE_GENERAL;

    Print_Advanced_Data advanced(PrintRangeAdv::All, defaultScaleAdv);
    ScopedMem<DLGTEMPLATE> dlgTemplate; // needed for RTL languages
    HPROPSHEETPAGE hPsp = CreatePrintAdvancedPropSheet(&advanced, dlgTemplate);
    pd.lphPropertyPages = &hPsp;
    pd.nPropertyPages = 1;

    LPDEVNAMES devNames;
    LPDEVMODE devMode;
    bool failedEngineClone;
    PrintData* data = nullptr;

    // restore remembered settings
    if (defaultDevMode) {
        DEVMODE* p = defaultDevMode.Get();
        pd.hDevMode = GlobalMemDup(p, p->dmSize + p->dmDriverExtra);
    }

    if (PrintDlgEx(&pd) != S_OK) {
        if (CommDlgExtendedError() != 0) {
            /* if PrintDlg was cancelled then
               CommDlgExtendedError is zero, otherwise it returns the
               error code, which we could look at here if we wanted.
               for now just warn the user that printing has stopped
               becasue of an error */
            MessageBoxWarning(win->hwndFrame, _TR("Couldn't initialize printer"), _TR("Printing problem."));
        }
        goto Exit;
    }

    if (pd.dwResultAction == PD_RESULT_PRINT || pd.dwResultAction == PD_RESULT_APPLY) {
        // remember settings for this process
        devMode = (LPDEVMODE)GlobalLock(pd.hDevMode);
        if (devMode) {
            defaultDevMode.Set((LPDEVMODE)memdup(devMode, devMode->dmSize + devMode->dmDriverExtra));
            GlobalUnlock(pd.hDevMode);
        }
        defaultScaleAdv = advanced.scale;
    }

    if (pd.dwResultAction != PD_RESULT_PRINT) {
        goto Exit;
    }

    if (pd.Flags & PD_CURRENTPAGE) {
        PRINTPAGERANGE pr = {(DWORD)dm->CurrentPageNo(), (DWORD)dm->CurrentPageNo()};
        ranges.Append(pr);
    } else if (win->currentTab->selectionOnPage && (pd.Flags & PD_SELECTION)) {
        printSelection = true;
    } else if (!(pd.Flags & PD_PAGENUMS)) {
        PRINTPAGERANGE pr = {1, (DWORD)dm->PageCount()};
        ranges.Append(pr);
    } else {
        AssertCrash(pd.nPageRanges > 0);
        for (DWORD i = 0; i < pd.nPageRanges; i++) {
            ranges.Append(pd.lpPageRanges[i]);
        }
    }

    devNames = (LPDEVNAMES)GlobalLock(pd.hDevNames);
    devMode = (LPDEVMODE)GlobalLock(pd.hDevMode);
    if (devNames) {
        printerInfo.pDriverName = (LPWSTR)devNames + devNames->wDriverOffset;
        printerInfo.pPrinterName = (LPWSTR)devNames + devNames->wDeviceOffset;
        printerInfo.pPortName = (LPWSTR)devNames + devNames->wOutputOffset;
    }
    data = new PrintData(dm->GetEngine(), &printerInfo, devMode, ranges, advanced, dm->GetRotation(),
                         printSelection ? win->currentTab->selectionOnPage : nullptr);
    if (devNames) {
        GlobalUnlock(pd.hDevNames);
    }
    if (devMode) {
        GlobalUnlock(pd.hDevMode);
    }

    // if a file is missing and the engine can't thus be cloned,
    // we print using the original engine on the main thread
    // so that the document can't be closed and the original engine
    // unexpectedly deleted
    // TODO: instead prevent closing the document so that printing
    // can still happen on a separate thread and be interruptible
    failedEngineClone = dm->GetEngine() && !data->engine;
    if (failedEngineClone) {
        data->engine = dm->GetEngine();
    }

    if (!waitForCompletion && !failedEngineClone) {
        PrintToDeviceOnThread(win, data);
    } else {
        PrintToDevice(*data);
        if (failedEngineClone) {
            data->engine = nullptr;
        }
        delete data;
    }

Exit:
    free(ppr);
    GlobalFree(pd.hDevNames);
    GlobalFree(pd.hDevMode);
}

static short GetPaperSize(BaseEngine* engine) {
    RectD mediabox = engine->PageMediabox(1);
    SizeD size = engine->Transform(mediabox, 1, 1.0f / engine->GetFileDPI(), 0).Size();

    switch (GetPaperFormat(size)) {
        case Paper_A4:
            return DMPAPER_A4;
        case Paper_A3:
            return DMPAPER_A3;
        case Paper_A5:
            return DMPAPER_A5;
        case Paper_Letter:
            return DMPAPER_LETTER;
        case Paper_Legal:
            return DMPAPER_LEGAL;
        case Paper_Tabloid:
            return DMPAPER_TABLOID;
        case Paper_Statement:
            return DMPAPER_STATEMENT;
        default:
            return 0;
    }
}

static short GetPaperByName(const WCHAR* papername) {
    if (str::EqI(papername, L"letter")) {
        return DMPAPER_LETTER;
    }
    if (str::EqI(papername, L"legal")) {
        return DMPAPER_LEGAL;
    }
    if (str::EqI(papername, L"tabloid")) {
        return DMPAPER_TABLOID;
    }
    if (str::EqI(papername, L"statement")) {
        return DMPAPER_STATEMENT;
    }
    if (str::EqI(papername, L"A3")) {
        return DMPAPER_A3;
    }
    if (str::EqI(papername, L"A4")) {
        return DMPAPER_A4;
    }
    if (str::EqI(papername, L"A5")) {
        return DMPAPER_A5;
    }
    return 0;
}

static short GetPaperSourceByName(const WCHAR* printerName, const WCHAR* binName, LPDEVMODE devMode) {
    CrashIf(!(devMode->dmFields & DM_DEFAULTSOURCE));
    if (!(devMode->dmFields & DM_DEFAULTSOURCE)) {
        return devMode->dmDefaultSource;
    }
    DWORD count = DeviceCapabilities(printerName, nullptr, DC_BINS, nullptr, nullptr);
    DWORD count2 = DeviceCapabilities(printerName, nullptr, DC_BINNAMES, nullptr, nullptr);
    if (count != count2 || 0 == count || ((DWORD)-1 == count)) {
        return devMode->dmDefaultSource;
    }
    // try to determine the paper bin number by name
    ScopedMem<WORD> bins(AllocArray<WORD>(count));
    AutoFreeW binNames(AllocArray<WCHAR>(24 * count + 1));
    DeviceCapabilitiesW(printerName, nullptr, DC_BINS, (WCHAR*)bins.Get(), nullptr);
    DeviceCapabilitiesW(printerName, nullptr, DC_BINNAMES, binNames.Get(), nullptr);
    for (DWORD i = 0; i < count; i++) {
        const WCHAR* currName = binNames.Get() + 24 * i;
        if (str::EqIS(currName, binName)) {
            return bins.Get()[i];
        }
    }
    // alternatively allow indicating the paper bin directly by number
    if (str::Parse(binName, L"%u%$", &count)) {
        return (short)count;
    }
    return devMode->dmDefaultSource;
}

static void ApplyPrintSettings(const WCHAR* printerName, const WCHAR* settings, int pageCount,
                               Vec<PRINTPAGERANGE>& ranges, Print_Advanced_Data& advanced, LPDEVMODE devMode) {
    WStrVec rangeList;
    if (settings) {
        rangeList.Split(settings, L",", true);
    }

    for (size_t i = 0; i < rangeList.Count(); i++) {
        int val;
        PRINTPAGERANGE pr = {0};
        if (str::Parse(rangeList.At(i), L"%d-%d%$", &pr.nFromPage, &pr.nToPage)) {
            pr.nFromPage = limitValue(pr.nFromPage, (DWORD)1, (DWORD)pageCount);
            pr.nToPage = limitValue(pr.nToPage, (DWORD)1, (DWORD)pageCount);
            ranges.Append(pr);
        } else if (str::Parse(rangeList.At(i), L"%d%$", &pr.nFromPage)) {
            pr.nFromPage = pr.nToPage = limitValue(pr.nFromPage, (DWORD)1, (DWORD)pageCount);
            ranges.Append(pr);
        } else if (str::EqI(rangeList.At(i), L"even")) {
            advanced.range = PrintRangeAdv::Even;
        } else if (str::EqI(rangeList.At(i), L"odd")) {
            advanced.range = PrintRangeAdv::Odd;
        } else if (str::EqI(rangeList.At(i), L"noscale")) {
            advanced.scale = PrintScaleAdv::None;
        } else if (str::EqI(rangeList.At(i), L"shrink")) {
            advanced.scale = PrintScaleAdv::Shrink;
        } else if (str::EqI(rangeList.At(i), L"fit")) {
            advanced.scale = PrintScaleAdv::Fit;
        } else if (str::EqI(rangeList.At(i), L"portrait")) {
            advanced.rotation = PrintRotationAdv::Portrait;
        } else if (str::EqI(rangeList.At(i), L"landscape")) {
            advanced.rotation = PrintRotationAdv::Landscape;
        } else if (str::Parse(rangeList.At(i), L"%dx%$", &val) && 0 < val && val < 1000) {
            devMode->dmCopies = (short)val;
        } else if (str::EqI(rangeList.At(i), L"simplex")) {
            devMode->dmDuplex = DMDUP_SIMPLEX;
        } else if (str::EqI(rangeList.At(i), L"duplex") || str::EqI(rangeList.At(i), L"duplexlong")) {
            devMode->dmDuplex = DMDUP_VERTICAL;
        } else if (str::EqI(rangeList.At(i), L"duplexshort")) {
            devMode->dmDuplex = DMDUP_HORIZONTAL;
        } else if (str::EqI(rangeList.At(i), L"color")) {
            devMode->dmColor = DMCOLOR_COLOR;
        } else if (str::EqI(rangeList.At(i), L"monochrome")) {
            devMode->dmColor = DMCOLOR_MONOCHROME;
        } else if (str::StartsWithI(rangeList.At(i), L"bin=")) {
            devMode->dmDefaultSource = GetPaperSourceByName(printerName, rangeList.At(i) + 4, devMode);
        } else if (str::StartsWithI(rangeList.At(i), L"paper=")) {
            devMode->dmPaperSize = GetPaperByName(rangeList.At(i) + 6);
        }
    }

    if (ranges.Count() == 0) {
        PRINTPAGERANGE pr = {1, (DWORD)pageCount};
        ranges.Append(pr);
    }
}

bool PrintFile(BaseEngine* engine, WCHAR* printerName, bool displayErrors, const WCHAR* settings, WindowInfo* win) {
    bool ok = false;
    if (!HasPermission(Perm_PrinterAccess)) {
        return false;
    }

#ifndef DISABLE_DOCUMENT_RESTRICTIONS
    if (engine && !engine->AllowsPrinting()) {
        engine = nullptr;
    }
#endif
    if (!engine) {
        if (displayErrors) {
            MessageBoxWarning(nullptr, _TR("Cannot print this file"), _TR("Printing problem."));
        }
        return false;
    }

    AutoFreeW defaultPrinter;
    if (!printerName) {
        defaultPrinter.Set(GetDefaultPrinterName());
        printerName = defaultPrinter;
    }

    HANDLE printer;
    BOOL res = OpenPrinter(printerName, &printer, nullptr);
    if (!res) { 
		if (win) {
			OnMenuPrint(win);
		}
		else if (displayErrors) {
			MessageBoxWarning(nullptr, _TR("Printer with given name doesn't exist"), _TR("Printing problem."));
		}
        return false;
    }

    LONG returnCode = 0;
    LONG structSize = 0;
    LPDEVMODE devMode = nullptr;
    // get printer driver information
    DWORD needed = 0;
    GetPrinter(printer, 2, nullptr, 0, &needed);
    ScopedMem<PRINTER_INFO_2> infoData((PRINTER_INFO_2*)AllocArray<BYTE>(needed));
    if (infoData) {
        res = GetPrinter(printer, 2, (LPBYTE)infoData.Get(), needed, &needed);
    }
    if (!res || !infoData || needed <= sizeof(PRINTER_INFO_2)) {
        goto Exit;
    }

    structSize = DocumentProperties(nullptr, printer, printerName, nullptr, /* Asking for size, so */
                                    nullptr,                                /* not used. */
                                    0);                                     /* Zero returns buffer size. */
    if (structSize < sizeof(DEVMODE)) {
        // If failure, inform the user, cleanup and return failure.
        if (displayErrors) {
            MessageBoxWarning(nullptr, _TR("Could not obtain Printer properties"), _TR("Printing problem."));
        }
        goto Exit;
    }
    devMode = (LPDEVMODE)calloc(structSize, 1);

    // Get the default DevMode for the printer and modify it for your needs.
    returnCode = DocumentProperties(nullptr, printer, printerName, devMode, /* The address of the buffer to fill. */
                                    nullptr,                                /* Not using the input buffer. */
                                    DM_OUT_BUFFER);                         /* Have the output buffer filled. */
    if (IDOK != returnCode) {
        // If failure, inform the user, cleanup and return failure.
        if (displayErrors) {
            MessageBoxWarning(nullptr, _TR("Could not obtain Printer properties"), _TR("Printing problem."));
        }
        goto Exit;
    }

    ClosePrinter(printer);
    printer = nullptr;

    // set paper size to match the size of the document's first page
    // (will be overridden by any paper= value in -print-settings)
    devMode->dmPaperSize = GetPaperSize(engine);

    {
        Print_Advanced_Data advanced;
        Vec<PRINTPAGERANGE> ranges;

        ApplyPrintSettings(printerName, settings, engine->PageCount(), ranges, advanced, devMode);

		if (win) {
			PrintData* pd = new PrintData(engine, infoData, devMode, ranges, advanced);
			PrintToDeviceOnThread(win, pd);
		}
		else {
			PrintData pd(engine, infoData, devMode, ranges, advanced);
			ok = PrintToDevice(pd);
			if (!ok && displayErrors) {
				MessageBoxWarning(nullptr, _TR("Couldn't initialize printer"), _TR("Printing problem."));
			}
		}
    }

Exit:
    free(devMode);
    if (printer) {
        ClosePrinter(printer);
    }
    return ok;
}

bool PrintFile(const WCHAR* fileName, WCHAR* printerName, bool displayErrors, const WCHAR* settings) {
    AutoFreeW fileName2(path::Normalize(fileName));
    BaseEngine* engine = EngineManager::CreateEngine(fileName2);
    bool ok = PrintFile(engine, printerName, displayErrors, settings);
    delete engine;
    return ok;
}
