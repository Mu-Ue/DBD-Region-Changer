#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include "region_service.h"
#include "hosts_file.h"
#include "ui.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

HWND g_hList = nullptr, g_hBtnRefresh = nullptr, g_hBtnApply = nullptr;
static HWND g_hHeader = nullptr;
static HWND g_hotButton = nullptr; // owner-drawn button under the mouse
static HWND g_lastFocus = nullptr; // restored when the window is reactivated
static HFONT g_fontUI = nullptr, g_fontBold = nullptr, g_fontIcon = nullptr;
bool g_hasUnappliedChanges = false;
int g_spinnerFrame = 0;
static bool g_updatingList = false; // suppresses checkbox notifications while repopulating
static bool g_justApplied = false;  // shows the restart hint until the next change
static bool g_trackingList = false; // TME_LEAVE requested for the list
static int g_hoverRow = -1;
static RECT g_hintRect, g_statusRect, g_listFrame;
static UINT g_dpi = USER_DEFAULT_SCREEN_DPI;
static ULONG_PTR g_gdiplusToken = 0;

// Layout metrics at 96 DPI.
static const int ROW_HEIGHT    = 30;
static const int HEADER_HEIGHT = 32;
static const int CHECK_X       = 14; // checkbox inset from the row's left edge
static const int CHECK_SIZE    = 16;
static const int LABEL_X       = 40; // region name inset, shared by the header
static const int CELL_PAD      = 14; // right inset of the numeric columns

static const COLORREF CLR_BG             = RGB(0x1E, 0x1E, 0x1E);
static const COLORREF CLR_SURFACE        = RGB(0x25, 0x25, 0x26);
static const COLORREF CLR_HEADER         = RGB(0x2D, 0x2D, 0x30);
static const COLORREF CLR_ROW_HOVER      = RGB(0x2F, 0x31, 0x35);
static const COLORREF CLR_ROW_CHECKED    = RGB(0x1F, 0x2C, 0x36);
static const COLORREF CLR_ROW_CHECKED_HOVER = RGB(0x27, 0x37, 0x44);
static const COLORREF CLR_DIVIDER        = RGB(0x30, 0x30, 0x33);
static const COLORREF CLR_BORDER         = RGB(0x3F, 0x3F, 0x44);
static const COLORREF CLR_CHECK_BORDER   = RGB(0x80, 0x80, 0x86);
static const COLORREF CLR_TEXT           = RGB(0xEC, 0xEC, 0xEC);
static const COLORREF CLR_TEXT_DIM       = RGB(0xB8, 0xB8, 0xBC);
static const COLORREF CLR_HEADER_TEXT    = RGB(0xA6, 0xAC, 0xB4);
static const COLORREF CLR_MUTED          = RGB(0x84, 0x84, 0x8A);
static const COLORREF CLR_DISABLED       = RGB(0x70, 0x70, 0x74);
static const COLORREF CLR_ACCENT         = RGB(0x3D, 0xB5, 0xF2);
static const COLORREF CLR_ACCENT_HOT     = RGB(0x64, 0xC5, 0xF6);
static const COLORREF CLR_ACCENT_PRESSED = RGB(0x2A, 0x8C, 0xC2);
static const COLORREF CLR_ON_ACCENT      = RGB(0x0E, 0x16, 0x1C);
static const COLORREF CLR_BUTTON         = RGB(0x2D, 0x2D, 0x30);
static const COLORREF CLR_BUTTON_HOT     = RGB(0x3A, 0x3A, 0x3E);
static const COLORREF CLR_BUTTON_PRESSED = RGB(0x26, 0x26, 0x28);
static const COLORREF CLR_GOOD           = RGB(0x4E, 0xC9, 0x82);
static const COLORREF CLR_WARN           = RGB(0xF2, 0xC1, 0x4E);
static const COLORREF CLR_BAD            = RGB(0xFF, 0x6B, 0x6B);

