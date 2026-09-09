#pragma once
#include <windows.h>
#include <vector>
#include <string>

// Shared constants (consolidated to avoid duplicate definitions across TUs)
inline constexpr UINT   ID_REFRESH        = 1002;
inline constexpr UINT   ID_APPLY          = 1003;
inline constexpr UINT   ID_LIST           = 1001;
inline constexpr UINT_PTR TIMER_SPINNER     = 1;
inline constexpr UINT_PTR TIMER_APPLY_FLASH = 2;
inline constexpr int    QUEUE_WORKER_COUNT  = 1;

// Spinner display frames
inline constexpr const wchar_t* SPINNER_FRAMES[] = { L"\u25D0", L"\u25D3", L"\u25D1", L"\u25D2" };

// Main callbacks / externs
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
extern std::vector<bool> g_unblockedRegions;
extern std::vector<bool> g_originalUnblockedRegions;
extern bool g_updatingList, g_applyFlash;
extern bool g_hasUnappliedChanges; // Tracks unapplied changes
extern HWND g_hList, g_hBtnRefresh, g_hBtnApply, g_hwndMain;
extern HFONT g_fontUI, g_fontBold;
