#pragma once
#include <windows.h>
#include <climits>
#include <string>
#include <vector>
// Metric sentinels. Both sort after every real value.
constexpr int METRIC_NA      = INT_MAX - 1; // fetched, but no value available
constexpr int METRIC_PENDING = INT_MAX;     // not fetched yet, or region offline
struct Region {
    std::string code;   // owns its data -- safe to keep across BuildRegions() rebuilds
    std::wstring label; // ditto
    bool online;
    int killerSecs = METRIC_PENDING;
    int survivorSecs = METRIC_PENDING;
    int pingMs = METRIC_PENDING;
};
struct QueueSnapshot {
    int killerSecs = METRIC_NA;
    int survivorSecs = METRIC_NA;
};
using RegionEntries = std::vector<std::pair<std::string, bool>>; // code, online

// Posted by background work to the main window.
constexpr UINT WM_APP_REGIONS_DONE = WM_APP + 1; // lParam: RegionEntries* or null
constexpr UINT WM_APP_QUEUE_DONE   = WM_APP + 2; // lParam: std::vector<QueueSnapshot>* or null
constexpr UINT WM_APP_PING_DONE    = WM_APP + 3; // wParam: region index, lParam: ms or -1
extern std::vector<Region> g_regions;
extern int REGION_COUNT;
extern int g_pendingFetches, g_completedFetches;
extern bool g_refreshing, g_updatingList, g_applyFlash;
extern int g_spinnerFrame;
extern HWND g_hList, g_hBtnRefresh, g_hBtnApply, g_hwndMain, g_hHeader;
extern HFONT g_fontUI, g_fontBold;
extern std::vector<bool> g_unblockedRegions;
extern std::vector<int> g_order;
extern int g_sortColumn;
extern bool g_sortAscending;
void InitializeRegions();
void StartRefresh(HWND hwnd);
void OnRegionsDiscovered(HWND hwnd, RegionEntries* entries);
void OnQueueDone(HWND hwnd, std::vector<QueueSnapshot>* snapshots);
void OnPingDone(HWND hwnd, int idx, int rtt);
void ShutdownRefresh();
void SortByColumn(int col, bool toggle);
void PopulateList(HWND hList);
void UpdateRegionRow(HWND hList, int regionIdx);




