#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include "region_service.h"
#include "hosts_file.h"
#include "ui.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

HWND g_hList = nullptr, g_hBtnRefresh = nullptr, g_hBtnApply = nullptr;
static HWND g_hHeader = nullptr;
static HFONT g_fontUI = nullptr, g_fontBold = nullptr;
bool g_hasUnappliedChanges = false;
int g_spinnerFrame = 0;
static bool g_updatingList = false; // suppresses checkbox notifications while repopulating
static bool g_applyFlash = false;
static UINT g_dpi = USER_DEFAULT_SCREEN_DPI;

// Converts a 96-DPI layout value to the window's current DPI.
static int Scale(int value) { return MulDiv(value, (int)g_dpi, USER_DEFAULT_SCREEN_DPI); }

static void CreateFonts() {
    HFONT oldUI = g_fontUI, oldBold = g_fontBold;
    g_fontUI = CreateFontW(Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontBold = CreateFontW(Scale(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_hList) SendMessage(g_hList, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
    if (oldUI) DeleteObject(oldUI);
    if (oldBold) DeleteObject(oldBold);
}
static const COLORREF CLR_BG        = RGB(0x1E, 0x1E, 0x1E);
static const COLORREF CLR_ACCENT    = RGB(0x3D, 0xB5, 0xF2);
static const COLORREF CLR_ACCENT_HI = RGB(0x2A, 0x8C, 0xC2);
static const COLORREF CLR_TEXT      = RGB(0xE8, 0xE8, 0xE8);
static const COLORREF CLR_WHITE     = RGB(0x25, 0x25, 0x26);
static const COLORREF CLR_BORDER    = RGB(0x45, 0x45, 0x45);
static const COLORREF CLR_ROW_ALT   = RGB(0x2B, 0x2D, 0x30);
static const COLORREF CLR_PING_GOOD = RGB(0x4E, 0xC9, 0x82);
static const COLORREF CLR_PING_WARN = RGB(0xF2, 0xC1, 0x4E);
static const COLORREF CLR_PING_BAD  = RGB(0xFF, 0x6B, 0x6B);
static const COLORREF CLR_MUTED     = RGB(0x9B, 0x9B, 0x9B);
static const COLORREF CLR_HEADER    = RGB(0x2D, 0x2F, 0x33);
static const COLORREF CLR_SELECTION = RGB(0x12, 0x3B, 0x55);
static const COLORREF CLR_HEADER_TEXT = RGB(0xC4, 0xCC, 0xD4);

static void ClearListSelection() {
    if (!g_hList) return;
    ListView_SetItemState(g_hList, -1, 0, LVIS_SELECTED);
    RedrawWindow(g_hList, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK ActionButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                                  LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_LBUTTONDOWN) ClearListSelection();
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
// Subclass for ListView to toggle check state on row click and clear selection highlight
static LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_LBUTTONDOWN) {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        LVHITTESTINFO hti{pt};
        int row = ListView_SubItemHitTest(hwnd, &hti);
        if (row >= 0) {
            BOOL checked = ListView_GetCheckState(hwnd, row);
            ListView_SetCheckState(hwnd, row, !checked);
            // remove any selection highlight
            ListView_SetItemState(hwnd, -1, 0, LVIS_SELECTED);
            return 0; // consume the click
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
static void LayoutColumns(HWND hList) {
    RECT rc; GetClientRect(hList, &rc);
    int total = rc.right - rc.left; // already excludes the scrollbar if one is showing
    int killerW = Scale(90), survW = Scale(90), pingW = Scale(80);
    int regionW = total - killerW - survW - pingW;
    if (regionW < Scale(120)) regionW = Scale(120);
    ListView_SetColumnWidth(hList, 0, regionW);
    ListView_SetColumnWidth(hList, 1, killerW);
    ListView_SetColumnWidth(hList, 2, survW);
    ListView_SetColumnWidth(hList, 3, pingW);
}

// Writes a region's queue/ping cells (columns 1-3) for the given row.
static void SetMetricCells(HWND hList, int row, const Region& region) {
    const int values[3] = { region.killerSecs, region.survivorSecs, region.pingMs };
    for (int col = 1; col <= 3; ++col) {
        int value = values[col - 1];
        wchar_t text[24];
        if (value == METRIC_PENDING) wcscpy_s(text, L"-");
        else if (value == METRIC_NA) wcscpy_s(text, L"N/A");
        else if (col == 3) swprintf_s(text, L"%dms", value);
        else swprintf_s(text, L"%d:%02d", value / 60, value % 60);
        ListView_SetItemText(hList, row, col, text);
    }
}

void PopulateList(HWND hList) {
    g_updatingList = true;
    std::vector<bool> selectedRegions(REGION_COUNT, false);
    int selectedRow = -1;
    while ((selectedRow = ListView_GetNextItem(hList, selectedRow, LVNI_SELECTED)) >= 0) {
        LVITEMW selected = {};
        selected.mask = LVIF_PARAM;
        selected.iItem = selectedRow;
        if (ListView_GetItem(hList, &selected) && selected.lParam >= 0 &&
            selected.lParam < REGION_COUNT) {
            selectedRegions[(int)selected.lParam] = true;
        }
    }
    ListView_DeleteAllItems(hList);
    for (int row = 0; row < (int)g_order.size(); ++row) {
        int regionIdx = g_order[row];
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        int displayRow = ListView_GetItemCount(hList);
        item.iItem = displayRow; item.iSubItem = 0;
        item.pszText = (LPWSTR)g_regions[regionIdx].label.c_str();
        item.lParam = regionIdx;
        ListView_InsertItem(hList, &item);
        SetMetricCells(hList, displayRow, g_regions[regionIdx]);
        ListView_SetCheckState(hList, displayRow, g_unblockedRegions[regionIdx]);
        if (selectedRegions[regionIdx]) {
            ListView_SetItemState(hList, displayRow, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    LayoutColumns(hList);
    g_updatingList = false;
}

// Update just one region's two data cells wherever it currently sits (sorting
// may have moved it), without disturbing scroll position or selection.
void UpdateRegionRow(HWND hList, int regionIdx) {
    LVFINDINFOW fi = {};
    fi.flags = LVFI_PARAM;
    fi.lParam = regionIdx;
    int row = ListView_FindItem(hList, -1, &fi);
    if (row < 0) return;
    SetMetricCells(hList, row, g_regions[regionIdx]);
    ListView_RedrawItems(hList, row, row); // ping color depends on the value, not just the text
}

// Owner-draw: flat rounded buttons.
static void DrawFlatButton(LPDRAWITEMSTRUCT dis, const wchar_t* text, bool primary) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH background = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, background);
    DeleteObject(background);

    COLORREF fill = primary ? (pressed ? CLR_ACCENT_HI : CLR_ACCENT) : CLR_WHITE;
    COLORREF txt  = primary ? CLR_WHITE : CLR_TEXT;

    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : CLR_BORDER);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, br);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, Scale(8), Scale(8));
    SelectObject(hdc, oldBr); SelectObject(hdc, oldPen);
    DeleteObject(br); DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txt);
    SelectObject(hdc, g_fontUI);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// The HDS_NOSIZING style / HDN_BEGINTRACK cancel aren't consistently
// honored by the header control in all Common Controls versions, so we
// also swallow the mouse-down that would start a divider drag directly --
// this is what actually and reliably blocks column resizing.
static void PaintHeaderItem(HDC hdc, const RECT& item, int col) {
    static const wchar_t* titles[4] = { L"Region", L"Killer", L"Survivor", L"Ping" };
    bool active = (col == g_sortColumn);
    if (active) {
        RECT indicator = item;
        indicator.left += 1;
        indicator.right -= 1;
        indicator.top = indicator.bottom - Scale(3);
        HBRUSH accent = CreateSolidBrush(CLR_ACCENT);
        FillRect(hdc, &indicator, accent);
        DeleteObject(accent);
    }

    wchar_t text[32];
    const wchar_t* arrow = active ? (g_sortAscending ? L"  \u25B2" : L"  \u25BC") : L"";
    swprintf_s(text, L"%s%s", titles[col], arrow);
    RECT textRect = item;
    textRect.left += Scale(10);
    textRect.right -= Scale(8);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, active ? CLR_ACCENT : CLR_HEADER_TEXT);
    SelectObject(hdc, g_fontBold);
    DrawTextW(hdc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void PaintModernHeader(HWND hwnd, HDC hdc) {
    RECT client;
    GetClientRect(hwnd, &client);
    HBRUSH background = CreateSolidBrush(CLR_HEADER);
    FillRect(hdc, &client, background);
    DeleteObject(background);

    for (int col = 0; col < 4; ++col) {
        RECT item;
        if (SendMessage(hwnd, HDM_GETITEMRECT, col, (LPARAM)&item) == FALSE) continue;
        PaintHeaderItem(hdc, item, col);
    }
}

static LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintModernHeader(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        HDHITTESTINFO hti = {};
        hti.pt.x = GET_X_LPARAM(lParam);
        hti.pt.y = GET_Y_LPARAM(lParam);
        SendMessage(hwnd, HDM_HITTEST, 0, (LPARAM)&hti);
        if (hti.flags & (HHT_ONDIVIDER | HHT_ONDIVOPEN)) return 0; // eat it -- no drag, no auto-fit
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        InitCommonControls();
        g_dpi = GetDpiForWindow(hwnd);
        CreateFonts();

        // Child windows are positioned by WM_SIZE, which follows WM_CREATE.
        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
        SetWindowTheme(g_hList, L"", L"");
        SendMessage(g_hList, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        ListView_SetBkColor(g_hList, CLR_WHITE);
        ListView_SetTextBkColor(g_hList, CLR_WHITE);
        g_hHeader = ListView_GetHeader(g_hList);
        SetWindowTheme(g_hHeader, L"", L"");
        SetWindowLongPtrW(g_hHeader, GWL_STYLE,
            GetWindowLongPtrW(g_hHeader, GWL_STYLE) | HDS_NOSIZING | HDS_BUTTONS);
        SetWindowSubclass(g_hHeader, HeaderSubclassProc, 1, 0);
        SetWindowSubclass(g_hList, ListViewSubclassProc, 0, 0);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)L"Region";   col.cx = 190; ListView_InsertColumn(g_hList, 0, &col);
        col.pszText = (LPWSTR)L"Killer";   col.cx = 90;  ListView_InsertColumn(g_hList, 1, &col);
        col.pszText = (LPWSTR)L"Survivor"; col.cx = 90;  ListView_InsertColumn(g_hList, 2, &col);
        col.pszText = (LPWSTR)L"Ping";     col.cx = 80;  ListView_InsertColumn(g_hList, 3, &col);

        g_hBtnRefresh = CreateWindowW(L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), GetModuleHandle(nullptr), nullptr);
        g_hBtnApply = CreateWindowW(L"BUTTON", L"Apply Checked Regions",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_APPLY)), GetModuleHandle(nullptr), nullptr);
        SetWindowSubclass(g_hBtnRefresh, ActionButtonSubclassProc, 1, 0);
        SetWindowSubclass(g_hBtnApply, ActionButtonSubclassProc, 1, 0);

        SortByColumn(0, false); // default: alphabetical by region (AP, EU, NA, SA...)

        DetectUnblockedRegions();
        g_originalUnblockedRegions = g_unblockedRegions; // baseline = what the hosts file holds
        PopulateList(g_hList);
        StartRefresh(hwnd);
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(CLR_BG);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        info->ptMinTrackSize.x = Scale(480);
        info->ptMinTrackSize.y = Scale(450);
        return 0;
    }
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int margin = Scale(12);
        int buttonH = Scale(34);
        int buttonY = height - margin - buttonH;
        int refreshW = Scale(140);
        if (g_hList) {
            MoveWindow(g_hList, margin, margin, width - 2 * margin, buttonY - 2 * margin, TRUE);
            LayoutColumns(g_hList);
        }
        if (g_hBtnRefresh) MoveWindow(g_hBtnRefresh, margin, buttonY, refreshW, buttonH, TRUE);
        if (g_hBtnApply) {
            int applyX = margin + refreshW + Scale(8);
            MoveWindow(g_hBtnApply, applyX, buttonY, width - applyX - margin, buttonH, TRUE);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        CreateFonts();
        const RECT* suggested = (const RECT*)lParam;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE); // triggers WM_SIZE -> relayout
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlID == ID_REFRESH) {
            wchar_t buf[32];
            if (g_refreshing) {
                int onlineCount = 0;
                for (const Region& region : g_regions) if (region.online) ++onlineCount;
                swprintf_s(buf, L"%s Refreshing %d/%d", SPINNER_FRAMES[g_spinnerFrame % 4],
                           g_completedFetches, onlineCount);
            }
            else wcscpy_s(buf, L"Refresh");
            DrawFlatButton(dis, buf, false);
            return TRUE;
        }
        if (dis->CtlID == ID_APPLY) {
            // Append " (Pending)" when unapplied changes exist
            const wchar_t* label = g_applyFlash ? L"\u2713 Applied" : L"Apply Checked Regions";
            wchar_t buf[64];
            if (g_hasUnappliedChanges) {
                swprintf_s(buf, L"%s (Pending)", label);
                DrawFlatButton(dis, buf, true);
            } else {
                DrawFlatButton(dis, label, true);
            }
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->hwndFrom == g_hHeader &&
            (nm->code == HDN_BEGINTRACKW || nm->code == HDN_DIVIDERDBLCLICKW)) {
            return TRUE; // block dragging a divider to resize, and double-click auto-fit
        }
        if (nm->hwndFrom == g_hHeader && nm->code == NM_CUSTOMDRAW) {
            LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)lParam;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                int col = (int)cd->dwItemSpec;
                RECT rc = cd->rc;

                HBRUSH bg = CreateSolidBrush(CLR_HEADER);
                FillRect(cd->hdc, &rc, bg);
                DeleteObject(bg);
                PaintHeaderItem(cd->hdc, rc, col);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
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
            if (!g_updatingList && (lv->uChanged & LVIF_STATE) && checkboxChanged && lv->iItem >= 0) {
                LVITEMW item = {};
                item.mask = LVIF_PARAM;
                item.iItem = lv->iItem;
                if (ListView_GetItem(g_hList, &item) && item.lParam >= 0 &&
                    item.lParam < REGION_COUNT) {
                    bool newState = ListView_GetCheckState(g_hList, lv->iItem) != FALSE;
                    g_unblockedRegions[(int)item.lParam] = newState;
                    g_hasUnappliedChanges = g_unblockedRegions != g_originalUnblockedRegions;
                    InvalidateRect(g_hBtnApply, nullptr, TRUE);
                }
            }
        }
        if (nm->idFrom == ID_LIST && nm->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                int idx = (int)cd->nmcd.lItemlParam; // actual region index, independent of sort order
                int row = (int)cd->nmcd.dwItemSpec;  // current visual row, for consistent striping after re-sort
                bool selected = (ListView_GetItemState(g_hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                bool isActive = idx >= 0 && idx < REGION_COUNT && g_unblockedRegions[idx];
                COLORREF rowBg = (row % 2 == 1) ? CLR_ROW_ALT : CLR_WHITE;

                if (isActive || selected) {
                    cd->clrTextBk = CLR_SELECTION;
                    cd->clrText = CLR_ACCENT;
                    SelectObject(cd->nmcd.hdc, g_fontBold);
                } else {
                    cd->clrText = CLR_TEXT;
                    cd->clrTextBk = rowBg;
                    SelectObject(cd->nmcd.hdc, g_fontUI);
                }
                return CDRF_NEWFONT | CDRF_NOTIFYSUBITEMDRAW;
            }
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                int idx = (int)cd->nmcd.lItemlParam;
                int row = (int)cd->nmcd.dwItemSpec;
                bool selected = (ListView_GetItemState(g_hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                bool isActive = idx >= 0 && idx < REGION_COUNT && g_unblockedRegions[idx];
                cd->clrTextBk = selected || isActive ? CLR_SELECTION :
                    ((row % 2 == 1) ? CLR_ROW_ALT : CLR_WHITE);
                SelectObject(cd->nmcd.hdc, selected || isActive ? g_fontBold : g_fontUI);
                if (idx >= 0 && idx < REGION_COUNT && cd->iSubItem == 3) {
                    // Always show ping color, even when selected
                    if (g_regions[idx].pingMs < 60) cd->clrText = CLR_PING_GOOD;
                    else if (g_regions[idx].pingMs < 100) cd->clrText = CLR_PING_WARN;
                    else if (g_regions[idx].pingMs < METRIC_NA) cd->clrText = CLR_PING_BAD;
                    else cd->clrText = CLR_MUTED;
                } else if (selected || isActive) {
                    cd->clrText = CLR_ACCENT;
                }
                return CDRF_NEWFONT;
            }
            }
        }
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == ID_REFRESH) {
            StartRefresh(hwnd);
        } else if (LOWORD(wParam) == ID_APPLY) {
            std::wstring err;
            if (ApplyRegions(g_unblockedRegions, err)) {
                g_applyFlash = true;
                g_originalUnblockedRegions = g_unblockedRegions;
                g_hasUnappliedChanges = false;
                InvalidateRect(g_hBtnApply, nullptr, TRUE);
                SetTimer(hwnd, TIMER_APPLY_FLASH, 1200, nullptr);
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
    case WM_TIMER: {
        if (wParam == TIMER_SPINNER) {
            g_spinnerFrame++;
            InvalidateRect(g_hBtnRefresh, nullptr, TRUE);
        } else if (wParam == TIMER_APPLY_FLASH) {
            KillTimer(hwnd, TIMER_APPLY_FLASH);
            g_applyFlash = false;
            InvalidateRect(g_hBtnApply, nullptr, TRUE);
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_hBtnRefresh) RemoveWindowSubclass(g_hBtnRefresh, ActionButtonSubclassProc, 1);
        if (g_hBtnApply) RemoveWindowSubclass(g_hBtnApply, ActionButtonSubclassProc, 1);
        if (g_hHeader) RemoveWindowSubclass(g_hHeader, HeaderSubclassProc, 1);
        ShutdownRefresh();
        if (g_fontUI) DeleteObject(g_fontUI);
        if (g_fontBold) DeleteObject(g_fontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