// Converts a 96-DPI layout value to the window's current DPI.
static int Scale(int value) { return MulDiv(value, (int)g_dpi, USER_DEFAULT_SCREEN_DPI); }

static HFONT MakeFont(int pixels, int weight, const wchar_t* face) {
    return CreateFontW(-Scale(pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, face);
}

static void CreateFonts() {
    HFONT old[] = { g_fontUI, g_fontBold, g_fontIcon };
    g_fontUI = MakeFont(14, FW_NORMAL, L"Segoe UI");
    g_fontBold = MakeFont(14, FW_SEMIBOLD, L"Segoe UI");
    g_fontIcon = MakeFont(12, FW_NORMAL, L"Segoe MDL2 Assets");
    if (g_hList) SendMessage(g_hList, WM_SETFONT, (WPARAM)g_fontUI, TRUE); // also re-measures rows
    for (HFONT font : old) if (font) DeleteObject(font);
}

// --- GDI helpers -----------------------------------------------------------
static void Fill(HDC hdc, const RECT& rc, COLORREF color) {
    SetDCBrushColor(hdc, color);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void RoundBox(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(DC_PEN));
    SetDCBrushColor(hdc, fill);
    SetDCPenColor(hdc, border);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
}

static void DrawLabel(HDC hdc, const wchar_t* text, RECT rc, HFONT font, COLORREF color, UINT align) {
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, &rc, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
}

static Gdiplus::Color ToGdiplus(COLORREF color) {
    return Gdiplus::Color(GetRValue(color), GetGValue(color), GetBValue(color));
}

// Anti-aliased progress ring: a quarter arc rotating around a faint track.
static void DrawSpinner(HDC hdc, int cx, int cy, int radius, int frame) {
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::REAL width = (Gdiplus::REAL)Scale(2);
    Gdiplus::RectF box((Gdiplus::REAL)(cx - radius), (Gdiplus::REAL)(cy - radius),
                       (Gdiplus::REAL)(radius * 2), (Gdiplus::REAL)(radius * 2));
    Gdiplus::Pen track(ToGdiplus(CLR_BORDER), width);
    graphics.DrawEllipse(&track, box);
    Gdiplus::Pen arc(ToGdiplus(CLR_ACCENT), width);
    arc.SetStartCap(Gdiplus::LineCapRound);
    arc.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawArc(&arc, box, (Gdiplus::REAL)(frame * SPINNER_STEP_DEG % 360), 90.0f);
}

static bool FocusCuesHidden(HWND hwnd) {
    return (SendMessage(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
}

static int CheckedCount() {
    int count = 0;
    for (bool checked : g_unblockedRegions) if (checked) ++count;
    return count;
}

// --- Region list -----------------------------------------------------------
static void LayoutColumns(HWND hList) {
    RECT rc; GetClientRect(hList, &rc);
    int total = rc.right - rc.left; // already excludes the scrollbar if one is showing
    int killerW = Scale(80), survW = Scale(90), pingW = Scale(80);
    int regionW = total - killerW - survW - pingW;
    if (regionW < Scale(120)) regionW = Scale(120);
    ListView_SetColumnWidth(hList, 0, regionW);
    ListView_SetColumnWidth(hList, 1, killerW);
    ListView_SetColumnWidth(hList, 2, survW);
    ListView_SetColumnWidth(hList, 3, pingW);
}

static void FormatMetric(int value, bool isPing, wchar_t (&text)[24]) {
    if (value == METRIC_PENDING) wcscpy_s(text, L"\u2013");
    else if (value == METRIC_NA) wcscpy_s(text, L"N/A");
    else if (isPing) swprintf_s(text, L"%d ms", value);
    else swprintf_s(text, L"%d:%02d", value / 60, value % 60);
}

static COLORREF MetricColor(int value, bool isPing) {
    if (value >= METRIC_NA) return CLR_MUTED;
    if (!isPing) return CLR_TEXT;
    return value < 60 ? CLR_GOOD : value < 100 ? CLR_WARN : CLR_BAD;
}

// Writes a region's queue/ping cells (columns 1-3) for the given row. The
// rows are owner-drawn; the item text keeps the list readable by screen readers.
static void SetMetricCells(HWND hList, int row, const Region& region) {
    const int values[3] = { region.killerSecs, region.survivorSecs, region.pingMs };
    for (int col = 1; col <= 3; ++col) {
        wchar_t text[24];
        FormatMetric(values[col - 1], col == 3, text);
        ListView_SetItemText(hList, row, col, text);
    }
}

void PopulateList(HWND hList) {
    g_updatingList = true;
    LPARAM focusedRegion = -1;
    LVITEMW focused = {};
    focused.mask = LVIF_PARAM;
    focused.iItem = ListView_GetNextItem(hList, -1, LVNI_FOCUSED);
    if (focused.iItem >= 0 && ListView_GetItem(hList, &focused)) focusedRegion = focused.lParam;

    ListView_DeleteAllItems(hList);
    for (int row = 0; row < (int)g_order.size(); ++row) {
        int regionIdx = g_order[row];
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = (LPWSTR)g_regions[regionIdx].label.c_str();
        item.lParam = regionIdx;
        ListView_InsertItem(hList, &item);
        SetMetricCells(hList, row, g_regions[regionIdx]);
        ListView_SetCheckState(hList, row, g_unblockedRegions[regionIdx]);
        if (regionIdx == focusedRegion) {
            ListView_SetItemState(hList, row, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
        }
    }
    LayoutColumns(hList);
    g_updatingList = false;
}

// Update just one region's data cells wherever it currently sits (sorting
// may have moved it), without disturbing scroll position or selection.
void UpdateRegionRow(HWND hList, int regionIdx) {
    LVFINDINFOW fi = {};
    fi.flags = LVFI_PARAM;
    fi.lParam = regionIdx;
    int row = ListView_FindItem(hList, -1, &fi);
    if (row < 0) return;
    SetMetricCells(hList, row, g_regions[regionIdx]);
    ListView_RedrawItems(hList, row, row);
}

static void DrawListRow(const DRAWITEMSTRUCT* dis) {
    int idx = (int)dis->itemData; // the item's lParam: region index, independent of sort order
    if (idx < 0 || idx >= REGION_COUNT) return;
    const Region& region = g_regions[idx];
    bool checked = g_unblockedRegions[idx];
    bool hover = (int)dis->itemID == g_hoverRow;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    COLORREF rowBg = checked ? (hover ? CLR_ROW_CHECKED_HOVER : CLR_ROW_CHECKED)
                             : (hover ? CLR_ROW_HOVER : CLR_SURFACE);
    Fill(hdc, rc, rowBg);
    RECT divider = rc;
    divider.top = divider.bottom - 1;
    Fill(hdc, divider, CLR_DIVIDER);

    RECT box;
    box.left = rc.left + Scale(CHECK_X);
    box.top = rc.top + (rc.bottom - rc.top - Scale(CHECK_SIZE)) / 2;
    box.right = box.left + Scale(CHECK_SIZE);
    box.bottom = box.top + Scale(CHECK_SIZE);
    if (checked) {
        RoundBox(hdc, box, Scale(5), CLR_ACCENT, CLR_ACCENT);
        DrawLabel(hdc, L"\uE73E", box, g_fontIcon, CLR_ON_ACCENT, DT_CENTER); // CheckMark
    } else {
        RoundBox(hdc, box, Scale(5), rowBg, CLR_CHECK_BORDER);
    }

    int edge[5] = { rc.left };
    for (int col = 0; col < 4; ++col) edge[col + 1] = edge[col] + ListView_GetColumnWidth(g_hList, col);

    RECT label = { rc.left + Scale(LABEL_X), rc.top, edge[1] - Scale(8), rc.bottom };
    COLORREF labelColor = !region.online ? CLR_MUTED : checked ? CLR_TEXT : CLR_TEXT_DIM;
    DrawLabel(hdc, region.label.c_str(), label, checked ? g_fontBold : g_fontUI, labelColor, DT_LEFT);

    const int values[3] = { region.killerSecs, region.survivorSecs, region.pingMs };
    for (int col = 1; col <= 3; ++col) {
        wchar_t text[24];
        FormatMetric(values[col - 1], col == 3, text);
        RECT cell = { edge[col], rc.top, edge[col + 1] - Scale(CELL_PAD), rc.bottom };
        DrawLabel(hdc, text, cell, g_fontUI, MetricColor(values[col - 1], col == 3), DT_RIGHT);
    }

    if ((dis->itemState & ODS_FOCUS) && !FocusCuesHidden(g_hList)) {
        SetDCBrushColor(hdc, CLR_ACCENT);
        FrameRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    }
}

static void SetHoverRow(HWND hList, int row) {
    if (row == g_hoverRow) return;
    if (g_hoverRow >= 0) ListView_RedrawItems(hList, g_hoverRow, g_hoverRow);
    g_hoverRow = row;
    if (row >= 0) ListView_RedrawItems(hList, row, row);
}

static int RowAt(HWND hList, POINT pt) {
    LVHITTESTINFO hti = {};
    hti.pt = pt;
    return ListView_SubItemHitTest(hList, &hti);
}

static void ToggleRow(HWND hList, int row) {
    ListView_SetCheckState(hList, row, !ListView_GetCheckState(hList, row));
}

// A click anywhere on a row toggles it; Space toggles the focused row.
static LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: { // a quick second click is a double-click; it toggles too
        SetFocus(hwnd);
        int row = RowAt(hwnd, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        if (row >= 0) {
            ListView_SetItemState(hwnd, row, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ToggleRow(hwnd, row);
        }
        return 0; // no drag-select or marquee
    }
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            int row = ListView_GetNextItem(hwnd, -1, LVNI_FOCUSED);
            if (row >= 0) ToggleRow(hwnd, row);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == L' ') return 0; // handled on key down; don't start a type-ahead search
        break;
    case WM_MOUSEMOVE:
        if (!g_trackingList) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_trackingList = TrackMouseEvent(&tme) != FALSE;
        }
        SetHoverRow(hwnd, RowAt(hwnd, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }));
        break;
    case WM_MOUSELEAVE:
        g_trackingList = false;
        SetHoverRow(hwnd, -1);
        break;
    case WM_PAINT: {
        // The dark theme draws column separators in the empty space below
        // the last row; paint over them.
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        RECT empty, header, last;
        GetClientRect(hwnd, &empty);
        GetWindowRect(g_hHeader, &header);
        empty.top = header.bottom - header.top;
        int count = ListView_GetItemCount(hwnd);
        if (count > 0 && ListView_GetItemRect(hwnd, count - 1, &last, LVIR_BOUNDS) && last.bottom > empty.top)
            empty.top = last.bottom;
        if (empty.top < empty.bottom) {
            HDC hdc = GetDC(hwnd);
            Fill(hdc, empty, CLR_SURFACE);
            ReleaseDC(hwnd, hdc);
        }
        return result;
    }
    case WM_MOUSEWHEEL:
    case WM_VSCROLL: { // rows move under a still cursor
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetHoverRow(hwnd, g_trackingList ? RowAt(hwnd, pt) : -1);
        return result;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// --- Column header ---------------------------------------------------------
static void PaintHeader(HWND hwnd, HDC hdc) {
    static const wchar_t* titles[4] = { L"Region", L"Killer", L"Survivor", L"Ping" };
    RECT client;
    GetClientRect(hwnd, &client);
    Fill(hdc, client, CLR_HEADER);

    for (int col = 0; col < 4; ++col) {
        RECT item;
        if (SendMessage(hwnd, HDM_GETITEMRECT, col, (LPARAM)&item) == FALSE) continue;
        bool active = col == g_sortColumn;
        HGDIOBJ oldFont = SelectObject(hdc, g_fontBold);
        SIZE size;
        GetTextExtentPoint32W(hdc, titles[col], (int)wcslen(titles[col]), &size);
        SelectObject(hdc, oldFont);

        // Titles line up with their column's values: the region name on the
        // left, the numbers on the right. The sort arrow sits on the far side.
        RECT text = item, arrow = item;
        if (col == 0) {
            text.left = item.left + Scale(LABEL_X);
            text.right = text.left + size.cx;
            arrow.left = text.right + Scale(4);
        } else {
            text.right = item.right - Scale(CELL_PAD);
            text.left = text.right - size.cx;
            arrow.left = text.left - Scale(16);
        }
        arrow.right = arrow.left + Scale(12);
        DrawLabel(hdc, titles[col], text, g_fontBold, active ? CLR_TEXT : CLR_HEADER_TEXT, DT_LEFT);
        if (active) {
            DrawLabel(hdc, g_sortAscending ? L"\uE70E" : L"\uE70D", arrow, g_fontIcon, // ChevronUp/Down
                      CLR_ACCENT, DT_CENTER);
        }
    }
    RECT border = client;
    border.top = border.bottom - 1;
    Fill(hdc, border, CLR_BORDER);
}

// The HDS_NOSIZING style / HDN_BEGINTRACK cancel aren't consistently
// honored by the header control in all Common Controls versions, so we
// also swallow the mouse-down that would start a divider drag directly --
// this is what actually and reliably blocks column resizing.
static LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintHeader(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == HDM_LAYOUT) { // taller than the font alone would make it
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        HDLAYOUT* layout = (HDLAYOUT*)lParam;
        layout->pwpos->cy = Scale(HEADER_HEIGHT);
        layout->prc->top = layout->pwpos->y + Scale(HEADER_HEIGHT);
        return result;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        HDHITTESTINFO hti = {};
        hti.pt.x = GET_X_LPARAM(lParam);
        hti.pt.y = GET_Y_LPARAM(lParam);
        SendMessage(hwnd, HDM_HITTEST, 0, (LPARAM)&hti);
        if (hti.flags & (HHT_ONDIVIDER | HHT_ONDIVOPEN)) return 0; // eat it -- no drag, no auto-fit
        // A quick second click arrives as a double-click, which the header
        // doesn't report as a column click. Pass it on as a plain press so
        // every click sorts, however fast.
        if (msg == WM_LBUTTONDBLCLK) return DefSubclassProc(hwnd, WM_LBUTTONDOWN, wParam, lParam);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// --- Buttons and status ----------------------------------------------------
static void DrawButton(const DRAWITEMSTRUCT* dis, const wchar_t* text, bool primary) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool hot = !disabled && dis->hwndItem == g_hotButton;
    bool focus = (dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT);

    COLORREF fill, textColor;
    if (disabled) {
        fill = CLR_BUTTON;
        textColor = CLR_DISABLED;
    } else if (primary) {
        fill = pressed ? CLR_ACCENT_PRESSED : hot ? CLR_ACCENT_HOT : CLR_ACCENT;
        textColor = CLR_ON_ACCENT;
    } else {
        fill = pressed ? CLR_BUTTON_PRESSED : hot ? CLR_BUTTON_HOT : CLR_BUTTON;
        textColor = CLR_TEXT;
    }
    COLORREF border = focus ? CLR_TEXT : (primary && !disabled) ? fill : CLR_BORDER;

    Fill(hdc, rc, CLR_BG);
    RoundBox(hdc, rc, Scale(8), fill, border);
    DrawLabel(hdc, text, rc, g_fontBold, textColor, DT_CENTER);
}

static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hotButton != hwnd) {
        g_hotButton = hwnd;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE || (msg == WM_ENABLE && !wParam)) {
        if (g_hotButton == hwnd) g_hotButton = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void PaintStatus(HDC hdc) {
    int checked = CheckedCount();
    wchar_t text[128];
    COLORREF color = CLR_MUTED;
    if (g_refreshing && REGION_COUNT == 0) {
        wcscpy_s(text, L"Loading regions\u2026");
    } else if (g_refreshing) {
        int onlineCount = 0;
        for (const Region& region : g_regions) if (region.online) ++onlineCount;
        swprintf_s(text, L"Refreshing %d/%d\u2026", g_completedFetches, onlineCount);
    } else if (REGION_COUNT == 0) {
        wcscpy_s(text, L"Couldn't load regions. Check your connection and Refresh.");
        color = CLR_WARN;
    } else if (g_hasUnappliedChanges && checked == 0) {
        wcscpy_s(text, L"Check at least one region to apply.");
        color = CLR_WARN;
    } else if (g_hasUnappliedChanges) {
        swprintf_s(text, L"Unapplied changes \u00B7 %d of %d regions checked", checked, REGION_COUNT);
        color = CLR_ACCENT;
    } else if (g_justApplied) {
        wcscpy_s(text, L"\u2713 Applied \u2014 restart Dead by Daylight to take effect.");
        color = CLR_GOOD;
    } else if (checked == REGION_COUNT) {
        wcscpy_s(text, L"All regions allowed");
    } else {
        swprintf_s(text, L"%d of %d regions allowed", checked, REGION_COUNT);
    }

    // Drawn off-screen so the animated spinner does not flicker.
    RECT rc = { 0, 0, g_statusRect.right - g_statusRect.left, g_statusRect.bottom - g_statusRect.top };
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
    Fill(mem, rc, CLR_BG);
    if (g_refreshing) {
        int radius = Scale(6);
        DrawSpinner(mem, radius + Scale(1), rc.bottom / 2, radius, g_spinnerFrame);
        rc.left += radius * 2 + Scale(10);
    }
    DrawLabel(mem, text, rc, g_fontUI, color, DT_LEFT);
    BitBlt(hdc, g_statusRect.left, g_statusRect.top, g_statusRect.right - g_statusRect.left,
           rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

// Syncs the Apply button and the status line with the current selection.
void UpdateStatus() {
    bool canApply = g_hasUnappliedChanges && CheckedCount() > 0;
    if (!canApply && GetFocus() == g_hBtnApply) SetFocus(g_hList); // never strand focus on a disabled button
    EnableWindow(g_hBtnApply, canApply);
    InvalidateRect(g_hBtnApply, nullptr, FALSE);
    InvalidateRect(GetParent(g_hBtnApply), &g_statusRect, TRUE);
}

// Dark scrollbars need the process to opt into dark mode through uxtheme's
// unnamed exports: #135 SetPreferredAppMode and #133 AllowDarkModeForWindow.
static void UseDarkScrollbars(HWND hwnd) {
    HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int (WINAPI*)(int);
    using AllowDarkModeForWindowFn = BOOL (WINAPI*)(HWND, BOOL);
    auto setPreferredAppMode = (SetPreferredAppModeFn)(void*)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    auto allowDarkModeForWindow = (AllowDarkModeForWindowFn)(void*)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
    if (setPreferredAppMode) setPreferredAppMode(2); // ForceDark: the UI is always dark
    if (allowDarkModeForWindow) allowDarkModeForWindow(hwnd, TRUE);
}

// --- Main window -----------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);
        g_dpi = GetDpiForWindow(hwnd);
        CreateFonts();
        Gdiplus::GdiplusStartupInput gdiplusInput;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
        HINSTANCE instance = ((CREATESTRUCTW*)lParam)->hInstance;

        // Child windows are positioned by WM_SIZE, which follows WM_CREATE.
        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"Regions",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_OWNERDRAWFIXED,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), instance, nullptr);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
        UseDarkScrollbars(g_hList);
        SetWindowTheme(g_hList, L"DarkMode_Explorer", nullptr);
        SendMessage(g_hList, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        ListView_SetBkColor(g_hList, CLR_SURFACE);
        g_hHeader = ListView_GetHeader(g_hList);
        SetWindowTheme(g_hHeader, L"", L"");
        SetWindowLongPtrW(g_hHeader, GWL_STYLE,
            GetWindowLongPtrW(g_hHeader, GWL_STYLE) | HDS_NOSIZING | HDS_BUTTONS);
        SetWindowSubclass(g_hHeader, HeaderSubclassProc, 1, 0);
        SetWindowSubclass(g_hList, ListViewSubclassProc, 1, 0);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT;
        col.pszText = (LPWSTR)L"Region";   ListView_InsertColumn(g_hList, 0, &col);
        col.pszText = (LPWSTR)L"Killer";   ListView_InsertColumn(g_hList, 1, &col);
        col.pszText = (LPWSTR)L"Survivor"; ListView_InsertColumn(g_hList, 2, &col);
        col.pszText = (LPWSTR)L"Ping";     ListView_InsertColumn(g_hList, 3, &col);

        g_hBtnRefresh = CreateWindowW(L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), instance, nullptr);
        g_hBtnApply = CreateWindowW(L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_APPLY)), instance, nullptr);
        SetWindowSubclass(g_hBtnRefresh, ButtonSubclassProc, 1, 0);
        SetWindowSubclass(g_hBtnApply, ButtonSubclassProc, 1, 0);

        SortByColumn(0, false); // default: alphabetical by region

        DetectUnblockedRegions();
        g_originalUnblockedRegions = g_unblockedRegions; // baseline = what the hosts file holds
        PopulateList(g_hList);
        UpdateStatus();
        StartRefresh(hwnd);
        // Hide focus outlines until the keyboard is used, as dialogs do.
        SendMessage(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) g_lastFocus = GetFocus();
        else SetFocus(g_lastFocus && IsWindowEnabled(g_lastFocus) ? g_lastFocus : g_hList);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        Fill((HDC)wParam, rc, CLR_BG);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawLabel(hdc, L"Check the regions you want to play in, then click Apply.",
                  g_hintRect, g_fontUI, CLR_TEXT_DIM, DT_LEFT);
        SetDCBrushColor(hdc, CLR_BORDER);
        FrameRect(hdc, &g_listFrame, (HBRUSH)GetStockObject(DC_BRUSH));
        PaintStatus(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN: // background the owner-drawn buttons erase with
        SetDCBrushColor((HDC)wParam, CLR_BG);
        return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        info->ptMinTrackSize.x = Scale(460);
        info->ptMinTrackSize.y = Scale(400);
        return 0;
    }
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int margin = Scale(16);
        int textH = Scale(20);
        int buttonH = Scale(34);
        int buttonW = Scale(150);
        int buttonY = height - margin - buttonH;

        g_hintRect = { margin, margin, width - margin, margin + textH };
        g_statusRect = { margin, buttonY - Scale(10) - textH, width - margin, buttonY - Scale(10) };
        g_listFrame = { margin, g_hintRect.bottom + Scale(8), width - margin, g_statusRect.top - Scale(8) };
        if (g_hList) {
            MoveWindow(g_hList, g_listFrame.left + 1, g_listFrame.top + 1,
                       g_listFrame.right - g_listFrame.left - 2, g_listFrame.bottom - g_listFrame.top - 2, TRUE);
            LayoutColumns(g_hList);
        }
        if (g_hBtnRefresh) MoveWindow(g_hBtnRefresh, margin, buttonY, buttonW, buttonH, TRUE);
        if (g_hBtnApply) MoveWindow(g_hBtnApply, width - margin - buttonW, buttonY, buttonW, buttonH, TRUE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        CreateFonts();
        const RECT* suggested = (const RECT*)lParam;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE); // triggers WM_SIZE -> relayout
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lParam;
        if (mis->CtlType != ODT_LISTVIEW) break;
        mis->itemHeight = Scale(ROW_HEIGHT);
        return TRUE;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlID == ID_LIST) {
            DrawListRow(dis);
        } else if (dis->CtlID == ID_REFRESH) {
            DrawButton(dis, L"Refresh", false);
        } else if (dis->CtlID == ID_APPLY) {
            DrawButton(dis, L"Apply", true);
        } else {
            break;
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->hwndFrom == g_hHeader &&
            (nm->code == HDN_BEGINTRACKW || nm->code == HDN_DIVIDERDBLCLICKW)) {
            return TRUE; // block dragging a divider to resize, and double-click auto-fit
        }
        if (nm->idFrom == ID_LIST && nm->code == LVN_COLUMNCLICK) {
            NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
            SortByColumn(lv->iSubItem, true);
            PopulateList(g_hList);
            InvalidateRect(g_hHeader, nullptr, TRUE);
            return 0;
        }
        if (nm->idFrom == ID_LIST && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW* lv = (NMLISTVIEW*)lParam;
            bool checkboxChanged = ((lv->uOldState ^ lv->uNewState) & LVIS_STATEIMAGEMASK) != 0;
            if (!g_updatingList && (lv->uChanged & LVIF_STATE) && checkboxChanged && lv->iItem >= 0 &&
                lv->lParam >= 0 && lv->lParam < REGION_COUNT) {
                g_unblockedRegions[(int)lv->lParam] = ListView_GetCheckState(g_hList, lv->iItem) != FALSE;
                g_hasUnappliedChanges = g_unblockedRegions != g_originalUnblockedRegions;
                g_justApplied = false;
                ListView_RedrawItems(g_hList, lv->iItem, lv->iItem);
                UpdateStatus();
            }
        }
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == ID_REFRESH) {
            if (GetFocus() == g_hBtnRefresh) SetFocus(g_hList); // it is disabled while refreshing
            StartRefresh(hwnd);
        } else if (LOWORD(wParam) == ID_APPLY) {
            HCURSOR oldCursor = SetCursor(LoadCursor(nullptr, IDC_WAIT)); // includes a DNS flush
            std::wstring err;
            bool applied = ApplyRegions(g_unblockedRegions, err);
            SetCursor(oldCursor);
            if (applied) {
                g_originalUnblockedRegions = g_unblockedRegions;
                g_hasUnappliedChanges = false;
                g_justApplied = true;
                UpdateStatus();
            } else {
                MessageBoxW(hwnd, err.c_str(), L"DBD Region Changer", MB_ICONWARNING);
            }
        }
        return 0;
    }
    case WM_APP_REGIONS_DONE:
        OnRegionsDiscovered(hwnd, (RegionEntries*)lParam);
        return 0;
    case WM_APP_QUEUE_DONE:
        OnQueueDone(hwnd, (std::vector<QueueSnapshot>*)lParam);
        return 0;
    case WM_APP_PING_DONE:
        OnPingDone(hwnd, (int)wParam, (int)lParam);
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_SPINNER) {
            g_spinnerFrame++;
            InvalidateRect(hwnd, &g_statusRect, FALSE);
        }
        return 0;
    case WM_DESTROY:
        if (g_hBtnRefresh) RemoveWindowSubclass(g_hBtnRefresh, ButtonSubclassProc, 1);
        if (g_hBtnApply) RemoveWindowSubclass(g_hBtnApply, ButtonSubclassProc, 1);
        if (g_hHeader) RemoveWindowSubclass(g_hHeader, HeaderSubclassProc, 1);
        if (g_hList) RemoveWindowSubclass(g_hList, ListViewSubclassProc, 1);
        ShutdownRefresh();
        if (g_fontUI) DeleteObject(g_fontUI);
        if (g_fontBold) DeleteObject(g_fontBold);
        if (g_fontIcon) DeleteObject(g_fontIcon);
        if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
