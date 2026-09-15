#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <winsock2.h>
#include <climits>
#include <string>
#include <vector>
#include "region_service.h"
#include "hosts_file.h"
#include "ui.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
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
    int killerW = 90, survW = 90, pingW = 80;
    int regionW = total - killerW - survW - pingW;
    if (regionW < 120) regionW = 120;
    ListView_SetColumnWidth(hList, 0, regionW);
    ListView_SetColumnWidth(hList, 1, killerW);
    ListView_SetColumnWidth(hList, 2, survW);
    ListView_SetColumnWidth(hList, 3, pingW);
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
        ListView_SetItemText(hList, displayRow, 1, g_regions[regionIdx].killer);
        ListView_SetItemText(hList, displayRow, 2, g_regions[regionIdx].survivor);
        ListView_SetItemText(hList, displayRow, 3, g_regions[regionIdx].ping);
        ListView_SetCheckState(hList, displayRow, g_unblockedRegions[regionIdx]);
        if (selectedRegions[regionIdx]) {
            ListView_SetItemState(hList, displayRow, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    LayoutColumns(hList);
    g_updatingList = false;
    // Record the original blocked/unblocked state as the baseline for pending changes
    g_originalUnblockedRegions = g_unblockedRegions;
}

// Helper to determine if any change differs from the original state
static bool HasPendingChanges() {
    if (g_originalUnblockedRegions.size() != g_unblockedRegions.size()) return true;
    for (size_t i = 0; i < g_originalUnblockedRegions.size(); ++i) {
        if (g_originalUnblockedRegions[i] != g_unblockedRegions[i]) return true;
    }
    return false;
}

// Update just one region's two data cells wherever it currently sits (sorting
// may have moved it), without disturbing scroll position or selection.
void UpdateRegionRow(HWND hList, int regionIdx) {
    LVFINDINFOW fi = {};
    fi.flags = LVFI_PARAM;
    fi.lParam = regionIdx;
    int row = ListView_FindItem(hList, -1, &fi);
    if (row < 0) return;
    ListView_SetItemText(hList, row, 1, g_regions[regionIdx].killer);
    ListView_SetItemText(hList, row, 2, g_regions[regionIdx].survivor);
    ListView_SetItemText(hList, row, 3, g_regions[regionIdx].ping);
    
    // Force redraw to ensure colors are applied
    InvalidateRect(hList, nullptr, TRUE);
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
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
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
        indicator.top = indicator.bottom - 3;
        HBRUSH accent = CreateSolidBrush(CLR_ACCENT);
        FillRect(hdc, &indicator, accent);
        DeleteObject(accent);
    }

    wchar_t text[32];
    const wchar_t* arrow = active ? (g_sortAscending ? L"  \u25B2" : L"  \u25BC") : L"";
    swprintf_s(text, L"%s%s", titles[col], arrow);
    RECT textRect = item;
    textRect.left += 10;
    textRect.right -= 8;
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
        g_fontUI = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fontBold = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        RECT client; GetClientRect(hwnd, &client);
        int w = client.right - client.left;

        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            12, 12, w - 24, 340, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), GetModuleHandle(nullptr), nullptr);
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
            12, 364, 140, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), GetModuleHandle(nullptr), nullptr);
        g_hBtnApply = CreateWindowW(L"BUTTON", L"Apply Checked Regions",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            160, 364, 200, 34, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_APPLY)), GetModuleHandle(nullptr), nullptr);
        SetWindowSubclass(g_hBtnRefresh, ActionButtonSubclassProc, 1, 0);
        SetWindowSubclass(g_hBtnApply, ActionButtonSubclassProc, 1, 0);

        g_order.resize(REGION_COUNT);
        for (int i = 0; i < REGION_COUNT; ++i) g_order[i] = i;
        SortByColumn(0, false); // default: alphabetical by region (AP, EU, NA, SA...)

        DetectUnblockedRegions();
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
        info->ptMinTrackSize.x = 480;
        info->ptMinTrackSize.y = 450;
        return 0;
    }
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int buttonY = height - 46;
        int listHeight = buttonY - 24;
        if (g_hList) {
            MoveWindow(g_hList, 12, 12, width - 24, listHeight, TRUE);
            LayoutColumns(g_hList);
        }
        if (g_hBtnRefresh) MoveWindow(g_hBtnRefresh, 12, buttonY, 140, 34, TRUE);
        if (g_hBtnApply) MoveWindow(g_hBtnApply, 160, buttonY, width - 172, 34, TRUE);
        return 0;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlID == ID_REFRESH) {
            wchar_t buf[32];
            if (g_refreshing) {
                LONG completed = InterlockedCompareExchange(&g_completedFetches, 0, 0);
                LONG onlineCount = 0;
                for (const Region& region : g_regions) if (region.online) ++onlineCount;
                swprintf_s(buf, L"%s Refreshing %ld/%ld", SPINNER_FRAMES[g_spinnerFrame % 4], completed, onlineCount);
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
        if (nm->idFrom == ID_LIST && nm->code == NM_CLICK) {
            NMITEMACTIVATE* click = (NMITEMACTIVATE*)lParam;
            LVHITTESTINFO hit = {};
            hit.pt = click->ptAction;
            int row = ListView_SubItemHitTest(g_hList, &hit);
            if (row >= 0 && (hit.flags & LVHT_ONITEMSTATEICON)) {
                ListView_SetItemState(g_hList, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(g_hList, row, LVIS_SELECTED, LVIS_SELECTED);
            }
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
                        // Determine whether there are any pending changes compared to the original snapshot
                        bool pending = HasPendingChanges();
                        // Update the global flag accordingly
                        g_hasUnappliedChanges = pending;
                        // Update the Apply button to reflect changed state
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
                    else if (g_regions[idx].pingMs != INT_MAX) cd->clrText = CLR_PING_BAD;
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
                g_hasUnappliedChanges = false; // Clear the unapplied changes flag
                InvalidateRect(g_hBtnApply, nullptr, TRUE);
                SetTimer(hwnd, TIMER_APPLY_FLASH, 1200, nullptr);
            } else {
                MessageBoxW(hwnd, err.c_str(), L"DBD Region Changer", MB_ICONWARNING);
            }
        }
        return 0;
    }
    case WM_APP_QUEUE_DONE: {
        QueueFetchBatch* batch = (QueueFetchBatch*)lParam;
        if (!batch) return 0;
        LONG currentGeneration = InterlockedCompareExchange(&g_refreshGeneration, 0, 0);
        if (batch->generation != currentGeneration ||
            (int)batch->snapshots.size() != REGION_COUNT) {
            delete batch;
            return 0;
        }
        for (int idx = 0; idx < REGION_COUNT; ++idx) {
            if (g_regions[idx].online) {
                wcscpy_s(g_regions[idx].killer, _countof(g_regions[idx].killer),
                         batch->snapshots[idx].killer);
                wcscpy_s(g_regions[idx].survivor, _countof(g_regions[idx].survivor),
                         batch->snapshots[idx].survivor);
                g_regions[idx].killerSecs = batch->snapshots[idx].killerSecs;
                g_regions[idx].survivorSecs = batch->snapshots[idx].survivorSecs;
            } else {
                wcscpy_s(g_regions[idx].killer, _countof(g_regions[idx].killer), L"-");
                wcscpy_s(g_regions[idx].survivor, _countof(g_regions[idx].survivor), L"-");
            }
            UpdateRegionRow(g_hList, idx);
        }
        delete batch;
        return 0;
    }
    case WM_APP_REGION_DONE: {
        FetchResult* result = (FetchResult*)lParam;
        if (!result) return 0;
        LONG currentGeneration = InterlockedCompareExchange(&g_refreshGeneration, 0, 0);
        if (result->generation != currentGeneration) {
            delete result;
            return 0;
        }
        int idx = result->idx;
        if (result->pingStage) {
            wcscpy_s(g_regions[idx].ping, 16, result->ping);
            g_regions[idx].pingMs = result->pingMs;
        } else {
            wcscpy_s(g_regions[idx].killer, 24, result->killer);
            wcscpy_s(g_regions[idx].survivor, 24, result->survivor);
            g_regions[idx].killerSecs = result->killerSecs;
            g_regions[idx].survivorSecs = result->survivorSecs;
        }
        delete result;
        UpdateRegionRow(g_hList, idx);
        if (InterlockedCompareExchange(&g_pendingFetches, 0, 0) <= 0) {
            g_refreshing = false;
            if (InterlockedCompareExchange(&g_activeWorkers, 0, 0) == 0) {
                CloseWorkerHandles();
                if (g_wsaStarted) {
                    WSACleanup();
                    g_wsaStarted = false;
                }
            }
            KillTimer(hwnd, TIMER_SPINNER);
            EnableWindow(g_hBtnRefresh, TRUE);
            InvalidateRect(g_hBtnRefresh, nullptr, TRUE);
        }
        return 0;
    }
    case WM_APP_WORKER_DONE:
        if (!g_refreshing && InterlockedCompareExchange(&g_activeWorkers, 0, 0) == 0) {
            CloseWorkerHandles();
            if (g_wsaStarted) {
                WSACleanup();
                g_wsaStarted = false;
            }
        }
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
        if (g_cancelEvent) {
            SetEvent(g_cancelEvent);
            for (int i = 0; i < g_workerCount; ++i) {
                if (g_workerHandles[i]) WaitForSingleObject(g_workerHandles[i], INFINITE);
            }
            CloseWorkerHandles();
            CloseHandle(g_cancelEvent);
            g_cancelEvent = nullptr;
        }
        if (g_wsaStarted) {
            WSACleanup();
            g_wsaStarted = false;
        }
        if (g_fontUI) DeleteObject(g_fontUI);
        if (g_fontBold) DeleteObject(g_fontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
