#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wininet.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <climits>
#include "region_service.h"
#include "hosts_file.h"
#include "ui.h"
std::vector<Region> g_regions;
int REGION_COUNT = 0;

static const size_t MAX_HTTP_BODY = 64 * 1024;
static const wchar_t* QUEUES_URL = L"https://api2.deadbyqueue.com/queues";
static const wchar_t* REGIONS_URL = L"https://api2.deadbyqueue.com/regions";
static const wchar_t* REGION_CACHE_NAME = L"regions.cache";

std::vector<bool> g_unblockedRegions;
std::vector<bool> g_originalUnblockedRegions;
std::vector<int> g_order;
int g_sortColumn = 0;
bool g_sortAscending = true;
bool g_refreshing = false;
int g_completedFetches = 0;        // UI thread only: pings finished, for the status line
static int g_pendingFetches = 0;   // UI thread only: outstanding discovery/queue/ping results

// Background work runs on the system thread pool. Work items receive copies
// of everything they need and only report back via PostMessage, so the UI
// thread can rebuild g_regions without racing them.
static PTP_CLEANUP_GROUP g_workGroup = nullptr;
static TP_CALLBACK_ENVIRON g_workEnv;
static HINTERNET g_internet = nullptr; // closing it aborts in-flight requests
static std::atomic<bool> g_shuttingDown{ false };

// ---------------------------------------------------------------------------
static std::wstring ToWide(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// ---------------------------------------------------------------------------
// HTTP GET via WinINet on the shared session. Returns "" on any failure.
// ---------------------------------------------------------------------------
static std::string HttpGet(const wchar_t* url) {
    std::string result;
    if (!g_internet || g_shuttingDown) return result;
    HINTERNET hUrl = InternetOpenUrlW(g_internet, url, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT |
        INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_SECURE, 0);
    if (hUrl) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        bool validStatus = HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                          &status, &statusSize, nullptr) && status >= 200 && status < 300;
        bool validLength = !HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                           &contentLength, &contentLengthSize, nullptr) ||
                   contentLength <= MAX_HTTP_BODY;
        if (validStatus && validLength) {
            char buf[4096]; DWORD bytesRead = 0;
            while (result.size() < MAX_HTTP_BODY &&
                   InternetReadFile(hUrl, buf,
                       (DWORD)(std::min)(MAX_HTTP_BODY - result.size(), sizeof(buf)), &bytesRead) &&
                   bytesRead > 0)
                result.append(buf, bytesRead);
            if (result.size() >= MAX_HTTP_BODY) result.clear();
        }
        InternetCloseHandle(hUrl);
    }
    return result;
}

static const wchar_t* RegionLabel(const std::string& code) {
    struct Label { const char* code; const wchar_t* label; };
    static const Label labels[] = {
        { "us-east-1", L"Virginia" }, { "us-east-2", L"Ohio" },
        { "us-west-1", L"Los Angeles" }, { "us-west-2", L"Oregon" },
        { "ca-central-1", L"Canada" }, { "eu-central-1", L"Frankfurt" },
        { "eu-west-1", L"Dublin" }, { "eu-west-2", L"London" },
        { "sa-east-1", L"Brazil" }, { "ap-south-1", L"India" },
        { "ap-east-1", L"Hong Kong" }, { "ap-northeast-1", L"Tokyo" },
        { "ap-northeast-2", L"Seoul" }, { "ap-southeast-1", L"Singapore" },
        { "ap-southeast-2", L"Sydney" }
    };
    for (const Label& entry : labels) if (code == entry.code) return entry.label;
    return nullptr;
}

static bool IsRegionCode(const std::string& code) {
    if (code.empty() || code.size() > 63) return false;
    for (unsigned char ch : code) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
    }
    return true;
}

// Wide GetModuleFileNameW so install paths containing non-ASCII characters
// (e.g. a non-English Windows username) resolve correctly. Returns a wide
// path -- std::ifstream/ofstream take a wchar_t* path directly under MSVC's
// STL (a supported, if non-portable, extension), so no lossy narrowing
// through the ANSI codepage happens along the way.
static std::wstring RegionCachePath() {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return REGION_CACHE_NAME;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return std::wstring(path) + REGION_CACHE_NAME;
}

static void ResetRegionMetrics(Region& region) {
    region.killerSecs = METRIC_PENDING;
    region.survivorSecs = METRIC_PENDING;
    region.pingMs = METRIC_PENDING;
}

