#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <vector>
#include "region_service.h"
#include "hosts_file.h"
#include "ui.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

HWND g_hList = nullptr, g_hBtnRefresh = nullptr, g_hBtnApply = nullptr;
static HWND g_hotButton = nullptr; // owner-drawn button under the mouse
static HWND g_lastFocus = nullptr; // restored when the window is reactivated
static HFONT g_fontUI = nullptr, g_fontBold = nullptr, g_fontIcon = nullptr;
bool g_hasUnappliedChanges = false;
int g_spinnerFrame = 0;
static bool g_justApplied = false;  // shows the restart hint until the next change
static bool g_trackingList = false; // TME_LEAVE requested for the list
static int g_hoverRow = -1;
static RECT g_hintRect, g_statusRect, g_listFrame;
static UINT g_dpi = USER_DEFAULT_SCREEN_DPI;
static ULONG_PTR g_gdiplusToken = 0;

// Layout metrics at 96 DPI.
static const int ROW_HEIGHT    = 30;
static const int HEADER_HEIGHT = 32;
// Every column is laid out the same way: | pad | lane | gap | text | pad |.
// The lane holds the checkboxes in the region column and the sort arrow in
// the header, so the region arrow sits right above the checkboxes.
static const int CELL_PAD      = 14; // inset at both ends of every column
static const int LANE_SIZE     = 16; // checkbox / sort arrow lane
static const int LANE_GAP      = 6;  // lane to column text
static const int LABEL_X       = CELL_PAD + LANE_SIZE + LANE_GAP; // region name inset

static const wchar_t* const COLUMN_TITLES[4] = { L"Region", L"Killer", L"Survivor", L"Ping" };

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
// A custom list rather than a ListView: a ListView scrolls by whole rows, which
// leaves a gap under the last row whenever the height isn't a multiple of it.
// This one scrolls by pixels, so the last row always ends flush at the bottom.
// The header stays fixed above the rows, which scroll beneath it.
static int g_scrollY = 0;      // pixels the rows are scrolled up by
static int g_focusRegion = -1; // region index of the keyboard-focused row
static int g_colEdge[5] = {};  // column x boundaries

static int HeaderHeight() { return Scale(HEADER_HEIGHT); }
static int RowHeight() { return Scale(ROW_HEIGHT); }
static int RowCount() { return (int)g_order.size(); }

static int ViewHeight(HWND hList) {
    RECT rc; GetClientRect(hList, &rc);
    return (std::max)(0, (int)rc.bottom - HeaderHeight());
}

static int MaxScroll(HWND hList) {
    return (std::max)(0, RowCount() * RowHeight() - ViewHeight(hList));
}

static int RowOfRegion(int regionIdx) {
    for (int row = 0; row < RowCount(); ++row) if (g_order[row] == regionIdx) return row;
    return -1;
}

static RECT RowRect(HWND hList, int row) {
    RECT rc; GetClientRect(hList, &rc);
    rc.top = HeaderHeight() + row * RowHeight() - g_scrollY;
    rc.bottom = rc.top + RowHeight();
    return rc;
}

static void InvalidateRow(HWND hList, int row) {
    if (row < 0) return;
    RECT rc = RowRect(hList, row);
    InvalidateRect(hList, &rc, FALSE);
}

static int RowAt(HWND hList, POINT pt) {
    RECT rc; GetClientRect(hList, &rc);
    if (!PtInRect(&rc, pt) || pt.y < HeaderHeight()) return -1;
    int row = (pt.y - HeaderHeight() + g_scrollY) / RowHeight();
    return row < RowCount() ? row : -1;
}

static void SetHoverRow(HWND hList, int row) {
    if (row == g_hoverRow) return;
    InvalidateRow(hList, g_hoverRow);
    g_hoverRow = row;
    InvalidateRow(hList, row);
}

// Rows move under a still cursor when the list scrolls or re-sorts.
static void RefreshHover(HWND hList) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hList, &pt);
    SetHoverRow(hList, g_trackingList ? RowAt(hList, pt) : -1);
}

