#pragma once
#include <windows.h>

inline constexpr UINT   ID_REFRESH        = 1002;
inline constexpr UINT   ID_APPLY          = 1003;
inline constexpr UINT   ID_LIST           = 1001;
inline constexpr UINT_PTR TIMER_SPINNER     = 1;
inline constexpr UINT_PTR TIMER_APPLY_FLASH = 2;

// Spinner display frames
inline constexpr const wchar_t* SPINNER_FRAMES[] = { L"◐", L"◓", L"◑", L"◒" };

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void PopulateList(HWND hList);
void UpdateRegionRow(HWND hList, int regionIdx);
extern bool g_hasUnappliedChanges;
extern int g_spinnerFrame;
extern HWND g_hList, g_hBtnRefresh, g_hBtnApply;