static void BuildRegions(const RegionEntries& entries) {
    g_regions.clear();
    g_regions.reserve(entries.size());
    for (const auto& entry : entries) {
        Region region{};
        region.code = entry.first;
        const wchar_t* knownLabel = RegionLabel(entry.first);
        region.label = knownLabel ? knownLabel : ToWide(entry.first.c_str());
        if (!entry.second) region.label += L" (Offline)";
        region.online = entry.second;
        ResetRegionMetrics(region);
        g_regions.push_back(std::move(region));
    }
    REGION_COUNT = (int)g_regions.size();
    g_order.resize(REGION_COUNT);
    for (int i = 0; i < REGION_COUNT; ++i) g_order[i] = i;
}

static bool LoadRegionCache(RegionEntries& entries) {
    std::ifstream file(RegionCachePath(), std::ios::binary);
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab + 2 != line.size() ||
            (line[tab + 1] != '0' && line[tab + 1] != '1') ||
            !IsRegionCode(line.substr(0, tab))) {
            entries.clear();
            return false;
        }
        entries.emplace_back(line.substr(0, tab), line[tab + 1] == '1');
    }
    return !file.bad() && !entries.empty() && entries.size() <= 128;
}

static void SaveRegionCache(const RegionEntries& entries) {
    std::ofstream file(RegionCachePath(), std::ios::binary | std::ios::trunc);
    if (!file) return;
    for (const auto& entry : entries) file << entry.first << '\t' << (entry.second ? '1' : '0') << '\n';
}

static bool DiscoverRegions(RegionEntries& entries) {
    std::string body = HttpGet(REGIONS_URL);
    size_t regionsPos = body.find("\"regions\"");
    size_t cursor = regionsPos == std::string::npos ? std::string::npos : body.find('{', regionsPos);
    if (cursor == std::string::npos) return false;
    entries.clear();
    while (++cursor < body.size() && body[cursor] != '}') {
        if (body[cursor] != '"') continue;
        size_t end = body.find('"', cursor + 1);
        size_t colon = end == std::string::npos ? std::string::npos : body.find(':', end + 1);
        if (end == std::string::npos || colon == std::string::npos) return false;
        size_t value = body.find_first_not_of(" \t\r\n", colon + 1);
        if (value == std::string::npos) return false;
        std::string code = body.substr(cursor + 1, end - cursor - 1);
        bool online = body.compare(value, 4, "true") == 0;
        if ((!online && body.compare(value, 5, "false") != 0) || !IsRegionCode(code)) return false;
        entries.emplace_back(std::move(code), online);
        cursor = value;
    }
    return !entries.empty() && entries.size() <= 128;
}

// Startup uses the cached region list so the window appears immediately;
// the first refresh then discovers the live list in the background.
void InitializeRegions() {
    RegionEntries entries;
    LoadRegionCache(entries);
    BuildRegions(entries);
}

// Finds `"key": { ... }` inside [begin, end) and narrows the range to that
// object's braces, so later lookups cannot run into a sibling object.
static bool FindObject(const std::string& body, const std::string& key, size_t& begin, size_t& end) {
    const std::string quoted = "\"" + key + "\"";
    for (size_t pos = body.find(quoted, begin); pos < end; pos = body.find(quoted, pos + 1)) {
        size_t colon = body.find_first_not_of(" \t\r\n", pos + quoted.size());
        if (colon >= end || body[colon] != ':') continue; // a value, not a key
        size_t brace = body.find_first_not_of(" \t\r\n", colon + 1);
        if (brace >= end || body[brace] != '{') continue;
        int depth = 0;
        for (size_t i = brace; i < end; ++i) {
            if (body[i] == '{') ++depth;
            else if (body[i] == '}' && --depth == 0) {
                begin = brace;
                end = i + 1;
                return true;
            }
        }
        return false;
    }
    return false;
}

// Reads `"<region>": { "<role>": { "time": "<seconds>" } }`.
static int ExtractQueueSeconds(const std::string& body, const std::string& regionCode,
                               const char* role) {
    size_t begin = 0, end = body.size();
    if (!FindObject(body, regionCode, begin, end) || !FindObject(body, role, begin, end))
        return METRIC_NA;
    size_t timePos = body.find("\"time\"", begin);
    size_t colon = timePos < end ? body.find_first_not_of(" \t\r\n", timePos + 6) : std::string::npos;
    if (colon >= end || body[colon] != ':') return METRIC_NA;
    size_t quote = body.find_first_not_of(" \t\r\n", colon + 1);
    if (quote >= end || body[quote] != '"') return METRIC_NA;
    size_t close = body.find('"', quote + 1);
    if (close >= end || close == quote + 1 || close - quote - 1 > 9) return METRIC_NA;
    int seconds = 0;
    for (size_t i = quote + 1; i < close; ++i) {
        if (body[i] < '0' || body[i] > '9') return METRIC_NA;
        seconds = seconds * 10 + (body[i] - '0');
    }
    return seconds;
}