static void ScrollListTo(HWND hList, int y) {
    y = (std::max)(0, (std::min)(y, MaxScroll(hList)));
    if (y == g_scrollY) return;
    g_scrollY = y;
    SetScrollPos(hList, SB_VERT, y, TRUE);
    InvalidateRect(hList, nullptr, FALSE);
    RefreshHover(hList);
}

static void EnsureRowVisible(HWND hList, int row) {
    int top = row * RowHeight();
    int view = ViewHeight(hList);
    if (top < g_scrollY) ScrollListTo(hList, top);
    else if (top + RowHeight() > g_scrollY + view) ScrollListTo(hList, top + RowHeight() - view);
}

static void FocusRow(HWND hList, int row) {
    if (row < 0 || row >= RowCount()) return;
    InvalidateRow(hList, RowOfRegion(g_focusRegion));
    g_focusRegion = g_order[row];
    InvalidateRow(hList, row);
    EnsureRowVisible(hList, row);
}

static void ToggleRegion(HWND hList, int regionIdx) {
    g_unblockedRegions[regionIdx] = !g_unblockedRegions[regionIdx];
    g_hasUnappliedChanges = g_unblockedRegions != g_originalUnblockedRegions;
    g_justApplied = false;
    InvalidateRow(hList, RowOfRegion(regionIdx));
    UpdateStatus();
}

