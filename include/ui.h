#pragma once
#include <windows.h>
#include <vector>
#include <string>
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
extern std::vector<bool> g_unblockedRegions;
extern std::vector<bool> g_originalUnblockedRegions;
extern bool g_updatingList, g_applyFlash;
extern bool g_hasUnappliedChanges; // Tracks unapplied changes
extern HWND g_hList, g_hBtnRefresh, g_hBtnApply, g_hwndMain;
extern HFONT g_fontUI, g_fontBold;