static bool ParseQueueResponse(const std::string& body, const RegionEntries& regions,
                               std::vector<QueueSnapshot>& snapshots) {
    if (body.empty()) return false;
    bool parsedAny = false;
    for (size_t i = 0; i < regions.size(); ++i) {
        if (!regions[i].second) continue;
        snapshots[i].killerSecs = ExtractQueueSeconds(body, regions[i].first, "killer");
        snapshots[i].survivorSecs = ExtractQueueSeconds(body, regions[i].first, "survivor");
        parsedAny |= snapshots[i].killerSecs != METRIC_NA || snapshots[i].survivorSecs != METRIC_NA;
    }
    return parsedAny;
}

// ---------------------------------------------------------------------------
// Ping estimation. We can't ping the gamelift-ping hostnames themselves once
// the hosts file is redirecting most of them to 0.0.0.0 -- instead we ping
// each region's S3 endpoint (s3.<region>.amazonaws.com), which is a good
// real-world proxy for network latency to that AWS region and isn't touched
// by our hosts file edits. Tries a real ICMP echo first (most accurate);
// many cloud endpoints/firewalls drop ICMP though, so we fall back to timing
// a raw TCP connect to port 443, which almost always gets through.
// ---------------------------------------------------------------------------
static int IcmpPingAddress(const IN_ADDR& address, DWORD timeoutMs = 1200) {
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return -1;

    char sendData[32] = "dbdregionchanger";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    char replyBuf[sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8];

    DWORD ret = IcmpSendEcho(hIcmp, address.S_un.S_addr, sendData, sizeof(sendData),
        nullptr, replyBuf, replySize, timeoutMs);
    int rtt = -1;
    if (ret > 0) {
        PICMP_ECHO_REPLY reply = (PICMP_ECHO_REPLY)replyBuf;
        if (reply->Status == IP_SUCCESS) rtt = (int)reply->RoundTripTime;
    }
    IcmpCloseHandle(hIcmp);
    return rtt;
}

static int TcpConnectPingAddress(const IN_ADDR& ip, DWORD timeoutMs = 1200) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr = ip;
    address.sin_port = htons(443);

    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    connect(s, (const sockaddr*)&address, sizeof(address));

    fd_set writeSet, errSet;
    FD_ZERO(&writeSet); FD_SET(s, &writeSet);
    FD_ZERO(&errSet);   FD_SET(s, &errSet);
    timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;

    int rtt = -1;
    if (select(0, nullptr, &writeSet, &errSet, &tv) > 0 &&
        FD_ISSET(s, &writeSet) && !FD_ISSET(s, &errSet)) {
        QueryPerformanceCounter(&t1);
        rtt = (int)(((t1.QuadPart - t0.QuadPart) * 1000) / frequency.QuadPart);
    }
    closesocket(s);
    return rtt;
}

static int MeasurePing(const char* regionCode) {
    wchar_t host[128];
    swprintf_s(host, 128, L"s3.%hs.amazonaws.com", regionCode);
    ADDRINFOW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    if (GetAddrInfoW(host, nullptr, &hints, &res) != 0 || !res) return -1;

    IN_ADDR address = ((sockaddr_in*)res->ai_addr)->sin_addr;
    FreeAddrInfoW(res);

    // Best of two samples smooths out one-off spikes. Once ICMP fails, stay
    // on TCP so a filtered host doesn't cost an ICMP timeout per sample.
    int best = -1;
    bool useIcmp = true;
    for (int sample = 0; sample < 2 && !g_shuttingDown; ++sample) {
        int rtt = useIcmp ? IcmpPingAddress(address) : -1;
        if (rtt < 0) {
            useIcmp = false;
            rtt = TcpConnectPingAddress(address);
        }
        if (rtt >= 0 && (best < 0 || rtt < best)) best = rtt;
    }
    return best;
}