// Recomputes the columns and the scroll range after a resize or a row change.
static void LayoutList(HWND hList) {
    RECT rc; GetClientRect(hList, &rc);
    int total = rc.right - rc.left; // already excludes the scrollbar if one is showing
    // The numeric columns share one width that fits the widest title.
    int titleW = 0;
    HDC hdc = GetDC(hList);
    HGDIOBJ oldFont = SelectObject(hdc, g_fontBold);
    for (int col = 1; col < 4; ++col) {
        SIZE size;
        GetTextExtentPoint32W(hdc, COLUMN_TITLES[col], (int)wcslen(COLUMN_TITLES[col]), &size);
        if (size.cx > titleW) titleW = size.cx;
    }
    SelectObject(hdc, oldFont);
    ReleaseDC(hList, hdc);
    int metricW = Scale(LABEL_X + CELL_PAD) + titleW;
    g_colEdge[1] = (std::max)(total - 3 * metricW, Scale(120));
    for (int col = 2; col <= 4; ++col) g_colEdge[col] = g_colEdge[col - 1] + metricW;

    // Growing the list pulls the rows down rather than opening a gap below them.
    g_scrollY = (std::min)(g_scrollY, MaxScroll(hList));
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMax = (std::max)(0, RowCount() * RowHeight() - 1);
    si.nPage = (UINT)ViewHeight(hList);
    si.nPos = g_scrollY;
    SetScrollInfo(hList, SB_VERT, &si, TRUE); // may toggle the scrollbar, re-entering via WM_SIZE
    InvalidateRect(hList, nullptr, FALSE);
    RefreshHover(hList);
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

// Call after the rows were added or re-sorted.
void PopulateList(HWND hList) {
    LayoutList(hList);
}

// Redraws one region's row wherever it currently sits (sorting may have moved it).
void UpdateRegionRow(HWND hList, int regionIdx) {
    InvalidateRow(hList, RowOfRegion(regionIdx));
}

static void DrawListRow(HWND hList, HDC hdc, RECT rc, int row) {
    int idx = g_order[row];
    if (idx < 0 || idx >= REGION_COUNT) return;
    const Region& region = g_regions[idx];
    bool checked = g_unblockedRegions[idx];
    bool hover = row == g_hoverRow;

    COLORREF rowBg = checked ? (hover ? CLR_ROW_CHECKED_HOVER : CLR_ROW_CHECKED)
                             : (hover ? CLR_ROW_HOVER : CLR_SURFACE);
    Fill(hdc, rc, rowBg);
    RECT divider = rc;
    divider.top = divider.bottom - 1;
    Fill(hdc, divider, CLR_DIVIDER);

    RECT box;
    box.left = rc.left + Scale(CELL_PAD);
    box.top = rc.top + (rc.bottom - rc.top - Scale(LANE_SIZE)) / 2;
    box.right = box.left + Scale(LANE_SIZE);
    box.bottom = box.top + Scale(LANE_SIZE);
    if (checked) {
        RoundBox(hdc, box, Scale(5), CLR_ACCENT, CLR_ACCENT);
        DrawLabel(hdc, L"\uE73E", box, g_fontIcon, CLR_ON_ACCENT, DT_CENTER); // CheckMark
    } else {
        RoundBox(hdc, box, Scale(5), rowBg, CLR_CHECK_BORDER);
    }

    RECT label = { rc.left + Scale(LABEL_X), rc.top, g_colEdge[1] - Scale(CELL_PAD), rc.bottom };
    COLORREF labelColor = !region.online ? CLR_MUTED : checked ? CLR_TEXT : CLR_TEXT_DIM;
    DrawLabel(hdc, region.label.c_str(), label, checked ? g_fontBold : g_fontUI, labelColor, DT_LEFT);

    const int values[3] = { region.killerSecs, region.survivorSecs, region.pingMs };
    for (int col = 1; col <= 3; ++col) {
        wchar_t text[24];
        FormatMetric(values[col - 1], col == 3, text);
        RECT cell = { g_colEdge[col], rc.top, g_colEdge[col + 1] - Scale(CELL_PAD), rc.bottom };
        DrawLabel(hdc, text, cell, g_fontUI, MetricColor(values[col - 1], col == 3), DT_RIGHT);
    }

    if (idx == g_focusRegion && GetFocus() == hList && !FocusCuesHidden(hList)) {
        SetDCBrushColor(hdc, CLR_ACCENT);
        FrameRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    }
}

static void PaintHeader(HDC hdc, RECT header) {
    Fill(hdc, header, CLR_HEADER);
    for (int col = 0; col < 4; ++col) {
        RECT item = { g_colEdge[col], header.top, g_colEdge[col + 1], header.bottom };
        bool active = col == g_sortColumn;
        HGDIOBJ oldFont = SelectObject(hdc, g_fontBold);
        SIZE size;
        GetTextExtentPoint32W(hdc, COLUMN_TITLES[col], (int)wcslen(COLUMN_TITLES[col]), &size);
        SelectObject(hdc, oldFont);

        // Titles line up with their column's values: the region name on the
        // left, the numbers on the right. The sort arrow always leads the
        // title in the lane, LANE_GAP away, so it never pushes a title out of
        // line with its values.
        RECT text = item, arrow = item;
        if (col == 0) {
            text.left = item.left + Scale(LABEL_X);
            text.right = text.left + size.cx;
        } else {
            text.right = item.right - Scale(CELL_PAD);
            text.left = text.right - size.cx;
        }
        arrow.right = text.left - Scale(LANE_GAP);
        arrow.left = arrow.right - Scale(LANE_SIZE);
        DrawLabel(hdc, COLUMN_TITLES[col], text, g_fontBold, active ? CLR_TEXT : CLR_HEADER_TEXT, DT_LEFT);
        if (active) {
            DrawLabel(hdc, g_sortAscending ? L"\uE70E" : L"\uE70D", arrow, g_fontIcon, // ChevronUp/Down
                      CLR_ACCENT, DT_CENTER);
        }
    }
    RECT border = header;
    border.top = border.bottom - 1;
    Fill(hdc, border, CLR_BORDER);
}

// Drawn off-screen so scrolling and hovering do not flicker.
static void PaintList(HWND hList) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hList, &ps);
    RECT client; GetClientRect(hList, &client);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
    Fill(mem, client, CLR_SURFACE);
    for (int row = g_scrollY / RowHeight(); row < RowCount(); ++row) {
        RECT rc = RowRect(hList, row);
        if (rc.top >= client.bottom) break;
        DrawListRow(hList, mem, rc, row);
    }
    PaintHeader(mem, { 0, 0, client.right, HeaderHeight() }); // covers rows scrolled under it
    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
    EndPaint(hList, &ps);
}

