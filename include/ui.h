#pragma once
#include <windows.h>

inline constexpr UINT   ID_REFRESH        = 1002;
inline constexpr UINT   ID_APPLY          = 1003;
inline constexpr UINT   ID_LIST           = 1001;
inline constexpr UINT_PTR TIMER_SPINNER = 1;

// Spinner animation: ~30 fps, one turn per second
inline constexpr UINT SPINNER_INTERVAL_MS = 33;
inline constexpr int  SPINNER_STEP_DEG    = 12;

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void PopulateList(HWND hList);
void UpdateRegionRow(HWND hList, int regionIdx);
void UpdateStatus();
int ClientHeightForRows(int rows, UINT dpi);
extern bool g_hasUnappliedChanges;
extern int g_spinnerFrame;
extern HWND g_hList, g_hBtnRefresh, g_hBtnApply;