// toggle=true: clicking the same column flips direction (like Explorer);
// clicking a different column resets to ascending. toggle=false re-applies
// the column with the current direction (e.g. after new data arrived).
void SortByColumn(int col, bool toggle) {
    if (toggle) g_sortAscending = col != g_sortColumn || !g_sortAscending;
    g_sortColumn = col;

    auto lessThan = [col](int a, int b) {
        switch (col) {
        case 0:  return wcscmp(g_regions[a].label.c_str(), g_regions[b].label.c_str()) < 0;
        case 1:  return g_regions[a].killerSecs < g_regions[b].killerSecs;
        case 2:  return g_regions[a].survivorSecs < g_regions[b].survivorSecs;
        default: return g_regions[a].pingMs < g_regions[b].pingMs;
        }
    };
    bool asc = g_sortAscending;
    std::stable_sort(g_order.begin(), g_order.end(), [&](int a, int b) {
        if (col != 0 && g_regions[a].online != g_regions[b].online) return g_regions[a].online;
        if (col == 0) return asc ? lessThan(a, b) : lessThan(b, a);
        int aValue = col == 1 ? g_regions[a].killerSecs : col == 2 ? g_regions[a].survivorSecs : g_regions[a].pingMs;
        int bValue = col == 1 ? g_regions[b].killerSecs : col == 2 ? g_regions[b].survivorSecs : g_regions[b].pingMs;
        if (aValue >= METRIC_NA || bValue >= METRIC_NA) { // missing values sort last either way
            if (aValue != bValue) return aValue < bValue;
        } else if (aValue != bValue) {
            return asc ? aValue < bValue : aValue > bValue;
        }
        return wcscmp(g_regions[a].label.c_str(), g_regions[b].label.c_str()) < 0;
    });
}

// ---------------------------------------------------------------------------
// Refresh pipeline: discover regions -> fetch queue times + ping each online
// region. g_pendingFetches counts every outstanding result, so a new refresh
// (which may rebuild g_regions) can only start once all work has reported in.
// ---------------------------------------------------------------------------
struct QueueJob { HWND hwnd; RegionEntries regions; };
struct PingJob { HWND hwnd; int idx; std::string code; };

static bool SubmitWork(PTP_SIMPLE_CALLBACK callback, void* context) {
    if (!g_workGroup) {
        g_workGroup = CreateThreadpoolCleanupGroup();
        if (!g_workGroup) return false;
        InitializeThreadpoolEnvironment(&g_workEnv);
        SetThreadpoolCallbackCleanupGroup(&g_workEnv, g_workGroup, nullptr);
    }
    return TrySubmitThreadpoolCallback(callback, context, &g_workEnv) != FALSE;
}

static void CALLBACK DiscoverRegionsWork(PTP_CALLBACK_INSTANCE, void* context) {
    HWND hwnd = (HWND)context;
    RegionEntries* entries = new RegionEntries;
    if (DiscoverRegions(*entries)) {
        SaveRegionCache(*entries);
    } else {
        delete entries;
        entries = nullptr;
    }
    if (!PostMessageW(hwnd, WM_APP_REGIONS_DONE, 0, (LPARAM)entries)) delete entries;
}

static void CALLBACK FetchQueuesWork(PTP_CALLBACK_INSTANCE, void* context) {
    QueueJob* job = (QueueJob*)context;
    std::wstring url = std::wstring(QUEUES_URL) + L"?_=" + std::to_wstring(GetTickCount64());
    auto* snapshots = new std::vector<QueueSnapshot>(job->regions.size());
    if (!ParseQueueResponse(HttpGet(url.c_str()), job->regions, *snapshots)) {
        delete snapshots;
        snapshots = nullptr;
    }
    if (!PostMessageW(job->hwnd, WM_APP_QUEUE_DONE, 0, (LPARAM)snapshots)) delete snapshots;
    delete job;
}

static void CALLBACK PingWork(PTP_CALLBACK_INSTANCE, void* context) {
    PingJob* job = (PingJob*)context;
    int rtt = g_shuttingDown ? -1 : MeasurePing(job->code.c_str());
    PostMessageW(job->hwnd, WM_APP_PING_DONE, (WPARAM)job->idx, (LPARAM)rtt);
    delete job;
}

static void FinishFetch(HWND hwnd) {
    if (--g_pendingFetches > 0) return;
    g_refreshing = false;
    if (g_sortColumn != 0) { // rows were placed before their values arrived
        SortByColumn(g_sortColumn, false);
        PopulateList(g_hList);
    }
    KillTimer(hwnd, TIMER_SPINNER);
    EnableWindow(g_hBtnRefresh, TRUE);
    UpdateStatus();
}