// Arrow, Page and Home/End keys move the focused row; Space toggles it.
static void OnListKey(HWND hList, WPARAM key) {
    int row = RowOfRegion(g_focusRegion);
    int page = (std::max)(1, ViewHeight(hList) / RowHeight());
    switch (key) {
    case VK_UP:    row = (std::max)(0, row - 1); break;
    case VK_DOWN:  row = row + 1; break;
    case VK_PRIOR: row = (std::max)(0, row - page); break;
    case VK_NEXT:  row = (std::min)(RowCount() - 1, (std::max)(0, row) + page); break;
    case VK_HOME:  row = 0; break;
    case VK_END:   row = RowCount() - 1; break;
    case VK_SPACE: if (row >= 0) ToggleRegion(hList, g_focusRegion); break;
    default: return;
    }
    SendMessage(hList, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    FocusRow(hList, row);
}

// A click on a column title sorts by it; a click anywhere on a row toggles it.
// The class has no CS_DBLCLKS, so rapid clicks all arrive as single presses.
static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SIZE:
        LayoutList(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintList(hwnd);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRow(hwnd, RowOfRegion(g_focusRegion));
        return 0;
    case WM_UPDATEUISTATE: // focus cues shown or hidden
        InvalidateRow(hwnd, RowOfRegion(g_focusRegion));
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (pt.y < HeaderHeight()) {
            for (int col = 0; col < 4; ++col) {
                if (pt.x >= g_colEdge[col] && pt.x < g_colEdge[col + 1]) {
                    SortByColumn(col, true);
                    PopulateList(hwnd);
                    break;
                }
            }
        } else if (int row = RowAt(hwnd, pt); row >= 0) {
            FocusRow(hwnd, row);
            ToggleRegion(hwnd, g_order[row]);
        }
        return 0;
    }
    case WM_KEYDOWN:
        OnListKey(hwnd, wParam);
        return 0;
    case WM_MOUSEMOVE:
        if (!g_trackingList) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_trackingList = TrackMouseEvent(&tme) != FALSE;
        }
        SetHoverRow(hwnd, RowAt(hwnd, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }));
        return 0;
    case WM_MOUSELEAVE:
        g_trackingList = false;
        SetHoverRow(hwnd, -1);
        return 0;
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        int step = lines == WHEEL_PAGESCROLL ? ViewHeight(hwnd) : (int)lines * RowHeight();
        ScrollListTo(hwnd, g_scrollY - MulDiv(GET_WHEEL_DELTA_WPARAM(wParam), step, WHEEL_DELTA));
        return 0;
    }
    case WM_VSCROLL: {
        int y = g_scrollY;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:   y -= RowHeight(); break;
        case SB_LINEDOWN: y += RowHeight(); break;
        case SB_PAGEUP:   y -= ViewHeight(hwnd); break;
        case SB_PAGEDOWN: y += ViewHeight(hwnd); break;
        case SB_TOP:      y = 0; break;
        case SB_BOTTOM:   y = MaxScroll(hwnd); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si = { sizeof(si), SIF_TRACKPOS };
            GetScrollInfo(hwnd, SB_VERT, &si);
            y = si.nTrackPos;
            break;
        }
        }
        ScrollListTo(hwnd, y);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
        g_dpi = GetDpiForWindow(hwnd);
        CreateFonts();
        Gdiplus::GdiplusStartupInput gdiplusInput;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);
        HINSTANCE instance = ((CREATESTRUCTW*)lParam)->hInstance;

        WNDCLASSEXW listClass = { sizeof(listClass) };
        listClass.lpfnWndProc = ListProc;
        listClass.hInstance = instance;
        listClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        listClass.lpszClassName = L"DBDRegionList";
        RegisterClassExW(&listClass);

        // Child windows are positioned by WM_SIZE, which follows WM_CREATE.
        g_hList = CreateWindowExW(0, L"DBDRegionList", L"Regions",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), instance, nullptr);
        UseDarkScrollbars(g_hList);
        SetWindowTheme(g_hList, L"DarkMode_Explorer", nullptr);

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
        info->ptMinTrackSize.x = Scale(540);
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
        LayoutList(g_hList); // row and header heights changed even if the size did not
        return 0;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlID == ID_REFRESH) {
            DrawButton(dis, L"Refresh", false);
        } else if (dis->CtlID == ID_APPLY) {
            DrawButton(dis, L"Apply", true);
        } else {
            break;
        }
        return TRUE;
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
