#pragma once
#include <windows.h>
#include <climits>
#include <string>
#include <vector>
struct Region {
    std::string code;   // owns its data -- safe to keep across BuildRegions() rebuilds
    std::wstring label; // ditto
    bool online;
    wchar_t killer[24];
    wchar_t survivor[24];
    wchar_t ping[16] = L"-";
    int killerSecs = INT_MAX;
    int survivorSecs = INT_MAX;
    int pingMs = INT_MAX;
};
struct QueueSnapshot {
    wchar_t killer[24] = L"N/A";
    wchar_t survivor[24] = L"N/A";
    int killerSecs = INT_MAX;
    int survivorSecs = INT_MAX;
};
struct QueueFetchBatch {
    LONG generation;
    std::vector<QueueSnapshot> snapshots;
};
struct FetchResult {
    int idx;
    LONG generation;
    bool pingStage = false;
    wchar_t killer[24] = L"N/A";
    wchar_t survivor[24] = L"N/A";
    wchar_t ping[16] = L"N/A";
    int killerSecs = INT_MAX;
    int survivorSecs = INT_MAX;
    int pingMs = INT_MAX;
};
constexpr UINT WM_APP_REGION_DONE = WM_APP + 1;
constexpr UINT WM_APP_WORKER_DONE = WM_APP + 2;
constexpr UINT WM_APP_QUEUE_DONE = WM_APP + 3;
extern std::vector<Region> g_regions;
extern int REGION_COUNT;
extern LONG g_pendingFetches, g_completedFetches, g_activeWorkers;
extern bool g_refreshing, g_wsaStarted, g_updatingList, g_applyFlash;
extern int g_spinnerFrame;
extern HWND g_hList, g_hBtnRefresh, g_hBtnApply, g_hwndMain, g_hHeader;
extern HFONT g_fontUI, g_fontBold;
extern std::vector<bool> g_unblockedRegions;
extern HANDLE g_cancelEvent;
extern std::vector<HANDLE> g_workerHandles;
extern int g_workerCount;
extern LONG g_nextQueueRegion, g_nextPingRegion, g_refreshGeneration;
extern std::vector<int> g_order;
extern int g_sortColumn;
extern bool g_sortAscending;
void InitializeRegions();
void StartRefresh(HWND hwnd);
void RefreshRegionStatus(HWND hList);
void SortByColumn(int col, bool toggle);
void PopulateList(HWND hList);
void UpdateRegionRow(HWND hList, int regionIdx);
void CloseWorkerHandles();