// Rebuilds the region list from a fresh discovery. The pending-change
// baseline is re-read from the hosts file; the user's unapplied checkbox
// choices are kept for regions that still exist.
static void ApplyDiscoveredRegions(const RegionEntries& entries) {
    RegionEntries previous;
    for (int i = 0; i < REGION_COUNT && i < (int)g_unblockedRegions.size(); ++i) {
        previous.emplace_back(g_regions[i].code, g_unblockedRegions[i]);
    }
    BuildRegions(entries);
    DetectUnblockedRegions();
    g_originalUnblockedRegions = g_unblockedRegions;
    for (int i = 0; i < REGION_COUNT; ++i) {
        for (const auto& oldRegion : previous) {
            if (oldRegion.first == g_regions[i].code) {
                g_unblockedRegions[i] = oldRegion.second;
                break;
            }
        }
    }
    g_hasUnappliedChanges = g_unblockedRegions != g_originalUnblockedRegions;
    SortByColumn(g_sortColumn, false);
    PopulateList(g_hList);
    UpdateStatus();
}

void StartRefresh(HWND hwnd) {
    if (g_refreshing) return;
    g_refreshing = true;
    g_spinnerFrame = 0;
    g_completedFetches = 0;
    EnableWindow(g_hBtnRefresh, FALSE);
    SetTimer(hwnd, TIMER_SPINNER, SPINNER_INTERVAL_MS, nullptr);

    for (Region& region : g_regions) ResetRegionMetrics(region);
    PopulateList(g_hList);
    UpdateStatus();

    if (!g_internet) {
        g_internet = InternetOpenW(L"DBDRegionChanger/2.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                   nullptr, nullptr, 0);
        DWORD timeoutMs = 6000;
        if (g_internet) {
            InternetSetOptionW(g_internet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            InternetSetOptionW(g_internet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
            InternetSetOptionW(g_internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        }
    }

    g_pendingFetches = 1;
    if (!SubmitWork(DiscoverRegionsWork, hwnd)) OnRegionsDiscovered(hwnd, nullptr);
}

void OnRegionsDiscovered(HWND hwnd, RegionEntries* entries) {
    if (entries) {
        ApplyDiscoveredRegions(*entries);
        delete entries;
    }
    RegionEntries snapshot;
    for (const Region& region : g_regions) snapshot.emplace_back(region.code, region.online);

    if (!snapshot.empty()) {
        QueueJob* queueJob = new QueueJob{ hwnd, snapshot };
        ++g_pendingFetches;
        if (!SubmitWork(FetchQueuesWork, queueJob)) {
            delete queueJob;
            --g_pendingFetches;
        }
    }
    for (int idx = 0; idx < REGION_COUNT; ++idx) {
        if (!g_regions[idx].online) continue;
        PingJob* pingJob = new PingJob{ hwnd, idx, g_regions[idx].code };
        ++g_pendingFetches;
        if (!SubmitWork(PingWork, pingJob)) {
            delete pingJob;
            --g_pendingFetches;
        }
    }
    FinishFetch(hwnd); // the discovery step itself
}

void OnQueueDone(HWND hwnd, std::vector<QueueSnapshot>* snapshots) {
    bool valid = snapshots && (int)snapshots->size() == REGION_COUNT;
    for (int idx = 0; idx < REGION_COUNT; ++idx) {
        if (!g_regions[idx].online) continue;
        g_regions[idx].killerSecs = valid ? (*snapshots)[idx].killerSecs : METRIC_NA;
        g_regions[idx].survivorSecs = valid ? (*snapshots)[idx].survivorSecs : METRIC_NA;
        UpdateRegionRow(g_hList, idx);
    }
    delete snapshots;
    FinishFetch(hwnd);
}

void OnPingDone(HWND hwnd, int idx, int rtt) {
    if (idx >= 0 && idx < REGION_COUNT) {
        g_regions[idx].pingMs = rtt >= 0 ? rtt : METRIC_NA;
        UpdateRegionRow(g_hList, idx);
    }
    ++g_completedFetches;
    FinishFetch(hwnd);
}

// Aborts network requests and waits for all background work to finish.
void ShutdownRefresh() {
    g_shuttingDown = true;
    if (g_internet) InternetCloseHandle(g_internet);
    if (g_workGroup) {
        CloseThreadpoolCleanupGroupMembers(g_workGroup, FALSE, nullptr);
        CloseThreadpoolCleanupGroup(g_workGroup);
        DestroyThreadpoolEnvironment(&g_workEnv);
        g_workGroup = nullptr;
    }
    g_internet = nullptr;
}
